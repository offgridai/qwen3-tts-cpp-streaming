#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class TtsHintEnergyClass : uint8_t {
    unknown = 0,
    silence = 1,
    speech_like = 2,
    burst_like = 3,
};

struct TtsStreamHintHeader {
    int32_t sample_rate = 24000;
    std::string model_type;
    bool has_instruction = false;
    bool has_speaker_conditioning = false;
};

struct TtsStreamHintChunk {
    int32_t chunk_index = 0;
    int32_t codec_frame_start = 0;
    int32_t codec_frame_end = 0;
    int64_t audio_sample_start = 0;
    int64_t audio_sample_end = 0;
    double audio_start_sec = 0.0;
    double audio_end_sec = 0.0;
    float rms_energy = 0.0f;
    float peak_energy = 0.0f;
    float zero_crossing_rate = 0.0f;
    TtsHintEnergyClass energy_class = TtsHintEnergyClass::unknown;
    bool is_paced_chunk = false;
    bool is_final = false;
};

struct TtsStreamChunk {
    std::vector<float> samples;
    int32_t sample_rate = 24000;
    bool is_final = false;
    bool has_hint = false;
    TtsStreamHintChunk hint;
};

struct TtsStreamOptions {
    std::string output_wav = "examples/bridge_test.wav";
    std::string model_identifier;
    std::string instruction;
    bool voice_design = false;
    float temperature = 0.9f;
    int32_t top_k = 75;
    float top_p = 1.0f;
    float repetition_penalty = 1.05f;
    int32_t max_audio_tokens = 4096;
    bool print_progress = true;
    bool print_timing = true;
    bool dump_first_frame_profile = false;
    bool dump_streaming_overlap = false;
    bool play_streaming = true;
    int32_t live_preroll_ms = 150;
    int32_t first_tail_window_frames = 3;
    int32_t ramp_tail_window_frames = 6;
    int32_t ramp_tail_window_count = 0;
    int32_t steady_tail_window_frames = 8;
    int32_t context_frames = 2;
    int32_t early_context_frames = 1;
    int32_t early_context_window_count = 2;
    int32_t final_context_frames = 3;
    bool adaptive_steady_windows = false;
    int32_t adaptive_min_tail_window_frames = 6;
    int32_t adaptive_low_watermark_ms = 220;
    int32_t adaptive_high_watermark_ms = 520;
    bool paced_audio_delivery = false;
    int32_t delivery_chunk_ms = 40;
    int32_t delivery_start_buffer_ms = 40;
    int32_t delivery_target_lead_ms = 300;
    bool paced_live_playback = false;
    int32_t steady_split_decode_frames = 0;
    bool async_streaming_decode = true;
    bool cache_instruction_tokens = false;
    std::string instruction_cache_key;
    bool warm_voice_profile = false;
    std::string warm_voice_profile_key;
    std::string warmup_text = "Hello.";
    std::function<void(const TtsStreamHintHeader&)> hint_header_callback;
};

using TtsChunkCallback = std::function<void(const TtsStreamChunk&)>;

class Qwen3StreamingTts {
public:
    Qwen3StreamingTts();
    ~Qwen3StreamingTts();

    bool load(const std::string& model_dir);
    bool load_speaker_embedding(const std::string& path);
    bool warm_voice_profile(const TtsStreamOptions& options);
    bool synthesize_streaming(const std::string& text, const TtsStreamOptions& options, TtsChunkCallback on_chunk);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
