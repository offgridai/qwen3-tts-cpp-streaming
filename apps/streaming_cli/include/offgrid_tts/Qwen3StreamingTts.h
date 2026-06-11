#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TtsStreamChunk {
    std::vector<float> samples;
    int32_t sample_rate = 24000;
    bool is_final = false;
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
    bool dump_first_frame_profile = false;
    bool dump_streaming_overlap = false;
    int32_t live_preroll_ms = 150;
    int32_t first_tail_window_frames = 3;
    int32_t ramp_tail_window_frames = 5;
    int32_t ramp_tail_window_count = 2;
    int32_t steady_tail_window_frames = 8;
    int32_t context_frames = 3;
    int32_t early_context_frames = 2;
    int32_t early_context_window_count = 2;
    int32_t final_context_frames = 4;
    bool adaptive_steady_windows = true;
    int32_t adaptive_min_tail_window_frames = 6;
    int32_t adaptive_low_watermark_ms = 220;
    int32_t adaptive_high_watermark_ms = 520;
    bool paced_audio_delivery = true;
    int32_t delivery_chunk_ms = 80;
    int32_t delivery_start_buffer_ms = 80;
    int32_t delivery_target_lead_ms = 240;
    bool paced_live_playback = false;
    int32_t steady_split_decode_frames = 0;
    bool cache_instruction_tokens = false;
    std::string instruction_cache_key;
    bool warm_voice_profile = false;
    std::string warm_voice_profile_key;
    std::string warmup_text = "Hello.";
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
