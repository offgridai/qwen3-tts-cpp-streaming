#include "offgrid_tts/Qwen3StreamingTts.h"

#include "qwen3_tts.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
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

TtsHintEnergyClass ConvertEnergyClass(qwen3_tts::tts_hint_energy_class value) {
    switch (value) {
    case qwen3_tts::tts_hint_energy_class::silence:
        return TtsHintEnergyClass::silence;
    case qwen3_tts::tts_hint_energy_class::speech_like:
        return TtsHintEnergyClass::speech_like;
    case qwen3_tts::tts_hint_energy_class::burst_like:
        return TtsHintEnergyClass::burst_like;
    case qwen3_tts::tts_hint_energy_class::unknown:
    default:
        return TtsHintEnergyClass::unknown;
    }
}

TtsStreamHintChunk ConvertHintChunk(const qwen3_tts::tts_stream_hint_chunk & chunk) {
    TtsStreamHintChunk out;
    out.chunk_index = chunk.chunk_index;
    out.codec_frame_start = chunk.codec_frame_start;
    out.codec_frame_end = chunk.codec_frame_end;
    out.audio_sample_start = chunk.audio_sample_start;
    out.audio_sample_end = chunk.audio_sample_end;
    out.audio_start_sec = chunk.audio_start_sec;
    out.audio_end_sec = chunk.audio_end_sec;
    out.rms_energy = chunk.rms_energy;
    out.peak_energy = chunk.peak_energy;
    out.zero_crossing_rate = chunk.zero_crossing_rate;
    out.energy_class = ConvertEnergyClass(chunk.energy_class);
    out.is_paced_chunk = chunk.is_paced_chunk;
    out.is_final = chunk.is_final;
    return out;
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

bool Qwen3StreamingTts::warm_voice_profile(const TtsStreamOptions& options) {
    const std::string model_name = NormalizeModelName(options.model_identifier);
    if (!impl_->engine.is_loaded() || impl_->loaded_model_name != model_name) {
        if (!impl_->engine.load_models(impl_->model_dir, model_name)) {
            std::cerr << "Failed to load engine models: " << impl_->engine.get_error() << "\n";
            return false;
        }
        impl_->loaded_model_name = model_name;
        impl_->caps = impl_->engine.get_model_capabilities();
    }

    qwen3_tts::tts_params params;
    params.print_progress = options.print_progress;
    params.print_timing = options.print_timing;
    params.max_audio_tokens = options.max_audio_tokens;
    params.temperature = options.temperature;
    params.top_k = options.top_k;
    params.top_p = options.top_p;
    params.repetition_penalty = options.repetition_penalty;
    params.streaming_generate = true;
    params.async_streaming_decode = options.async_streaming_decode;
    params.play_streaming = false;
    params.prewarm_streaming = false;
    params.instruction = options.instruction;
    params.cache_instruction_tokens = options.cache_instruction_tokens;
    params.instruction_cache_key = options.instruction_cache_key;
    params.voice_profile_key = options.warm_voice_profile_key;
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
    params.paced_live_playback = false;
    params.steady_split_decode_frames = options.steady_split_decode_frames;
    params.dump_first_frame_profile = false;
    params.dump_streaming_overlap = false;
    if (options.hint_header_callback) {
        params.stream_hint_header_callback =
            [on_hint_header = options.hint_header_callback](const qwen3_tts::tts_stream_hint_header & header) {
                TtsStreamHintHeader out;
                out.sample_rate = header.sample_rate;
                out.model_type = header.model_type;
                out.has_instruction = header.has_instruction;
                out.has_speaker_conditioning = header.has_speaker_conditioning;
                on_hint_header(out);
            };
    }

    return impl_->engine.warm_voice_profile(options.warmup_text, params);
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
    params.print_progress = options.print_progress;
    params.print_timing = options.print_timing;
    params.max_audio_tokens = options.max_audio_tokens;
    params.temperature = options.temperature;
    params.top_k = options.top_k;
    params.top_p = options.top_p;
    params.repetition_penalty = options.repetition_penalty;
    params.streaming_generate = true;
    params.async_streaming_decode = options.async_streaming_decode;
    params.play_streaming = options.play_streaming;
    params.prewarm_streaming = false;
    params.instruction = options.instruction;
    params.cache_instruction_tokens = options.cache_instruction_tokens;
    params.instruction_cache_key = options.instruction_cache_key;
    params.voice_profile_key = options.warm_voice_profile_key;
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
    if (options.hint_header_callback) {
        params.stream_hint_header_callback =
            [on_hint_header = options.hint_header_callback](const qwen3_tts::tts_stream_hint_header & header) {
                TtsStreamHintHeader out;
                out.sample_rate = header.sample_rate;
                out.model_type = header.model_type;
                out.has_instruction = header.has_instruction;
                out.has_speaker_conditioning = header.has_speaker_conditioning;
                on_hint_header(out);
            };
    }
    if (on_chunk) {
        auto pending_hint_mutex = std::make_shared<std::mutex>();
        auto pending_hint = std::make_shared<TtsStreamHintChunk>();
        auto pending_hint_valid = std::make_shared<bool>(false);
        params.stream_hint_chunk_callback =
            [pending_hint_mutex, pending_hint, pending_hint_valid](const qwen3_tts::tts_stream_hint_chunk & hint) -> bool {
                std::lock_guard<std::mutex> lock(*pending_hint_mutex);
                *pending_hint = ConvertHintChunk(hint);
                *pending_hint_valid = true;
                return true;
            };
        params.audio_chunk_callback =
            [on_chunk, pending_hint_mutex, pending_hint, pending_hint_valid](const float* samples, int32_t n_samples, int32_t sample_rate, bool is_final) -> bool {
                TtsStreamChunk chunk;
                chunk.sample_rate = sample_rate;
                chunk.is_final = is_final;
                {
                    std::lock_guard<std::mutex> lock(*pending_hint_mutex);
                    if (*pending_hint_valid) {
                        chunk.has_hint = true;
                        chunk.hint = *pending_hint;
                        *pending_hint_valid = false;
                    }
                }
                if (samples && n_samples > 0) {
                    chunk.samples.assign(samples, samples + n_samples);
                }
                on_chunk(chunk);
                return true;
            };
    }

    if (options.warm_voice_profile) {
        if (!warm_voice_profile(options)) {
            std::cerr << "Voice profile warmup failed: " << impl_->engine.get_error() << "\n";
            return false;
        }
    }

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

    return true;
}
