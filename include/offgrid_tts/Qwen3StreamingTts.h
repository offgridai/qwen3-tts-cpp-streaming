#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TtsStreamChunk {
    int sample_rate = 24000;
    int channels = 1;
    std::vector<int16_t> pcm;
    bool is_final = false;
};

struct TtsStreamOptions {
    int first_tail_window_frames = 1;
    int steady_tail_window_frames = 12;
    int context_frames = 4;
    int final_context_frames = 4;
    bool prewarm = true;
    bool async_decode = true;
    bool play_streaming = true;
    bool dump_first_frame_profile = false;
    std::string output_wav = "examples/bridge_test.wav";
};

using TtsChunkCallback = std::function<void(const TtsStreamChunk&)>;

class Qwen3StreamingTts {
public:
    Qwen3StreamingTts();
    ~Qwen3StreamingTts();

    Qwen3StreamingTts(const Qwen3StreamingTts&) = delete;
    Qwen3StreamingTts& operator=(const Qwen3StreamingTts&) = delete;

    bool load(const std::string& model_dir);
    bool load_speaker_embedding(const std::string& path);

    bool synthesize_streaming(
        const std::string& text,
        const TtsStreamOptions& options,
        TtsChunkCallback on_chunk);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
