#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct TtsStreamChunk {
    std::vector<float> samples;
    int32_t sample_rate = 24000;
    bool is_final = false;
};

struct TtsModelCapabilities {
    bool loaded = false;
    bool supports_voice_clone = false;
    bool supports_named_speakers = false;
    bool supports_instruction = false;
    int32_t speaker_embedding_dim = 0;
    int32_t speaker_count = 0;
    std::string model_type;
};

struct TtsStreamOptions {
    // Leave empty to stream without writing a WAV file.
    std::string output_wav;
    std::string model_identifier;
    std::string instruction;
    std::string speaker;
    int32_t language_id = 2050;
    bool voice_design = false;
    float temperature = 0.75f;
    int32_t top_k = 16;
    float top_p = 0.9f;
    float cb0_top_p = 1.0f;
    float repetition_penalty = 1.02f;
    int64_t seed = -1;
    int32_t max_audio_tokens = 4096;
    float max_audio_frames_per_text_token = 5.0f;
    int32_t min_dynamic_audio_tokens = 64;
    bool print_progress = true;
    bool print_timing = true;
    bool dump_first_frame_profile = false;
    bool dump_streaming_overlap = false;
    bool play_streaming = true;
    int32_t live_preroll_ms = 150;
    int32_t first_tail_window_frames = 3;
    int32_t ramp_tail_window_frames = 6;
    int32_t ramp_tail_window_count = 0;
    // 0 selects the model-tuned default: 7 frames for 0.6B, 12 for 1.7B.
    int32_t steady_tail_window_frames = 0;
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
};

// Return false to cancel generation after the current chunk.
using TtsChunkCallback = std::function<bool(const TtsStreamChunk&)>;

class Qwen3StreamingTts {
public:
    Qwen3StreamingTts();
    ~Qwen3StreamingTts();

    Qwen3StreamingTts(const Qwen3StreamingTts&) = delete;
    Qwen3StreamingTts& operator=(const Qwen3StreamingTts&) = delete;
    Qwen3StreamingTts(Qwen3StreamingTts&&) noexcept;
    Qwen3StreamingTts& operator=(Qwen3StreamingTts&&) noexcept;

    // Load the selected model immediately. An empty identifier selects the
    // engine's default model from model_dir.
    bool load(const std::string& model_dir, const std::string& model_identifier = {});
    bool load_speaker_embedding(const std::string& path);
    bool synthesize_streaming(const std::string& text,
                              const TtsStreamOptions& options,
                              TtsChunkCallback on_chunk = {});

    // Offline physical-batch hooks. Generation remains sequential; equal-size
    // codec sequences can then share one vocoder graph execution.
    bool generate_audio_codes(const std::string& text,
                              const TtsStreamOptions& options,
                              std::vector<int32_t>& codes,
                              int32_t& frame_count);
    bool decode_audio_codes_batch(const std::vector<std::vector<int32_t>>& code_batches,
                                  std::vector<std::vector<float>>& audio_batches);
    bool save_audio(const std::string& path, const std::vector<float>& audio,
                    int32_t sample_rate = 24000);

    bool is_loaded() const;
    const TtsModelCapabilities& capabilities() const;
    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
