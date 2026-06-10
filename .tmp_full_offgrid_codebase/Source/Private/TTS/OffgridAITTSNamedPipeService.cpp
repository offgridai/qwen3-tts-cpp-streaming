#include "TTS/OffgridAITTSNamedPipeService.h"

#include "OffgridAI.h"
#include "Core/OffgridAIServiceProcessLauncher.h"
#include "Data/OffgridAITTSServiceSettingsDataAsset.h"
#include "TTS/OffgridAITTSPipeProtocol.h"
#include "TTS/Transport/OffgridAITTSPipeClient.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
    FString QuoteArg(const FString& Value)
    {
        if (Value.Contains(TEXT(" ")) || Value.Contains(TEXT("\t")) || Value.Contains(TEXT("\"")))
        {
            FString Escaped = Value;
            Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
            return FString::Printf(TEXT("\"%s\""), *Escaped);
        }
        return Value;
    }
    FString ResolveFullPath(const FString& InPath)
    {
        FString Path = InPath;
        FPaths::NormalizeFilename(Path);
        return FPaths::ConvertRelativePathToFull(Path);
    }

    bool FileExists(const FString& InPath)
    {
        return !InPath.IsEmpty() && FPaths::FileExists(ResolveFullPath(InPath));
    }

    bool DirectoryExists(const FString& InPath)
    {
        return !InPath.IsEmpty() && FPaths::DirectoryExists(ResolveFullPath(InPath));
    }
}

FOffgridAITTSNamedPipeService::FOffgridAITTSNamedPipeService(const UOffgridAITTSServiceSettingsDataAsset* InSettings)
    : Settings(InSettings)
    , PipeClient(MakeUnique<FOffgridAITTSPipeClient>())
{
}

FOffgridAITTSNamedPipeService::~FOffgridAITTSNamedPipeService()
{
    Reset();
}

void FOffgridAITTSNamedPipeService::Reset()
{
    StreamEventQueue.Empty();

    if (PipeClient && PipeClient->IsConnected())
    {
        FOffgridAITTSRequest ShutdownRequest;
        PopulateCommonRequestFields(ShutdownRequest);
        ShutdownRequest.Op = EOffgridAITTSOp::Shutdown;
        SendRequest(ShutdownRequest, nullptr, false);
        PipeClient->Disconnect();
    }

    bStartupCompleted = false;

    if (ServiceProcessHandle.IsValid())
    {
        FPlatformProcess::TerminateProc(ServiceProcessHandle, true);
        FPlatformProcess::CloseProc(ServiceProcessHandle);
        ServiceProcessHandle.Reset();
    }

    ServiceProcessId = 0;
}

void FOffgridAITTSNamedPipeService::Tick(double NowSeconds)
{
    (void)NowSeconds;
}

bool FOffgridAITTSNamedPipeService::ValidateStartupSettings(FString& OutError) const
{
    OutError.Reset();

    if (!Settings)
    {
        OutError = TEXT("TTS settings asset is null.");
        return false;
    }

    if (Settings->ServiceExecutablePath.IsEmpty())
    {
        OutError = TEXT("TTS ServiceExecutablePath is empty.");
        return false;
    }

    if (!FileExists(Settings->ServiceExecutablePath))
    {
        OutError = FString::Printf(TEXT("TTS ServiceExecutablePath does not exist: %s"), *Settings->ServiceExecutablePath);
        return false;
    }

    if (!Settings->ServiceWorkingDirectory.IsEmpty() && !DirectoryExists(Settings->ServiceWorkingDirectory))
    {
        OutError = FString::Printf(TEXT("TTS ServiceWorkingDirectory does not exist: %s"), *Settings->ServiceWorkingDirectory);
        return false;
    }

    if (Settings->ModelDirectory.IsEmpty() && Settings->LegacyModelIdentifier.IsEmpty())
    {
        OutError = TEXT("TTS ModelDirectory is empty and legacy ModelIdentifier fallback is also empty.");
        return false;
    }

    if (!Settings->ModelDirectory.IsEmpty() && !DirectoryExists(Settings->ModelDirectory))
    {
        OutError = FString::Printf(TEXT("TTS ModelDirectory does not exist: %s"), *Settings->ModelDirectory);
        return false;
    }

    if (Settings->DefaultVoiceMode != EOffgridAITTSVoiceMode::VoiceDesign
        && !Settings->VoiceEmbeddingPath.IsEmpty()
        && !DirectoryExists(Settings->VoiceEmbeddingPath)
        && !FileExists(Settings->VoiceEmbeddingPath))
    {
        OutError = FString::Printf(TEXT("TTS VoiceEmbeddingPath does not exist as a directory or file: %s"), *Settings->VoiceEmbeddingPath);
        return false;
    }

    if (Settings->DefaultVoiceMode != EOffgridAITTSVoiceMode::VoiceDesign
        && !Settings->ReferenceAudioPath.IsEmpty() && !FileExists(Settings->ReferenceAudioPath))
    {
        OutError = FString::Printf(TEXT("TTS ReferenceAudioPath does not exist: %s"), *Settings->ReferenceAudioPath);
        return false;
    }

    return true;
}

bool FOffgridAITTSNamedPipeService::ValidateVoiceEmbeddingPath(FName VoiceID, FString& OutError) const
{
    OutError.Reset();

    if (!Settings || Settings->VoiceEmbeddingPath.IsEmpty())
    {
        return true;
    }

    // Legacy mode: a full JSON file path is configured globally and used for every line.
    if (FileExists(Settings->VoiceEmbeddingPath))
    {
        return true;
    }

    if (!DirectoryExists(Settings->VoiceEmbeddingPath))
    {
        OutError = FString::Printf(TEXT("TTS VoiceEmbeddingPath does not exist: %s"), *Settings->VoiceEmbeddingPath);
        return false;
    }

    if (VoiceID.IsNone())
    {
        OutError = FString::Printf(TEXT("TTS VoiceID is empty while VoiceEmbeddingPath is a directory: %s"), *Settings->VoiceEmbeddingPath);
        return false;
    }

    const FString VoiceFileName = VoiceID.ToString();
    const FString VoicePath = FPaths::Combine(Settings->VoiceEmbeddingPath, VoiceFileName);
    if (!FileExists(VoicePath))
    {
        OutError = FString::Printf(TEXT("TTS voice embedding file does not exist for VoiceID '%s': %s"), *VoiceFileName, *VoicePath);
        return false;
    }

    return true;
}

FString FOffgridAITTSNamedPipeService::VoiceModeToProtocolString(EOffgridAITTSVoiceMode VoiceMode)
{
    switch (VoiceMode)
    {
    case EOffgridAITTSVoiceMode::Base:
        return TEXT("base");
    case EOffgridAITTSVoiceMode::VoiceDesign:
        return TEXT("voice_design");
    case EOffgridAITTSVoiceMode::CustomVoice:
    default:
        return TEXT("custom_voice");
    }
}

bool FOffgridAITTSNamedPipeService::EnsureServiceReady(FString& OutError)
{
    OutError.Reset();

    if (!ValidateStartupSettings(OutError))
    {
        return false;
    }

    if (!EnsureServiceConnected())
    {
        // Not connected yet is normally transient during startup; keep polling until supervisor timeout.
        return false;
    }

    if (!StartupServiceIfNeeded(&OutError))
    {
        // Empty OutError means transient startup/pipe race; non-empty means deterministic fatal startup failure.
        return false;
    }

    // Startup only means OffgridAI_TTS.exe accepted the boot request. For Qwen,
    // model load/compile/warmup happens behind the service, so Health is the real
    // readiness gate. Treat "still booting" as transient by not surfacing OutError.
    FOffgridAITTSRequest HealthRequest;
    PopulateCommonRequestFields(HealthRequest);
    HealthRequest.Op = EOffgridAITTSOp::Health;
    HealthRequest.RequestID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);

    FOffgridAITTSResponse HealthResponse;
    if (!SendRequest(HealthRequest, &HealthResponse, true))
    {
        return false;
    }

    if (!HealthResponse.bOk)
    {
        const bool bStillBooting = HealthResponse.ErrorMessage.Contains(TEXT("still booting"));
        if (!bStillBooting)
        {
            OutError = HealthResponse.ErrorMessage.IsEmpty()
                ? TEXT("TTS health check failed with no error message.")
                : HealthResponse.ErrorMessage;
        }
        return false;
    }

    return true;
}

bool FOffgridAITTSNamedPipeService::BeginSynthesis(
    const FString& RequestID,
    const FOffgridAILinePerformanceRequest& LineRequest,
    const TArray<uint8>& LoopbackPCM,
    int32 LoopbackSampleRate,
    int32 LoopbackNumChannels)
{
    (void)LoopbackPCM;
    (void)LoopbackSampleRate;
    (void)LoopbackNumChannels;

    if (LineRequest.TTSVoiceMode != EOffgridAITTSVoiceMode::VoiceDesign)
    {
        FString VoicePathError;
        if (!ValidateVoiceEmbeddingPath(LineRequest.VoiceID, VoicePathError))
        {
            UE_LOG(LogOffgridAI, Error, TEXT("TTS NamedPipeService: %s"), *VoicePathError);
            return false;
        }
    }

    if (!EnsureServiceConnected() || !StartupServiceIfNeeded())
    {
        return false;
    }

    FOffgridAITTSRequest Request;
    PopulateCommonRequestFields(Request);
    Request.Op = EOffgridAITTSOp::BeginSynthesis;
    Request.RequestID = RequestID;
    Request.ConversationID = LineRequest.ConversationID.ToString(EGuidFormats::DigitsWithHyphens);
    Request.NPCID = LineRequest.NPCID.ToString();
    Request.LineID = LineRequest.LineID.ToString();
    Request.VoiceID = LineRequest.VoiceID.ToString();
    Request.VoiceMode = VoiceModeToProtocolString(LineRequest.TTSVoiceMode);
    Request.bVoiceDesign = LineRequest.bTTSVoiceDesign || LineRequest.TTSVoiceMode == EOffgridAITTSVoiceMode::VoiceDesign;
    Request.Instruction = LineRequest.TTSInstruction;
    Request.VoiceDesignInstruction = LineRequest.TTSInstruction;
    if (Request.bVoiceDesign)
    {
        Request.VoiceID.Empty();
        Request.SpeakerEmbeddingPath.Empty();
        Request.ReferenceAudioPath.Empty();
        Request.ReferenceText.Empty();
    }
    Request.Dialogue = LineRequest.Dialogue.ToString();
    Request.Emotion = LineRequest.TTSEmotionInstruction.IsEmpty()
        ? LineRequest.Emotion.ToString()
        : LineRequest.TTSEmotionInstruction;

    UE_LOG(LogOffgridAI, Log, TEXT("[TTS][Request] begin_synthesis request=%s npc=%s line=%s chars=%d voice_mode=%s voice_design=%s instruction_len=%d model_dir=%s legacy_model_id=%s voice_embedding=%s line_emotion=%s tts_emotion=%s forward_emotion=%s"),
        *Request.RequestID,
        *Request.NPCID,
        *Request.LineID,
        Request.Dialogue.Len(),
        *Request.VoiceMode,
        Request.bVoiceDesign ? TEXT("true") : TEXT("false"),
        Request.Instruction.Len(),
        *Request.ModelDirectory,
        *Request.ModelIdentifier,
        *Request.SpeakerEmbeddingPath,
        *LineRequest.Emotion.ToString(),
        *Request.Emotion,
        Request.bForwardEmotionToInstruction ? TEXT("true") : TEXT("false"));

    if (Request.bVoiceDesign)
    {
        FString InstructionPreview = Request.Instruction.Left(300);
        InstructionPreview.ReplaceInline(TEXT("\r"), TEXT(" "));
        InstructionPreview.ReplaceInline(TEXT("\n"), TEXT(" | "));
        UE_LOG(LogOffgridAI, Log, TEXT("[TTS][VoiceDesign] request=%s instruction_preview=\"%s\""), *Request.RequestID, *InstructionPreview);
    }

    FOffgridAITTSResponse Response;
    if (!SendRequest(Request, &Response, true) || !Response.bOk)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("TTS NamedPipeService: begin_synthesis failed request=%s error=%s"), *Request.RequestID, *Response.ErrorMessage);
        return false;
    }

    UE_LOG(LogOffgridAI, Log, TEXT("[TTS][Request] begin_synthesis accepted request=%s"), *Request.RequestID);
    return true;
}

void FOffgridAITTSNamedPipeService::Cancel(const FGuid& ConversationID, FName NPCID, FName LineID)
{
    if (!ConversationID.IsValid() || !PipeClient || !PipeClient->IsConnected())
    {
        return;
    }

    FOffgridAITTSRequest Request;
    PopulateCommonRequestFields(Request);
    Request.Op = EOffgridAITTSOp::Cancel;
    Request.RequestID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    Request.ConversationID = ConversationID.ToString(EGuidFormats::DigitsWithHyphens);
    Request.NPCID = NPCID.ToString();
    Request.LineID = LineID.ToString();
    SendRequest(Request, nullptr, false);
}

bool FOffgridAITTSNamedPipeService::TryPopStreamEvent(FOffgridAITTSStreamEvent& OutStreamEvent)
{
    if (StreamEventQueue.Dequeue(OutStreamEvent))
    {
        return true;
    }

    if (!PipeClient || !PipeClient->IsConnected())
    {
        return false;
    }

    FOffgridAITTSRequest Request;
    PopulateCommonRequestFields(Request);
    Request.Op = EOffgridAITTSOp::PollEvent;
    Request.RequestID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);

    FOffgridAITTSResponse Response;
    if (!SendRequest(Request, &Response, true) || !Response.bOk || !Response.bHasEvent)
    {
        return false;
    }

    OutStreamEvent = Response.Event;
    switch (OutStreamEvent.Type)
    {
    case EOffgridAITTSStreamEventType::StreamStarted:
        UE_LOG(LogOffgridAI, Log, TEXT("[TTS][Event] stream_started request=%s npc=%s line=%s sr=%d channels=%d"),
            *OutStreamEvent.RequestID, *OutStreamEvent.NPCID.ToString(), *OutStreamEvent.LineID.ToString(), OutStreamEvent.SampleRate, OutStreamEvent.NumChannels);
        break;
    case EOffgridAITTSStreamEventType::AudioChunk:
        UE_LOG(LogOffgridAI, Log, TEXT("[TTS_TRACE] T5 first_or_next_audio_event_received request=%s line=%s bytes=%d sr=%d channels=%d start_sample=%lld samples=%d"),
            *OutStreamEvent.RequestID,
            *OutStreamEvent.LineID.ToString(),
            OutStreamEvent.PCMChunk.Num(),
            OutStreamEvent.SampleRate,
            OutStreamEvent.NumChannels,
            OutStreamEvent.ChunkStartSample,
            OutStreamEvent.ChunkSampleCount);
        if (Settings && Settings->bVerboseLogging)
        {
            UE_LOG(LogOffgridAI, Log, TEXT("[TTS][Event] audio_chunk request=%s bytes=%d sr=%d channels=%d"),
                *OutStreamEvent.RequestID, OutStreamEvent.PCMChunk.Num(), OutStreamEvent.SampleRate, OutStreamEvent.NumChannels);
        }
        break;
    case EOffgridAITTSStreamEventType::Completed:
        UE_LOG(LogOffgridAI, Log, TEXT("[TTS][Event] completed request=%s npc=%s line=%s"),
            *OutStreamEvent.RequestID, *OutStreamEvent.NPCID.ToString(), *OutStreamEvent.LineID.ToString());
        break;
    case EOffgridAITTSStreamEventType::Error:
        UE_LOG(LogOffgridAI, Error, TEXT("[TTS][Event] error request=%s npc=%s line=%s error=%s"),
            *OutStreamEvent.RequestID, *OutStreamEvent.NPCID.ToString(), *OutStreamEvent.LineID.ToString(), *OutStreamEvent.ErrorMessage);
        break;
    default:
        break;
    }
    return true;
}

bool FOffgridAITTSNamedPipeService::EnsureServiceConnected()
{
    if (PipeClient && PipeClient->IsConnected())
    {
        return true;
    }

    if (!Settings)
    {
        return false;
    }

    if (!ServiceProcessHandle.IsValid())
    {
        const FString ExecutablePath = Settings->ServiceExecutablePath;
        if (ExecutablePath.IsEmpty())
        {
            UE_LOG(LogOffgridAI, Error, TEXT("TTS NamedPipeService: ServiceExecutablePath was empty."));
            return false;
        }

        const FString Args = BuildLaunchArguments();
        ServiceProcessHandle = FOffgridAIServiceProcessLauncher::LaunchDetached(
            ExecutablePath,
            Args,
            Settings->ServiceWorkingDirectory,
            ServiceProcessId);

        if (!ServiceProcessHandle.IsValid())
        {
            UE_LOG(LogOffgridAI, Error, TEXT("TTS NamedPipeService: failed to launch service executable '%s'"), *ExecutablePath);
            return false;
        }
    }

    // Do not block here. Startup polling is driven by FOffgridAILocalServiceGateway::TickServiceStartup.
    // A single connect attempt per tick lets ASR, LLM, and TTS launch in parallel instead of
    // waiting for each service's full pipe timeout before starting the next one.
    if (PipeClient->Connect(Settings->PipeName))
    {
        return true;
    }

    return false;

}

bool FOffgridAITTSNamedPipeService::StartupServiceIfNeeded(FString* OutFatalError)
{
    if (bStartupCompleted)
    {
        return true;
    }

    FOffgridAITTSRequest Request;
    PopulateCommonRequestFields(Request);
    Request.Op = EOffgridAITTSOp::Startup;
    Request.RequestID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);

    FOffgridAITTSResponse Response;
    if (!SendRequest(Request, &Response, true))
    {
        return false;
    }

    if (!Response.bOk)
    {
        const FString FatalError = Response.ErrorMessage.IsEmpty()
            ? TEXT("TTS startup failed with no error message.")
            : Response.ErrorMessage;
        UE_LOG(LogOffgridAI, Error, TEXT("[TTS][Startup] startup failed: %s"), *FatalError);
        if (OutFatalError)
        {
            *OutFatalError = FatalError;
        }
        return false;
    }

    UE_LOG(LogOffgridAI, Log, TEXT("[TTS][Startup] startup accepted model_dir=%s legacy_model_id=%s default_voice_mode=%s voice_embedding=%s"),
        Settings ? *Settings->ModelDirectory : TEXT(""),
        Settings ? *Settings->LegacyModelIdentifier : TEXT(""),
        Settings ? *VoiceModeToProtocolString(Settings->DefaultVoiceMode) : TEXT(""),
        Settings ? *Settings->VoiceEmbeddingPath : TEXT(""));
    bStartupCompleted = true;
    return true;
}

bool FOffgridAITTSNamedPipeService::SendRequest(const FOffgridAITTSRequest& Request, FOffgridAITTSResponse* OutResponse, bool bExpectResponse)
{
    if (!PipeClient || !PipeClient->IsConnected())
    {
        return false;
    }

    TArray<uint8> RequestBytes;
    if (!OffgridAITTSProtocol::SerializeRequest(Request, RequestBytes) || !PipeClient->SendBytes(RequestBytes))
    {
        return false;
    }

    if (!bExpectResponse)
    {
        return true;
    }

    TArray<uint8> ResponseBytes;
    if (!PipeClient->ReceiveBytes(ResponseBytes))
    {
        return false;
    }

    FOffgridAITTSResponse ParsedResponse;
    if (!OffgridAITTSProtocol::DeserializeResponse(ResponseBytes, ParsedResponse))
    {
        return false;
    }

    if (OutResponse)
    {
        *OutResponse = MoveTemp(ParsedResponse);
    }

    return true;
}

void FOffgridAITTSNamedPipeService::PopulateCommonRequestFields(FOffgridAITTSRequest& OutRequest) const
{
    if (!Settings)
    {
        return;
    }

    OutRequest.ModelIdentifier = Settings->LegacyModelIdentifier;
    OutRequest.ModelDirectory = Settings->ModelDirectory;
    OutRequest.VoiceMode = VoiceModeToProtocolString(Settings->DefaultVoiceMode);
    OutRequest.bVoiceDesign = Settings->DefaultVoiceMode == EOffgridAITTSVoiceMode::VoiceDesign;
    OutRequest.SpeakerEmbeddingPath = Settings->VoiceEmbeddingPath;
    OutRequest.ReferenceAudioPath = Settings->ReferenceAudioPath;
    OutRequest.ReferenceText = Settings->ReferenceText;
    if (OutRequest.bVoiceDesign)
    {
        OutRequest.Instruction = Settings->DefaultVoiceDesignInstruction;
        OutRequest.VoiceDesignInstruction = Settings->DefaultVoiceDesignInstruction;
        OutRequest.SpeakerEmbeddingPath.Empty();
        OutRequest.ReferenceAudioPath.Empty();
        OutRequest.ReferenceText.Empty();
    }
    OutRequest.Language = Settings->Language.IsEmpty() ? TEXT("English") : Settings->Language;
    OutRequest.ServiceWorkingDirectory = Settings->ServiceWorkingDirectory;
    OutRequest.bUseGPU = Settings->bUseGPU;
    OutRequest.bPrewarmStreaming = Settings->bPrewarmStreaming;
    OutRequest.bForwardEmotionToInstruction = Settings->bForwardEmotionToInstruction;
    OutRequest.bAsyncStreamingDecode = Settings->bAsyncStreamingDecode;
    OutRequest.bDumpFirstFrameProfile = Settings->bDumpFirstFrameProfile;
    OutRequest.FirstTailWindowFrames = Settings->FirstTailWindowFrames;
    OutRequest.SteadyTailWindowFrames = Settings->SteadyTailWindowFrames;
    OutRequest.ContextFrames = Settings->ContextFrames;
    OutRequest.FinalContextFrames = Settings->FinalContextFrames;
    OutRequest.PrewarmFrames = Settings->PrewarmFrames;
    OutRequest.PrewarmRepeats = Settings->PrewarmRepeats;
    OutRequest.bPrewarmFirstDecode = Settings->bPrewarmFirstDecode;
    OutRequest.bPrewarmSteadyDecode = Settings->bPrewarmSteadyDecode;
    OutRequest.bPrewarmFinalDecode = Settings->bPrewarmFinalDecode;
    OutRequest.MaxAudioTokens = Settings->MaxAudioTokens;
    OutRequest.TopK = Settings->TopK;
    OutRequest.TopP = Settings->TopP;
    OutRequest.Temperature = Settings->Temperature;
    OutRequest.RepetitionPenalty = Settings->RepetitionPenalty;

    OutRequest.OutputSampleRate = Settings->OutputSampleRateHz;
    OutRequest.NumChannels = Settings->NumChannels;
    OutRequest.SampleFormat = GetConfiguredSampleFormat();
    OutRequest.PreferredChunkMs = Settings->PreferredChunkMs;
    OutRequest.bVerboseLogging = Settings->bVerboseLogging;
}

FString FOffgridAITTSNamedPipeService::BuildLaunchArguments() const
{
    if (!Settings)
    {
        return FString();
    }

    TArray<FString> Args = Settings->LaunchArguments;
    for (int32 Index = 0; Index < Args.Num(); ++Index)
    {
        Args[Index] = QuoteArg(Args[Index]);
    }

    return FString::Join(Args, TEXT(" "));
}

FString FOffgridAITTSNamedPipeService::GetConfiguredSampleFormat() const
{
    // v1.0 TTS streaming uses a fixed PCM16 byte contract.
    // Do not advertise Float32 here unless EOffgridAIAudioSampleFormat is deliberately expanded.
    return TEXT("pcm16");
}
