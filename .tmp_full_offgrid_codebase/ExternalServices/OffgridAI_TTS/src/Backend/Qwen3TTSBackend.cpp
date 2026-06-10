#include "Backend/Qwen3TTSBackend.h"

#include "Protocol/TTSProtocol.h"

#include "qwen3_tts.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <streambuf>
#include <iomanip>

namespace fs = std::filesystem;

namespace
{
    int32_t LanguageToId(const std::string& Language)
    {
        std::string L = Language;
        std::transform(L.begin(), L.end(), L.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
        if (L == "zh" || L == "chinese" || L == "mandarin") return 2055;
        if (L == "ja" || L == "japanese") return 2058;
        if (L == "ko" || L == "korean") return 2064;
        if (L == "de" || L == "german") return 2053;
        if (L == "fr" || L == "french") return 2061;
        if (L == "es" || L == "spanish") return 2054;
        if (L == "ru" || L == "russian") return 2069;
        return 2050;
    }

    int32_t ClampPositive(int32_t Value, int32_t Fallback)
    {
        return Value > 0 ? Value : Fallback;
    }

    bool FileExists(const std::string& Path)
    {
        std::error_code Ec;
        return !Path.empty() && fs::exists(fs::path(Path), Ec) && fs::is_regular_file(fs::path(Path), Ec);
    }

    bool DirectoryExists(const std::string& Path)
    {
        std::error_code Ec;
        return !Path.empty() && fs::exists(fs::path(Path), Ec) && fs::is_directory(fs::path(Path), Ec);
    }

    bool LooksLikeJsonFilePath(const std::string& Path)
    {
        if (Path.empty())
        {
            return false;
        }

        std::error_code Ec;
        const fs::path FsPath(Path);
        const std::string Extension = FsPath.extension().string();
        return Extension == ".json" || Extension == ".JSON";
    }

    std::string BoolText(bool bValue)
    {
        return bValue ? "true" : "false";
    }

    std::string ToLowerCopy(std::string Value)
    {
        std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
        return Value;
    }

    bool IsVoiceDesignRequest(const TTSRequest& Request)
    {
        const std::string Mode = ToLowerCopy(Request.VoiceMode);
        return Request.bVoiceDesign || Mode == "voice_design" || Mode == "voicedesign";
    }

    std::string SelectInstruction(const TTSRequest& Request)
    {
        if (!Request.VoiceDesignInstruction.empty()) return Request.VoiceDesignInstruction;
        if (!Request.Instruction.empty()) return Request.Instruction;
        return Request.bForwardEmotionToInstruction ? Request.Emotion : std::string();
    }


    class TeeStreamBuf final : public std::streambuf
    {
    public:
        TeeStreamBuf(std::streambuf* InConsole, std::streambuf* InFile)
            : Console(InConsole), File(InFile)
        {
        }

    protected:
        int overflow(int Ch) override
        {
            if (Ch == EOF)
            {
                return !EOF;
            }
            const int ConsoleResult = Console ? Console->sputc(static_cast<char>(Ch)) : Ch;
            const int FileResult = File ? File->sputc(static_cast<char>(Ch)) : Ch;
            return (ConsoleResult == EOF || FileResult == EOF) ? EOF : Ch;
        }

        int sync() override
        {
            const int ConsoleResult = Console ? Console->pubsync() : 0;
            const int FileResult = File ? File->pubsync() : 0;
            return (ConsoleResult == 0 && FileResult == 0) ? 0 : -1;
        }

    private:
        std::streambuf* Console = nullptr;
        std::streambuf* File = nullptr;
    };

    void InitializeFileLogging(const std::string& WorkingDirectory)
    {
        static std::once_flag Once;
        static std::ofstream LogFile;
        static std::unique_ptr<TeeStreamBuf> CoutTee;
        static std::unique_ptr<TeeStreamBuf> CerrTee;
        static std::streambuf* OriginalCout = nullptr;
        static std::streambuf* OriginalCerr = nullptr;

        std::call_once(Once, [&]()
        {
            std::error_code Ec;
            fs::path LogDirectory = WorkingDirectory.empty() ? fs::current_path(Ec) : fs::path(WorkingDirectory);
            if (Ec || LogDirectory.empty())
            {
                LogDirectory = fs::current_path();
            }
            fs::create_directories(LogDirectory, Ec);
            const fs::path LogPath = LogDirectory / "OffgridAI_TTS.log";
            LogFile.open(LogPath, std::ios::out | std::ios::trunc);
            if (!LogFile.is_open())
            {
                std::cerr << "[OffgridAI_TTS][logging][error] failed to open log file: " << LogPath.string() << std::endl;
                return;
            }

            OriginalCout = std::cout.rdbuf();
            OriginalCerr = std::cerr.rdbuf();
            CoutTee = std::make_unique<TeeStreamBuf>(OriginalCout, LogFile.rdbuf());
            CerrTee = std::make_unique<TeeStreamBuf>(OriginalCerr, LogFile.rdbuf());
            std::cout.rdbuf(CoutTee.get());
            std::cerr.rdbuf(CerrTee.get());

            std::cout << "[OffgridAI_TTS][logging] writing service log to " << LogPath.string() << std::endl;
        });
    }

    std::string MaybeFindFile(const std::string& Directory, const std::string& RequiredSubstring, const std::string& OptionalSubstring)
    {
        std::error_code Ec;
        if (!DirectoryExists(Directory)) return std::string();
        for (const auto& Entry : fs::directory_iterator(fs::path(Directory), Ec))
        {
            if (Ec || !Entry.is_regular_file()) continue;
            std::string Filename = Entry.path().filename().string();
            std::string Lower = Filename;
            std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
            std::string Required = RequiredSubstring;
            std::string Optional = OptionalSubstring;
            std::transform(Required.begin(), Required.end(), Required.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
            std::transform(Optional.begin(), Optional.end(), Optional.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
            if (Lower.find(".gguf") == std::string::npos) continue;
            if (!Required.empty() && Lower.find(Required) == std::string::npos) continue;
            if (!Optional.empty() && Lower.find(Optional) == std::string::npos) continue;
            return Entry.path().string();
        }
        return std::string();
    }
}

Qwen3TTSBackend::Qwen3TTSBackend()
    : Runtime(std::make_unique<qwen3_tts::Qwen3TTS>())
{
}

Qwen3TTSBackend::~Qwen3TTSBackend()
{
    TTSResponse Ignored;
    TTSRequest Request;
    Shutdown(Request, Ignored);
}

bool Qwen3TTSBackend::Startup(const TTSRequest& Request, TTSResponse& OutResponse)
{
    OutResponse.RequestId = Request.RequestId;
    InitializeFileLogging(Request.ServiceWorkingDirectory);

    std::lock_guard<std::mutex> Lock(RuntimeMutex);

    const std::string ModelDirectory = ResolvePath(Request.ModelDirectory.empty() ? Request.ModelIdentifier : Request.ModelDirectory,
                                                   Request.ServiceWorkingDirectory);
    const std::string ModelIdentifier = Request.ModelDirectory.empty() ? std::string() : Request.ModelIdentifier;
    if (ModelDirectory.empty())
    {
        OutResponse.Ok = false;
        OutResponse.ErrorMessage = "Native Qwen3 TTS startup failed: model_directory/model_identifier was empty.";
        LastError = OutResponse.ErrorMessage;
        bLoaded = false;
        std::cerr << "[OffgridAI_TTS][startup][error] " << LastError << std::endl;
        return true;
    }

    if (!DirectoryExists(ModelDirectory))
    {
        OutResponse.Ok = false;
        OutResponse.ErrorMessage = "Native Qwen3 TTS startup failed: model directory does not exist: " + ModelDirectory;
        LastError = OutResponse.ErrorMessage;
        bLoaded = false;
        std::cerr << "[OffgridAI_TTS][startup][error] " << LastError << std::endl;
        return true;
    }

    const std::string ExpectedTokenizer = MaybeFindFile(ModelDirectory, "tokenizer", "");
    const std::string ExpectedBase = ModelIdentifier.empty()
        ? MaybeFindFile(ModelDirectory, "base", "")
        : (FileExists((fs::path(ModelDirectory) / ModelIdentifier).string())
            ? (fs::path(ModelDirectory) / ModelIdentifier).string()
            : MaybeFindFile(ModelDirectory, ModelIdentifier, ""));
    const std::string ExpectedCustomVoice = MaybeFindFile(ModelDirectory, "customvoice", "");

    // Normalize the user-facing ModelIdentifier before passing it to qwen. The
    // qwen loader treats a non-empty identifier as a concrete model filename or
    // path relative to ModelDirectory; aliases like "Base" must therefore be
    // resolved to the actual GGUF filename first.
    const std::string EffectiveModelIdentifier = ModelIdentifier.empty()
        ? std::string()
        : (!ExpectedBase.empty() ? fs::path(ExpectedBase).filename().string() : ModelIdentifier);

    std::cout << "[OffgridAI_TTS][startup] request_id=" << Request.RequestId << std::endl;
    std::cout << "[OffgridAI_TTS][startup] model_directory=" << ModelDirectory << std::endl;
    std::cout << "[OffgridAI_TTS][startup] model_identifier=" << (ModelIdentifier.empty() ? "<auto>" : ModelIdentifier)
              << " effective_model_identifier=" << (EffectiveModelIdentifier.empty() ? "<auto>" : EffectiveModelIdentifier) << std::endl;
    std::cout << "[OffgridAI_TTS][startup] detected_tts_gguf=" << (ExpectedBase.empty() ? "<not-found>" : ExpectedBase) << std::endl;
    std::cout << "[OffgridAI_TTS][startup] detected_customvoice_gguf=" << (ExpectedCustomVoice.empty() ? "<not-found>" : ExpectedCustomVoice) << std::endl;
    std::cout << "[OffgridAI_TTS][startup] detected_tokenizer_gguf=" << (ExpectedTokenizer.empty() ? "<not-found>" : ExpectedTokenizer) << std::endl;
    std::cout << "[OffgridAI_TTS][startup] use_gpu=" << BoolText(Request.bUseGPU)
              << " prewarm=" << BoolText(Request.bPrewarmStreaming)
              << " async_decode=" << BoolText(Request.bAsyncStreamingDecode)
              << " first_tail=" << ClampPositive(Request.FirstTailWindowFrames, 1)
              << " steady_tail=" << ClampPositive(Request.SteadyTailWindowFrames, 8)
              << " context=" << ClampPositive(Request.ContextFrames, 4)
              << " final_context=" << ClampPositive(Request.FinalContextFrames, 4)
              << " prewarm_decode_shapes=" << BoolText(Request.bPrewarmFirstDecode) << "/"
              << BoolText(Request.bPrewarmSteadyDecode) << "/"
              << BoolText(Request.bPrewarmFinalDecode) << std::endl;

    if (ExpectedTokenizer.empty())
    {
        OutResponse.Ok = false;
        OutResponse.ErrorMessage = "Native Qwen3 TTS startup failed: no tokenizer GGUF found in model directory: " + ModelDirectory;
        LastError = OutResponse.ErrorMessage;
        bLoaded = false;
        std::cerr << "[OffgridAI_TTS][startup][error] " << LastError << std::endl;
        return true;
    }

    if (!ModelIdentifier.empty() && ExpectedBase.empty())
    {
        OutResponse.Ok = false;
        OutResponse.ErrorMessage = "Native Qwen3 TTS startup failed: model_identifier did not resolve to a GGUF in model directory. model_identifier=" + ModelIdentifier + " model_directory=" + ModelDirectory;
        LastError = OutResponse.ErrorMessage;
        bLoaded = false;
        std::cerr << "[OffgridAI_TTS][startup][error] " << LastError << std::endl;
        return true;
    }

    if (!Runtime)
    {
        Runtime = std::make_unique<qwen3_tts::Qwen3TTS>();
    }

    if (!bLoaded || LoadedModelDirectory != ModelDirectory || LoadedModelIdentifier != EffectiveModelIdentifier)
    {
        std::cout << "[OffgridAI_TTS][startup] loading native Qwen3 TTS models with identifier="
                  << (EffectiveModelIdentifier.empty() ? "<auto>" : EffectiveModelIdentifier) << std::endl;
        if (!Runtime->load_models(ModelDirectory, EffectiveModelIdentifier))
        {
            OutResponse.Ok = false;
            OutResponse.ErrorMessage = "Native Qwen3 TTS model load failed: " + Runtime->get_error();
            LastError = OutResponse.ErrorMessage;
            bLoaded = false;
            bStartupPrewarmed = false;
            std::cerr << "[OffgridAI_TTS][startup][error] " << LastError << std::endl;
            return true;
        }
        LoadedModelDirectory = ModelDirectory;
        LoadedModelIdentifier = EffectiveModelIdentifier;
        bStartupPrewarmed = false;
        std::cout << "[OffgridAI_TTS][startup] model load complete." << std::endl;
        const auto Caps = Runtime->get_model_capabilities();
        std::cout << "[OffgridAI_TTS][startup] model_caps"
                  << " type=" << (Caps.model_type.empty() ? "<empty>" : Caps.model_type)
                  << " voice_clone=" << BoolText(Caps.supports_voice_clone)
                  << " named_speakers=" << BoolText(Caps.supports_named_speakers)
                  << " instruction=" << BoolText(Caps.supports_instruction)
                  << " speaker_dim=" << Caps.speaker_embedding_dim
                  << " speaker_count=" << Caps.speaker_count << std::endl;
        const auto Speakers = Runtime->get_available_speakers();
        if (!Speakers.empty())
        {
            std::cout << "[OffgridAI_TTS][startup] available_speakers=";
            for (size_t Index = 0; Index < Speakers.size(); ++Index)
            {
                if (Index > 0) std::cout << ",";
                std::cout << Speakers[Index];
            }
            std::cout << std::endl;
        }
    }
    else
    {
        std::cout << "[OffgridAI_TTS][startup] model already loaded; reusing runtime." << std::endl;
    }

    SpeakerEmbedding.clear();
    LoadedSpeakerEmbeddingPath.clear();
    bStartupPrewarmed = false;
    const bool bVoiceDesignStartup = IsVoiceDesignRequest(Request);
    const std::string SpeakerPath = bVoiceDesignStartup ? std::string() : ResolvePath(Request.SpeakerEmbeddingPath, Request.ServiceWorkingDirectory);
    if (bVoiceDesignStartup)
    {
        std::cout << "[OffgridAI_TTS][startup] VoiceDesign mode: speaker embeddings/reference audio are disabled." << std::endl;
    }
    if (!SpeakerPath.empty())
    {
        if (DirectoryExists(SpeakerPath))
        {
            std::cout << "[OffgridAI_TTS][startup] speaker_embedding_path is a directory; per-request voice_id filenames will be resolved under "
                      << SpeakerPath << std::endl;
        }
        else
        {
            std::cout << "[OffgridAI_TTS][startup] speaker_embedding_path=" << SpeakerPath << std::endl;
            std::string Error;
            if (!LoadSpeakerEmbedding(SpeakerPath, Error))
            {
                OutResponse.Ok = false;
                OutResponse.ErrorMessage = Error;
                LastError = Error;
                bLoaded = false;
                std::cerr << "[OffgridAI_TTS][startup][error] " << LastError << std::endl;
                return true;
            }
            LoadedSpeakerEmbeddingPath = SpeakerPath;
            std::cout << "[OffgridAI_TTS][startup] speaker embedding loaded floats=" << SpeakerEmbedding.size() << std::endl;
        }
    }
    else
    {
        std::cout << "[OffgridAI_TTS][startup] no speaker embedding path supplied; using model default/named speaker/reference-audio path." << std::endl;
    }

    bLoaded = true;
    bShuttingDown = false;

    if (Request.bPrewarmStreaming && !bStartupPrewarmed)
    {
        std::string PrewarmError;
        if (!RunStartupPrewarmLocked(Request, PrewarmError))
        {
            OutResponse.Ok = false;
            OutResponse.ErrorMessage = PrewarmError.empty() ? "Native Qwen3 TTS startup prewarm failed." : PrewarmError;
            LastError = OutResponse.ErrorMessage;
            bLoaded = false;
            bStartupPrewarmed = false;
            std::cerr << "[OffgridAI_TTS][startup][prewarm][error] " << LastError << std::endl;
            return true;
        }
        bStartupPrewarmed = true;
    }
    else if (!Request.bPrewarmStreaming)
    {
        std::cout << "[OffgridAI_TTS][startup][prewarm] skipped by settings; per-request prewarm remains disabled for latency unless explicitly re-enabled in code." << std::endl;
    }

    LastError.clear();
    OutResponse.Ok = true;
    return true;
}

bool Qwen3TTSBackend::BeginSynthesis(const TTSRequest& Request, TTSResponse& OutResponse)
{
    const auto BeginWall = std::chrono::steady_clock::now();
    OutResponse.RequestId = Request.RequestId;

    std::cout << "[OffgridAI_TTS][begin] received request_id=" << Request.RequestId
              << " conversation=" << Request.ConversationId
              << " npc=" << Request.NPCId
              << " line=" << Request.LineId
              << " text_chars=" << Request.Dialogue.size() << std::endl;

    if (!bLoaded)
    {
        OutResponse.Ok = false;
        OutResponse.ErrorMessage = LastError.empty() ? "Native Qwen3 TTS is not loaded." : LastError;
        std::cerr << "[OffgridAI_TTS][begin][error] " << OutResponse.ErrorMessage << std::endl;
        return true;
    }

    if (Request.Dialogue.empty())
    {
        OutResponse.Ok = false;
        OutResponse.ErrorMessage = "Native Qwen3 TTS begin_synthesis failed: dialogue was empty.";
        std::cerr << "[OffgridAI_TTS][begin][error] " << OutResponse.ErrorMessage << std::endl;
        return true;
    }

    StopActiveSynthesis();
    bCancelRequested = false;
    const uint64_t Generation = ++ActiveGeneration;

    TTSEvent Started;
    Started.Type = ETTSEventType::StreamStarted;
    Started.RequestId = Request.RequestId;
    Started.ConversationId = Request.ConversationId;
    Started.NPCId = Request.NPCId;
    Started.LineId = Request.LineId;
    Started.SampleRate = Request.OutputSampleRate > 0 ? Request.OutputSampleRate : 24000;
    Started.NumChannels = 1;
    EnqueueEvent(std::move(Started));
    std::cout << "[OffgridAI_TTS][begin] enqueued stream_started request_id=" << Request.RequestId << std::endl;

    ActiveThread = std::thread(&Qwen3TTSBackend::SynthesisThreadMain, this, Request, Generation);
    const auto EndWall = std::chrono::steady_clock::now();
    const auto ReturnMs = std::chrono::duration_cast<std::chrono::milliseconds>(EndWall - BeginWall).count();
    std::cout << "[OffgridAI_TTS][begin] worker thread launched request_id=" << Request.RequestId
              << " generation=" << Generation
              << " begin_response_ms=" << ReturnMs << std::endl;
    OutResponse.Ok = true;
    return true;
}

bool Qwen3TTSBackend::Cancel(const TTSRequest& Request, TTSResponse& OutResponse)
{
    OutResponse.RequestId = Request.RequestId;
    StopActiveSynthesis();
    OutResponse.Ok = true;
    return true;
}

bool Qwen3TTSBackend::PollEvent(const TTSRequest& Request, TTSResponse& OutResponse)
{
    OutResponse.RequestId = Request.RequestId;
    OutResponse.Ok = true;

    std::lock_guard<std::mutex> Lock(EventMutex);
    if (PendingEvents.empty())
    {
        if (bHasDeferredCompletedEvent)
        {
            OutResponse.HasEvent = true;
            OutResponse.Event = std::move(DeferredCompletedEvent);
            bHasDeferredCompletedEvent = false;
            DeferredCompletedEvent = TTSEvent();
        }
        else
        {
            OutResponse.HasEvent = false;
            return true;
        }
    }
    else
    {
        OutResponse.HasEvent = true;
        OutResponse.Event = std::move(PendingEvents.front());
        PendingEvents.pop();
    }
    if (OutResponse.Event.Type == ETTSEventType::AudioChunk)
    {
        std::cout << "[OffgridAI_TTS][poll] event=audio_chunk request_id=" << OutResponse.Event.RequestId
                  << " bytes=" << OutResponse.Event.PCMChunk.size()
                  << " sr=" << OutResponse.Event.SampleRate << std::endl;
    }
    else if (OutResponse.Event.Type == ETTSEventType::Error)
    {
        std::cerr << "[OffgridAI_TTS][poll] event=error request_id=" << OutResponse.Event.RequestId
                  << " error=" << OutResponse.Event.ErrorMessage << std::endl;
    }
    else
    {
        std::cout << "[OffgridAI_TTS][poll] event=" << TTSProtocol::EventTypeToString(OutResponse.Event.Type)
                  << " request_id=" << OutResponse.Event.RequestId << std::endl;
    }
    return true;
}

bool Qwen3TTSBackend::Health(const TTSRequest& Request, TTSResponse& OutResponse)
{
    OutResponse.RequestId = Request.RequestId;
    OutResponse.Ok = bLoaded && Runtime != nullptr && Runtime->is_loaded() && !bShuttingDown;
    if (!OutResponse.Ok)
    {
        OutResponse.ErrorMessage = LastError.empty() ? "Native Qwen3 TTS is not healthy." : LastError;
    }
    return true;
}

bool Qwen3TTSBackend::Shutdown(const TTSRequest& Request, TTSResponse& OutResponse)
{
    OutResponse.RequestId = Request.RequestId;
    bShuttingDown = true;
    StopActiveSynthesis();
    {
        std::lock_guard<std::mutex> Lock(EventMutex);
        while (!PendingEvents.empty()) PendingEvents.pop();
        bHasDeferredCompletedEvent = false;
        DeferredCompletedEvent = TTSEvent();
    }
    OutResponse.Ok = true;
    return true;
}

void Qwen3TTSBackend::StopActiveSynthesis()
{
    bCancelRequested = true;
    if (ActiveThread.joinable())
    {
        ActiveThread.join();
    }
}

void Qwen3TTSBackend::SynthesisThreadMain(TTSRequest Request, uint64_t Generation)
{
    const auto StartTime = std::chrono::steady_clock::now();
    const bool bVoiceDesign = IsVoiceDesignRequest(Request);
    const std::string EffectiveInstruction = SelectInstruction(Request);
    std::vector<float> RequestSpeakerEmbedding;
    std::string RequestSpeakerEmbeddingPath = bVoiceDesign ? std::string() : ResolveSpeakerEmbeddingPathForRequest(Request);
    if (bVoiceDesign && EffectiveInstruction.empty())
    {
        EnqueueError(Request, "VoiceDesign synthesis requires a non-empty instruction or voice_design_instruction.");
        return;
    }
    if (!RequestSpeakerEmbeddingPath.empty())
    {
        std::string Error;
        if (!LoadSpeakerEmbeddingFile(RequestSpeakerEmbeddingPath, RequestSpeakerEmbedding, Error))
        {
            EnqueueError(Request, Error);
            return;
        }
    }
    else if (!bVoiceDesign && !SpeakerEmbedding.empty())
    {
        RequestSpeakerEmbedding = SpeakerEmbedding;
        RequestSpeakerEmbeddingPath = LoadedSpeakerEmbeddingPath;
    }

    std::cout << "[OffgridAI_TTS][synth] start request_id=" << Request.RequestId
              << " generation=" << Generation
              << " speaker_embedding=" << (RequestSpeakerEmbedding.empty() ? "none" : RequestSpeakerEmbeddingPath)
              << " reference_audio=" << (Request.ReferenceAudioPath.empty() ? "none" : Request.ReferenceAudioPath)
              << " voice_id=" << (Request.VoiceId.empty() ? "<empty>" : Request.VoiceId)
              << " voice_mode=" << (Request.VoiceMode.empty() ? "<empty>" : Request.VoiceMode)
              << " voice_design=" << BoolText(bVoiceDesign)
              << " emotion=" << (Request.Emotion.empty() ? "<empty>" : Request.Emotion)
              << " instruction_len=" << EffectiveInstruction.size()
              << " forward_emotion_to_instruction=" << BoolText(Request.bForwardEmotionToInstruction) << std::endl;

    qwen3_tts::tts_params Params;
    Params.max_audio_tokens = ClampPositive(Request.MaxAudioTokens, 4096);
    Params.temperature = Request.Temperature;
    Params.top_p = Request.TopP;
    Params.top_k = Request.TopK;
    Params.n_threads = 4;
    Params.print_progress = Request.bVerboseLogging;
    Params.print_timing = Request.bVerboseLogging;
    Params.repetition_penalty = Request.RepetitionPenalty;
    Params.language_id = LanguageToId(Request.Language);
    Params.instruction = EffectiveInstruction;
    // VoiceDesign is instruction-only: never pass named speakers, reference audio, or speaker embeddings.
    Params.speaker = (bVoiceDesign || !RequestSpeakerEmbedding.empty() || !Request.ReferenceAudioPath.empty())
        ? std::string()
        : Request.VoiceId;
    Params.streaming_generate = true;
    Params.first_tail_window_frames = ClampPositive(Request.FirstTailWindowFrames, 1);
    Params.steady_tail_window_frames = ClampPositive(Request.SteadyTailWindowFrames, 8);
    Params.context_frames = ClampPositive(Request.ContextFrames, 4);
    Params.final_context_frames = ClampPositive(Request.FinalContextFrames, 4);
    // Match the working streaming CLI for generation/decode scheduling. Unreal owns
    // audio playback, so play_streaming remains false, but async decode must stay on
    // for VoiceDesign to avoid blocking autoregressive generation behind vocoder work.
    Params.async_streaming_decode = bVoiceDesign ? true : Request.bAsyncStreamingDecode;
    Params.play_streaming = false; // Unreal LineCoach owns playback.
    // Startup prewarm owns Qwen graph/model warmup. Only fall back to per-request
    // prewarm when startup skipped it; never pay the prewarm cost on every line.
    Params.prewarm_streaming = Request.bPrewarmStreaming && !bStartupPrewarmed;
    Params.prewarm_frames = ClampPositive(Request.PrewarmFrames, 1);
    Params.prewarm_repeats = ClampPositive(Request.PrewarmRepeats, 1);
    Params.prewarm_first_decode = Request.bPrewarmFirstDecode;
    Params.prewarm_steady_decode = Request.bPrewarmSteadyDecode;
    Params.prewarm_final_decode = Request.bPrewarmFinalDecode;
    Params.dump_first_frame_profile = Request.bDumpFirstFrameProfile;

    std::cout << "[OffgridAI_TTS][synth] request params request_id=" << Request.RequestId
              << " instruction=" << (Params.instruction.empty() ? "<empty>" : Params.instruction)
              << " named_speaker=" << (Params.speaker.empty() ? "<empty>" : Params.speaker)
              << " per_request_prewarm=" << BoolText(Params.prewarm_streaming)
              << " startup_prewarmed=" << BoolText(bStartupPrewarmed)
              << " first_tail=" << Params.first_tail_window_frames
              << " steady_tail=" << Params.steady_tail_window_frames
              << " context=" << Params.context_frames
              << " final_context=" << Params.final_context_frames
              << " async_decode=" << BoolText(Params.async_streaming_decode) << std::endl;

    auto FirstChunkSeen = std::make_shared<std::atomic<bool>>(false);
    auto CallbackSampleCount = std::make_shared<std::atomic<size_t>>(0);
    auto TransportChunkCount = std::make_shared<std::atomic<uint64_t>>(0);
    auto TransportByteCount = std::make_shared<std::atomic<uint64_t>>(0);
    auto TransportSampleCursor = std::make_shared<std::atomic<int64_t>>(0);
    auto LastTransportChunkMs = std::make_shared<std::atomic<int64_t>>(-1);
    auto MaxTransportGapMs = std::make_shared<std::atomic<int64_t>>(0);
    int32_t AudioChunkSampleRate = Request.OutputSampleRate > 0 ? Request.OutputSampleRate : 24000;

    // Keep the service bridge deliberately thin. qwen3-tts-cpp-streaming already
    // owns the streaming schedule: tail windows, async decode, context overlap,
    // and first-PCM timing. Previous Offgrid service revisions added a second
    // paced emitter, service-side lead buffers, and audio caps in this hot path;
    // those were integration mistakes that delayed and stalled an otherwise
    // working faster-than-realtime streaming engine. The callback below only
    // converts the newly decoded contiguous float samples to PCM16 and enqueues
    // one transport event immediately. No sleeping, pacing, accumulating, caps,
    // or chunk-reordering are allowed here.
    auto EnqueuePCM16Immediate = [this, &AudioChunkSampleRate, &Request, StartTime, TransportChunkCount, TransportByteCount, TransportSampleCursor, LastTransportChunkMs, MaxTransportGapMs](std::vector<uint8_t>&& PCMBytes)
    {
        if (PCMBytes.empty())
        {
            return;
        }

        const uint64_t ChunkSeq = TransportChunkCount->fetch_add(1);
        TransportByteCount->fetch_add(static_cast<uint64_t>(PCMBytes.size()));
        const int32_t BytesPerSampleFrame = static_cast<int32_t>(sizeof(int16_t));
        const int32_t ChunkSamples = static_cast<int32_t>(PCMBytes.size() / static_cast<size_t>(BytesPerSampleFrame));
        const int64_t ChunkStartSample = TransportSampleCursor->fetch_add(static_cast<int64_t>(ChunkSamples));

        TTSEvent Event;
        Event.Type = ETTSEventType::AudioChunk;
        Event.RequestId = Request.RequestId;
        Event.ConversationId = Request.ConversationId;
        Event.NPCId = Request.NPCId;
        Event.LineId = Request.LineId;
        Event.SampleRate = AudioChunkSampleRate > 0 ? AudioChunkSampleRate : 24000;
        Event.NumChannels = 1;
        Event.ChunkStartSample = ChunkStartSample;
        Event.ChunkSampleCount = ChunkSamples;
        Event.StreamTotalSamples = ChunkStartSample + ChunkSamples;
        Event.PCMChunk = std::move(PCMBytes);

        const auto Now = std::chrono::steady_clock::now();
        const int64_t WallMs = std::chrono::duration_cast<std::chrono::milliseconds>(Now - StartTime).count();
        const int64_t PreviousMs = LastTransportChunkMs->exchange(WallMs);
        const int64_t GapMs = PreviousMs >= 0 ? (WallMs - PreviousMs) : 0;
        if (GapMs > MaxTransportGapMs->load())
        {
            MaxTransportGapMs->store(GapMs);
        }

        if (ChunkSeq < 4 || GapMs > 120 || Request.bVerboseLogging)
        {
            const double ChunkMs = (Event.SampleRate > 0)
                ? (static_cast<double>(Event.ChunkSampleCount) / static_cast<double>(Event.SampleRate) * 1000.0)
                : 0.0;
            const double StreamMs = (Event.SampleRate > 0)
                ? (static_cast<double>(Event.StreamTotalSamples) / static_cast<double>(Event.SampleRate) * 1000.0)
                : 0.0;
            std::cout << "[OffgridAI_TTS][stream] direct_chunk request_id=" << Event.RequestId
                      << " seq=" << ChunkSeq
                      << " wall_ms=" << WallMs
                      << " gap_ms=" << GapMs
                      << " start_sample=" << Event.ChunkStartSample
                      << " samples=" << Event.ChunkSampleCount
                      << " chunk_ms=" << std::fixed << std::setprecision(1) << ChunkMs
                      << " stream_ms=" << StreamMs << std::defaultfloat
                      << " bytes=" << Event.PCMChunk.size()
                      << " sr=" << Event.SampleRate << std::endl;
        }

        EnqueueEvent(std::move(Event));
    };

    Params.streaming_audio_sink = [this, Request, Generation, StartTime, FirstChunkSeen, CallbackSampleCount, &AudioChunkSampleRate, &EnqueuePCM16Immediate](const float* Samples, size_t NumSamples, int32_t SampleRate) -> bool
    {
        if (bCancelRequested || ActiveGeneration.load() != Generation)
        {
            return false;
        }
        if (!Samples || NumSamples == 0)
        {
            return true;
        }

        AudioChunkSampleRate = SampleRate > 0 ? SampleRate : 24000;
        CallbackSampleCount->fetch_add(NumSamples);

        bool Expected = false;
        if (FirstChunkSeen->compare_exchange_strong(Expected, true))
        {
            const auto Now = std::chrono::steady_clock::now();
            const auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(Now - StartTime).count();
            std::cout << "[OffgridAI_TTS][synth] first_pcm_callback request_id=" << Request.RequestId
                      << " samples=" << NumSamples
                      << " sr=" << AudioChunkSampleRate
                      << " first_pcm_ms=" << Ms
                      << " transport=direct_qwen_stream" << std::endl;
        }

        EnqueuePCM16Immediate(FloatMonoToPCM16(Samples, NumSamples));
        return true;
    };

    qwen3_tts::tts_result Result;
    {
        std::cout << "[OffgridAI_TTS][synth] entering qwen runtime request_id=" << Request.RequestId << std::endl;
        std::lock_guard<std::mutex> Lock(RuntimeMutex);
        if (bVoiceDesign)
        {
            Result = Runtime->synthesize(Request.Dialogue, Params);
        }
        else if (!RequestSpeakerEmbedding.empty())
        {
            Result = Runtime->synthesize_with_speaker_embedding(Request.Dialogue, RequestSpeakerEmbedding, Params);
        }
        else if (!Request.ReferenceAudioPath.empty())
        {
            const std::string RefAudio = ResolvePath(Request.ReferenceAudioPath, Request.ServiceWorkingDirectory);
            Result = Runtime->synthesize_with_voice(Request.Dialogue, RefAudio, Params);
        }
        else
        {
            Result = Runtime->synthesize(Request.Dialogue, Params);
        }
    }

    {
        const auto Now = std::chrono::steady_clock::now();
        const auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(Now - StartTime).count();
        std::cout << "[OffgridAI_TTS][synth] qwen runtime returned request_id=" << Request.RequestId
                  << " success=" << BoolText(Result.success)
                  << " audio_samples=" << Result.audio.size()
                  << " elapsed_ms=" << Ms
                  << " t_generate_ms=" << Result.t_generate_ms
                  << " t_decode_ms=" << Result.t_decode_ms
                  << " t_total_ms=" << Result.t_total_ms
                  << " error=" << (Result.error_msg.empty() ? "<none>" : Result.error_msg) << std::endl;
    }

    const size_t CallbackSamples = CallbackSampleCount->load();
    if (Result.success && Result.audio.size() > CallbackSamples)
    {
        const size_t MissingSamples = Result.audio.size() - CallbackSamples;
        std::cout << "[OffgridAI_TTS][synth] direct_stream final_tail request_id=" << Request.RequestId
                  << " callback_samples=" << CallbackSamples
                  << " result_samples=" << Result.audio.size()
                  << " missing_samples=" << MissingSamples
                  << " transport_chunks_before_tail=" << TransportChunkCount->load() << std::endl;
        EnqueuePCM16Immediate(FloatMonoToPCM16(Result.audio.data() + CallbackSamples, MissingSamples));
    }
    else
    {
        std::cout << "[OffgridAI_TTS][synth] direct_stream coverage request_id=" << Request.RequestId
                  << " callback_samples=" << CallbackSamples
                  << " result_samples=" << Result.audio.size()
                  << " transport_chunks=" << TransportChunkCount->load()
                  << " transport_bytes=" << TransportByteCount->load()
                  << " max_transport_gap_ms=" << MaxTransportGapMs->load() << std::endl;
    }

    if (ActiveGeneration.load() != Generation)
    {
        return;
    }

    if (bCancelRequested)
    {
        TTSEvent Completed;
        Completed.Type = ETTSEventType::Completed;
        Completed.RequestId = Request.RequestId;
        Completed.ConversationId = Request.ConversationId;
        Completed.NPCId = Request.NPCId;
        Completed.LineId = Request.LineId;
        Completed.SampleRate = Request.OutputSampleRate > 0 ? Request.OutputSampleRate : 24000;
        Completed.NumChannels = 1;
        Completed.StreamTotalSamples = static_cast<int64_t>(TransportSampleCursor->load());
        EnqueueCompleted(std::move(Completed));
        return;
    }

    if (!Result.success)
    {
        const std::string Error = Result.error_msg.empty() ? "Native Qwen3 synthesis failed." : Result.error_msg;
        std::cerr << "[OffgridAI_TTS][synth][error] request_id=" << Request.RequestId << " error=" << Error << std::endl;
        EnqueueError(Request, Error);
        return;
    }

    TTSEvent Completed;
    Completed.Type = ETTSEventType::Completed;
    Completed.RequestId = Request.RequestId;
    Completed.ConversationId = Request.ConversationId;
    Completed.NPCId = Request.NPCId;
    Completed.LineId = Request.LineId;
    Completed.SampleRate = Result.sample_rate > 0 ? Result.sample_rate : 24000;
    Completed.NumChannels = 1;
    Completed.StreamTotalSamples = static_cast<int64_t>(TransportSampleCursor->load());
    EnqueueCompleted(std::move(Completed));
    {
        const auto Now = std::chrono::steady_clock::now();
        const auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(Now - StartTime).count();
        std::cout << "[OffgridAI_TTS][synth] completed request_id=" << Request.RequestId
                  << " total_ms=" << Ms << std::endl;
    }
}

void Qwen3TTSBackend::EnqueueEvent(TTSEvent&& Event)
{
    std::lock_guard<std::mutex> Lock(EventMutex);
    PendingEvents.push(std::move(Event));
}

void Qwen3TTSBackend::EnqueueCompleted(TTSEvent&& Event)
{
    std::lock_guard<std::mutex> Lock(EventMutex);
    // Completion is a normal FIFO event. Qwen streaming callbacks enqueue PCM chunks
    // immediately as they are decoded; any final non-callback tail is synchronously
    // enqueued before completion.
    PendingEvents.push(std::move(Event));
    bHasDeferredCompletedEvent = false;
    DeferredCompletedEvent = TTSEvent();
}

void Qwen3TTSBackend::EnqueueError(const TTSRequest& Request, const std::string& ErrorMessage)
{
    std::cerr << "[OffgridAI_TTS][event] enqueue error request_id=" << Request.RequestId
              << " error=" << ErrorMessage << std::endl;
    TTSEvent Event;
    Event.Type = ETTSEventType::Error;
    Event.RequestId = Request.RequestId;
    Event.ConversationId = Request.ConversationId;
    Event.NPCId = Request.NPCId;
    Event.LineId = Request.LineId;
    Event.SampleRate = Request.OutputSampleRate > 0 ? Request.OutputSampleRate : 24000;
    Event.NumChannels = 1;
    Event.ErrorMessage = ErrorMessage;
    EnqueueEvent(std::move(Event));
}

bool Qwen3TTSBackend::RunStartupPrewarmLocked(const TTSRequest& Request, std::string& OutError)
{
    if (!Runtime || !Runtime->is_loaded())
    {
        OutError = "Native Qwen3 TTS startup prewarm failed: runtime was not loaded.";
        return false;
    }

    const auto StartTime = std::chrono::steady_clock::now();
    const bool bVoiceDesign = IsVoiceDesignRequest(Request);
    const std::string EffectiveInstruction = SelectInstruction(Request);
    std::cout << "[OffgridAI_TTS][startup][prewarm] begin"
              << " voice_design=" << BoolText(bVoiceDesign)
              << " speaker_embedding=" << ((!bVoiceDesign && !SpeakerEmbedding.empty()) ? "loaded" : "none")
              << " reference_audio=" << ((bVoiceDesign || Request.ReferenceAudioPath.empty()) ? "none" : Request.ReferenceAudioPath)
              << " frames=" << ClampPositive(Request.PrewarmFrames, 1)
              << " repeats=" << ClampPositive(Request.PrewarmRepeats, 1)
              << " first_decode=" << BoolText(Request.bPrewarmFirstDecode)
              << " steady_decode=" << BoolText(Request.bPrewarmSteadyDecode)
              << " final_decode=" << BoolText(Request.bPrewarmFinalDecode)
              << std::endl;

    qwen3_tts::tts_params Params;
    Params.max_audio_tokens = 64;
    Params.temperature = Request.Temperature;
    Params.top_p = Request.TopP;
    Params.top_k = Request.TopK;
    Params.n_threads = 4;
    Params.print_progress = false;
    Params.print_timing = Request.bVerboseLogging;
    Params.repetition_penalty = Request.RepetitionPenalty;
    Params.language_id = LanguageToId(Request.Language);
    Params.instruction = bVoiceDesign ? (EffectiveInstruction.empty() ? "Voice identity: natural conversational adult voice. Delivery: neutral." : EffectiveInstruction) : std::string();
    Params.speaker = (bVoiceDesign || !SpeakerEmbedding.empty() || !Request.ReferenceAudioPath.empty())
        ? std::string()
        : Request.VoiceId;
    Params.streaming_generate = true;
    Params.first_tail_window_frames = ClampPositive(Request.FirstTailWindowFrames, 1);
    Params.steady_tail_window_frames = ClampPositive(Request.SteadyTailWindowFrames, 8);
    Params.context_frames = ClampPositive(Request.ContextFrames, 4);
    Params.final_context_frames = ClampPositive(Request.FinalContextFrames, 4);
    Params.async_streaming_decode = Request.bAsyncStreamingDecode;
    Params.play_streaming = false;
    Params.prewarm_streaming = true;
    Params.prewarm_frames = ClampPositive(Request.PrewarmFrames, 1);
    Params.prewarm_repeats = ClampPositive(Request.PrewarmRepeats, 1);
    Params.prewarm_first_decode = Request.bPrewarmFirstDecode;
    Params.prewarm_steady_decode = Request.bPrewarmSteadyDecode;
    Params.prewarm_final_decode = Request.bPrewarmFinalDecode;
    Params.dump_first_frame_profile = false;
    Params.streaming_audio_sink = [](const float*, size_t, int32_t) -> bool
    {
        return true;
    };

    qwen3_tts::tts_result Result;
    if (bVoiceDesign)
    {
        Result = Runtime->synthesize("Hello.", Params);
    }
    else if (!SpeakerEmbedding.empty())
    {
        Result = Runtime->synthesize_with_speaker_embedding("Hello.", SpeakerEmbedding, Params);
    }
    else if (!Request.ReferenceAudioPath.empty())
    {
        const std::string RefAudio = ResolvePath(Request.ReferenceAudioPath, Request.ServiceWorkingDirectory);
        Result = Runtime->synthesize_with_voice("Hello.", RefAudio, Params);
    }
    else
    {
        Result = Runtime->synthesize("Hello.", Params);
    }

    const auto EndTime = std::chrono::steady_clock::now();
    const auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(EndTime - StartTime).count();
    if (!Result.success)
    {
        OutError = "Native Qwen3 TTS startup prewarm failed: " + (Result.error_msg.empty() ? std::string("unknown error") : Result.error_msg);
        std::cerr << "[OffgridAI_TTS][startup][prewarm][error] elapsed_ms=" << Ms
                  << " error=" << OutError << std::endl;
        return false;
    }

    std::cout << "[OffgridAI_TTS][startup][prewarm] complete elapsed_ms=" << Ms
              << " discarded_audio_samples=" << Result.audio.size() << std::endl;
    return true;
}

bool Qwen3TTSBackend::LoadSpeakerEmbedding(const std::string& Path, std::string& OutError)
{
    return LoadSpeakerEmbeddingFile(Path, SpeakerEmbedding, OutError);
}

bool Qwen3TTSBackend::LoadSpeakerEmbeddingFile(const std::string& Path, std::vector<float>& OutEmbedding, std::string& OutError)
{
    OutEmbedding.clear();
    if (!FileExists(Path))
    {
        OutError = "Speaker embedding file does not exist: " + Path;
        return false;
    }

    if (!qwen3_tts::load_speaker_embedding_file(Path, OutEmbedding))
    {
        OutError = "Failed to load speaker embedding: " + Path;
        return false;
    }
    if (OutEmbedding.empty())
    {
        OutError = "Speaker embedding was empty: " + Path;
        return false;
    }
    return true;
}

std::string Qwen3TTSBackend::ResolveSpeakerEmbeddingPathForRequest(const TTSRequest& Request)
{
    const std::string BasePath = ResolvePath(Request.SpeakerEmbeddingPath, Request.ServiceWorkingDirectory);
    if (BasePath.empty())
    {
        return std::string();
    }

    // New multi-NPC contract: SpeakerEmbeddingPath points at the directory that
    // contains voice JSON files, and VoiceId is the filename for this NPC, e.g.
    // SpeakerEmbeddingPath=C:\...\reference, VoiceId=ref_speaker.json.
    if (!Request.VoiceId.empty() && DirectoryExists(BasePath))
    {
        return (fs::path(BasePath) / fs::path(Request.VoiceId)).lexically_normal().string();
    }

    // Also support a not-yet-created directory path with a trailing slash. This is
    // useful while authoring data assets before the voice JSON has been copied in.
    if (!Request.VoiceId.empty() && !LooksLikeJsonFilePath(BasePath))
    {
        return (fs::path(BasePath) / fs::path(Request.VoiceId)).lexically_normal().string();
    }

    // Backward compatibility: old projects may still put the full JSON file path
    // directly in SpeakerEmbeddingPath and ignore VoiceId for voice selection.
    return BasePath;
}

std::string Qwen3TTSBackend::ResolvePath(const std::string& Path, const std::string& WorkingDirectory)
{
    if (Path.empty()) return std::string();
    fs::path P(Path);
    if (P.is_absolute()) return P.string();
    if (!WorkingDirectory.empty())
    {
        return (fs::path(WorkingDirectory) / P).lexically_normal().string();
    }
    return fs::absolute(P).string();
}

std::vector<uint8_t> Qwen3TTSBackend::FloatMonoToPCM16(const float* Samples, size_t NumSamples)
{
    std::vector<uint8_t> Bytes;
    Bytes.resize(NumSamples * sizeof(int16_t));
    int16_t* Out = reinterpret_cast<int16_t*>(Bytes.data());
    for (size_t i = 0; i < NumSamples; ++i)
    {
        float V = Samples[i];
        if (V > 1.0f) V = 1.0f;
        if (V < -1.0f) V = -1.0f;
        Out[i] = static_cast<int16_t>(std::lrintf(V * 32767.0f));
    }
    return Bytes;
}
