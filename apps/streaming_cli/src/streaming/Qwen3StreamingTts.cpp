#include "offgrid_tts/Qwen3StreamingTts.h"

#include "qwen3_tts.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path Abs(const fs::path& p) {
    std::error_code ec;
    return fs::absolute(p, ec);
}

std::string NormalizeModelName(std::string model_identifier) {
    if (model_identifier.empty()) {
        return {};
    }

    const fs::path p(model_identifier);
    model_identifier = p.filename().string();
    if (model_identifier.size() < 5 || model_identifier.substr(model_identifier.size() - 5) != ".gguf") {
        model_identifier += ".gguf";
    }
    return model_identifier;
}

} // namespace

struct Qwen3StreamingTts::Impl {
    qwen3_tts::Qwen3TTS engine;
    std::string model_dir = "models";
    std::string loaded_model_name;
    qwen3_tts::tts_model_capabilities caps;
    std::vector<float> speaker_embedding;
};

Qwen3StreamingTts::Qwen3StreamingTts()
    : impl_(new Impl()) {}

Qwen3StreamingTts::~Qwen3StreamingTts() {
    delete impl_;
}

bool Qwen3StreamingTts::load(const std::string& model_dir) {
    impl_->model_dir = model_dir;
    return true;
}

bool Qwen3StreamingTts::load_speaker_embedding(const std::string& path) {
    if (path.empty()) {
        impl_->speaker_embedding.clear();
        return true;
    }

    std::vector<float> embedding;
    if (!qwen3_tts::load_speaker_embedding_file(path, embedding)) {
        std::cerr << "Failed to load speaker embedding: " << path << "\n";
        return false;
    }

    impl_->speaker_embedding = std::move(embedding);
    return true;
}

bool Qwen3StreamingTts::synthesize_streaming(
    const std::string& text,
    const TtsStreamOptions& options,
    TtsChunkCallback on_chunk)
{
    const std::string model_name = NormalizeModelName(options.model_identifier);
    if (!impl_->engine.is_loaded() || impl_->loaded_model_name != model_name) {
        if (!impl_->engine.load_models(impl_->model_dir, model_name)) {
            std::cerr << "Failed to load engine models: " << impl_->engine.get_error() << "\n";
            return false;
        }
        impl_->loaded_model_name = model_name;
        impl_->caps = impl_->engine.get_model_capabilities();
        std::cerr << "Loaded model type: " << impl_->caps.model_type
                  << " | supports_instruction=" << (impl_->caps.supports_instruction ? "yes" : "no")
                  << " | supports_voice_clone=" << (impl_->caps.supports_voice_clone ? "yes" : "no")
                  << " | supports_named_speakers=" << (impl_->caps.supports_named_speakers ? "yes" : "no")
                  << "\n";
    }

    const bool is_voice_design_model = impl_->caps.model_type == "voice_design";
    if (options.voice_design && !is_voice_design_model) {
        std::cerr << "--voice-design was requested, but loaded model type is '" << impl_->caps.model_type << "'\n";
        return false;
    }
    if (is_voice_design_model && !options.voice_design) {
        std::cerr << "VoiceDesign model detected; enabling voice-design behavior automatically.\n";
    }
    if (is_voice_design_model && options.instruction.empty()) {
        std::cerr << "VoiceDesign requires a non-empty instruction. Use --voice-design-instruct or --instruction.\n";
        return false;
    }
    if (is_voice_design_model && !impl_->speaker_embedding.empty()) {
        std::cerr << "VoiceDesign does not accept speaker embeddings. Remove --speaker-embedding.\n";
        return false;
    }

    qwen3_tts::tts_params params;
    params.print_progress = true;
    params.print_timing = true;
    params.temperature = options.temperature;
    params.top_k = options.top_k;
    params.top_p = options.top_p;
    params.repetition_penalty = options.repetition_penalty;
    params.streaming_generate = true;
    params.async_streaming_decode = true;
    params.play_streaming = true;
    params.prewarm_streaming = true;
    params.prewarm_frames = 1;
    params.instruction = options.instruction;
    params.live_preroll_ms = options.live_preroll_ms;
    params.first_tail_window_frames = options.first_tail_window_frames;
    params.steady_tail_window_frames = options.steady_tail_window_frames;
    params.context_frames = options.context_frames;
    params.final_context_frames = options.final_context_frames;
    params.dump_first_frame_profile = options.dump_first_frame_profile;
    params.dump_streaming_overlap = options.dump_streaming_overlap;

    qwen3_tts::tts_result result;
    if (!impl_->speaker_embedding.empty()) {
        result = impl_->engine.synthesize_with_speaker_embedding(text, impl_->speaker_embedding, params);
    } else {
        result = impl_->engine.synthesize(text, params);
    }

    if (!result.success) {
        std::cerr << "Synthesis failed: " << result.error_msg << "\n";
        return false;
    }

    const fs::path output_wav = Abs(options.output_wav.empty() ? fs::path("examples/bridge_test.wav") : fs::path(options.output_wav));
    std::error_code ec;
    fs::create_directories(output_wav.parent_path(), ec);
    if (!qwen3_tts::save_audio_file(output_wav.string(), result.audio, result.sample_rate)) {
        std::cerr << "Failed to write output WAV: " << output_wav.string() << "\n";
        return false;
    }

    if (on_chunk) {
        on_chunk(TtsStreamChunk{result.audio, result.sample_rate, true});
    }

    return true;
}
