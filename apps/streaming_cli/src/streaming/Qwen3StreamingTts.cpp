#include "offgrid_tts/Qwen3StreamingTts.h"

#include "qwen3_tts.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string NormalizeModelName(std::string model_identifier) {
    if (model_identifier.empty()) {
        return {};
    }

    model_identifier = fs::path(model_identifier).filename().string();
    if (model_identifier.size() < 5 || model_identifier.substr(model_identifier.size() - 5) != ".gguf") {
        model_identifier += ".gguf";
    }
    return model_identifier;
}

TtsModelCapabilities ConvertCapabilities(const qwen3_tts::tts_model_capabilities& caps) {
    TtsModelCapabilities out;
    out.loaded = caps.loaded;
    out.supports_voice_clone = caps.supports_voice_clone;
    out.supports_named_speakers = caps.supports_named_speakers;
    out.supports_instruction = caps.supports_instruction;
    out.speaker_embedding_dim = caps.speaker_embedding_dim;
    out.speaker_count = caps.speaker_count;
    out.model_type = caps.model_type;
    return out;
}

} // namespace

struct Qwen3StreamingTts::Impl {
    qwen3_tts::Qwen3TTS engine;
    std::string model_dir;
    std::string loaded_model_name;
    std::string error;
    TtsModelCapabilities caps;
    std::vector<float> speaker_embedding;
};

Qwen3StreamingTts::Qwen3StreamingTts()
    : impl_(std::make_unique<Impl>()) {}

Qwen3StreamingTts::~Qwen3StreamingTts() = default;
Qwen3StreamingTts::Qwen3StreamingTts(Qwen3StreamingTts&&) noexcept = default;
Qwen3StreamingTts& Qwen3StreamingTts::operator=(Qwen3StreamingTts&&) noexcept = default;

bool Qwen3StreamingTts::load(const std::string& model_dir, const std::string& model_identifier) {
    impl_->error.clear();
    if (!fs::is_directory(model_dir)) {
        impl_->error = "Model directory does not exist: " + model_dir;
        return false;
    }

    const std::string model_name = NormalizeModelName(model_identifier);
    if (!impl_->engine.load_models(model_dir, model_name)) {
        impl_->error = "Failed to load engine models: " + impl_->engine.get_error();
        return false;
    }

    impl_->model_dir = model_dir;
    impl_->loaded_model_name = model_name;
    impl_->caps = ConvertCapabilities(impl_->engine.get_model_capabilities());
    return true;
}

bool Qwen3StreamingTts::load_speaker_embedding(const std::string& path) {
    impl_->error.clear();
    if (path.empty()) {
        impl_->speaker_embedding.clear();
        return true;
    }

    std::vector<float> embedding;
    if (!qwen3_tts::load_speaker_embedding_file(path, embedding)) {
        impl_->error = "Failed to load speaker embedding: " + path;
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
    impl_->error.clear();
    const std::string requested_model = NormalizeModelName(options.model_identifier);
    if (!impl_->engine.is_loaded()) {
        impl_->error = "No model is loaded. Call load() before synthesis.";
        return false;
    }
    if (!requested_model.empty() && requested_model != impl_->loaded_model_name) {
        if (!load(impl_->model_dir, requested_model)) {
            return false;
        }
    }

    const bool is_voice_design_model = impl_->caps.model_type == "voice_design";
    if (options.voice_design && !is_voice_design_model) {
        impl_->error = "Voice design was requested for model type '" + impl_->caps.model_type + "'.";
        return false;
    }
    if (is_voice_design_model && options.instruction.empty()) {
        impl_->error = "VoiceDesign requires a non-empty instruction.";
        return false;
    }
    if (is_voice_design_model && !impl_->speaker_embedding.empty()) {
        impl_->error = "VoiceDesign does not accept speaker embeddings.";
        return false;
    }

    qwen3_tts::tts_params params;
    params.print_progress = options.print_progress;
    params.print_timing = options.print_timing;
    params.max_audio_tokens = options.max_audio_tokens;
    params.max_audio_frames_per_text_token = options.max_audio_frames_per_text_token;
    params.min_dynamic_audio_tokens = options.min_dynamic_audio_tokens;
    params.temperature = options.temperature;
    params.top_k = options.top_k;
    params.top_p = options.top_p;
    params.cb0_top_p = options.cb0_top_p;
    params.repetition_penalty = options.repetition_penalty;
    params.seed = options.seed;
    params.language_id = options.language_id;
    params.speaker = options.speaker;
    params.streaming_generate = true;
    params.async_streaming_decode = options.async_streaming_decode;
    params.play_streaming = options.play_streaming;
    params.instruction = options.instruction;
    params.cache_instruction_tokens = options.cache_instruction_tokens;
    params.instruction_cache_key = options.instruction_cache_key;
    params.live_preroll_ms = options.live_preroll_ms;
    params.first_tail_window_frames = options.first_tail_window_frames;
    params.ramp_tail_window_frames = options.ramp_tail_window_frames;
    params.ramp_tail_window_count = options.ramp_tail_window_count;
    params.steady_tail_window_frames = options.steady_tail_window_frames;
    params.context_frames = options.context_frames;
    params.early_context_frames = options.early_context_frames;
    params.early_context_window_count = options.early_context_window_count;
    params.final_context_frames = options.final_context_frames;
    params.adaptive_steady_windows = options.adaptive_steady_windows;
    params.adaptive_min_tail_window_frames = options.adaptive_min_tail_window_frames;
    params.adaptive_low_watermark_ms = options.adaptive_low_watermark_ms;
    params.adaptive_high_watermark_ms = options.adaptive_high_watermark_ms;
    params.paced_audio_delivery = options.paced_audio_delivery;
    params.delivery_chunk_ms = options.delivery_chunk_ms;
    params.delivery_start_buffer_ms = options.delivery_start_buffer_ms;
    params.delivery_target_lead_ms = options.delivery_target_lead_ms;
    params.paced_live_playback = options.paced_live_playback;
    params.steady_split_decode_frames = options.steady_split_decode_frames;
    params.dump_first_frame_profile = options.dump_first_frame_profile;
    params.dump_streaming_overlap = options.dump_streaming_overlap;
    if (on_chunk) {
        params.audio_chunk_callback = [on_chunk = std::move(on_chunk)](
            const float* samples, int32_t n_samples, int32_t sample_rate, bool is_final) {
            TtsStreamChunk chunk;
            chunk.sample_rate = sample_rate;
            chunk.is_final = is_final;
            if (samples && n_samples > 0) {
                chunk.samples.assign(samples, samples + n_samples);
            }
            return on_chunk(chunk);
        };
    }

    qwen3_tts::tts_result result;
    if (!impl_->speaker_embedding.empty()) {
        result = impl_->engine.synthesize_with_speaker_embedding(text, impl_->speaker_embedding, params);
    } else {
        result = impl_->engine.synthesize(text, params);
    }
    if (!result.success) {
        impl_->error = "Synthesis failed: " + result.error_msg;
        return false;
    }

    if (!options.output_wav.empty()) {
        const fs::path output_wav = fs::absolute(fs::path(options.output_wav));
        std::error_code ec;
        if (!output_wav.parent_path().empty()) {
            fs::create_directories(output_wav.parent_path(), ec);
            if (ec) {
                impl_->error = "Failed to create output directory: " + ec.message();
                return false;
            }
        }
        if (!qwen3_tts::save_audio_file(output_wav.string(), result.audio, result.sample_rate)) {
            impl_->error = "Failed to write output WAV: " + output_wav.string();
            return false;
        }
    }

    return true;
}

bool Qwen3StreamingTts::is_loaded() const {
    return impl_->engine.is_loaded();
}

const TtsModelCapabilities& Qwen3StreamingTts::capabilities() const {
    return impl_->caps;
}

const std::string& Qwen3StreamingTts::last_error() const {
    return impl_->error;
}
