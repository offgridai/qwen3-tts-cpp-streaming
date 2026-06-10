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
    bool dump_first_frame_profile = false;
    bool dump_streaming_overlap = false;
    int32_t live_preroll_ms = 0;
    int32_t first_tail_window_frames = 1;
    int32_t steady_tail_window_frames = 12;
    int32_t context_frames = 4;
    int32_t final_context_frames = 4;
};

using TtsChunkCallback = std::function<void(const TtsStreamChunk&)>;

class Qwen3StreamingTts {
public:
    Qwen3StreamingTts();
    ~Qwen3StreamingTts();

    bool load(const std::string& model_dir);
    bool load_speaker_embedding(const std::string& path);
    bool synthesize_streaming(const std::string& text, const TtsStreamOptions& options, TtsChunkCallback on_chunk);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
