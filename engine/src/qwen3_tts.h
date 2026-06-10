#pragma once

#include "text_tokenizer.h"
#include "tts_transformer.h"
#include "audio_tokenizer_encoder.h"
#include "audio_tokenizer_decoder.h"

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace qwen3_tts {
namespace pipeline_internal {
struct ops;
}

// TTS generation parameters
struct tts_params {
    // Maximum number of audio tokens to generate
    int32_t max_audio_tokens = 4096;
    
    // Temperature for sampling (0 = greedy)
    float temperature = 0.9f;
    
    // Top-p sampling
    float top_p = 1.0f;
    
    // Top-k sampling (0 = disabled)
    int32_t top_k = 75;
    
    // Number of threads
    int32_t n_threads = 4;
    
    // Print progress during generation
    bool print_progress = false;
    
    // Print timing information
    bool print_timing = true;
    
    // Repetition penalty for CB0 token generation (HuggingFace style)
    float repetition_penalty = 1.05f;

    // Language ID for codec (2050=en, 2069=ru, 2055=zh, 2058=ja, 2064=ko, 2053=de, 2061=fr, 2054=es)
    int32_t language_id = 2050;

    // Optional style/voice instruction
    std::string instruction;

    // Optional named speaker (for CustomVoice models)
    std::string speaker;


    // Experimental: interleave generation with tail-context vocoder decode.
    // Produces a WAV-compatible output buffer, but decode is still single-threaded
    // and blocks generation inside the frame callback.
    bool streaming_generate = true;
    int32_t first_tail_window_frames = 3;
    // After the first window, optionally use a few smaller ramp windows before
    // settling into the steady-state size. This reduces early burstiness while
    // preserving the larger hot-path decode shape for throughput.
    int32_t ramp_tail_window_frames = 6;
    int32_t ramp_tail_window_count = 0;
    int32_t steady_tail_window_frames = 8;
    int32_t context_frames = 3;
    // Optional larger context for the final streaming window. A short steady-state
    // context is good for latency, but the last acoustic tail can need more left
    // context to avoid sounding clipped/truncated. <=0 means use context_frames.
    int32_t final_context_frames = 4;

    // Experimental: when streaming_generate is enabled, queue vocoder decode
    // work to a background worker so autoregressive code generation can keep
    // running while prior windows decode.
    bool async_streaming_decode = true;

    // Experimental Windows-only live monitor for streaming chunks. The final
    // WAV is still written normally. On unsupported platforms this is ignored.
    bool play_streaming = true;

    // Experimental: before a streaming request, run a tiny throwaway
    // transformer/decode pass to build/capture hot graphs and warm CUDA kernels.
    // This is excluded from reported synthesis timing.
    bool prewarm_streaming = true;
    int32_t prewarm_frames = 1;
    // Repeat the prewarm pass. Useful for testing whether first-request latency
    // is dominated by first-run graph/kernel/JIT overhead versus steady hot-path cost.
    int32_t prewarm_repeats = 1;
    // Warm the exact first decode graph/window, the steady tail-context graph,
    // and optionally the final-context graph. These are independent so latency
    // experiments can isolate which warmup actually matters.
    bool prewarm_first_decode = true;
    bool prewarm_steady_decode = true;
    bool prewarm_final_decode = false;

    // Diagnostic: print a compact first-frame latency breakdown for streaming generation.
    bool dump_first_frame_profile = false;

    // Windows live playback only: buffer this many milliseconds before the first
    // waveOutWrite. This is a playback safety cushion only; it does not alter
    // WAV assembly or synthesis timing. A value of 0 preserves immediate playback.
    int32_t live_preroll_ms = 150;

    // Adaptive streaming: shrink steady-state decode windows when queued audio
    // is running low, then expand again once playback headroom recovers.
    bool adaptive_steady_windows = false;
    int32_t adaptive_min_tail_window_frames = 4;
    int32_t adaptive_low_watermark_ms = 250;
    int32_t adaptive_high_watermark_ms = 900;

    // Diagnostic: print per-window streaming queue/decode timing so we can verify
    // whether generation is actually overlapping vocoder decode.
    bool dump_streaming_overlap = false;

    // Optional paced delivery: keep coarse decode windows for throughput, but
    // emit smaller audio chunks to downstream consumers and live playback.
    bool paced_audio_delivery = false;
    int32_t delivery_chunk_ms = 80;
    int32_t delivery_start_buffer_ms = 150;
    int32_t delivery_target_lead_ms = 500;
    std::function<bool(const float * samples, int32_t n_samples, int32_t sample_rate, bool is_final)> audio_chunk_callback;
};

// TTS generation result
struct tts_result {
    // Generated audio samples (24kHz, mono)
    std::vector<float> audio;
    
    // Sample rate
    int32_t sample_rate = 24000;
    
    // Success flag
    bool success = false;
    
    // Error message if failed
    std::string error_msg;
    
    // Timing info (in milliseconds)
    int64_t t_load_ms = 0;
    int64_t t_tokenize_ms = 0;
    int64_t t_encode_ms = 0;
    int64_t t_generate_ms = 0;
    int64_t t_decode_ms = 0;
    int64_t t_total_ms = 0;

    // Process memory snapshots (bytes)
    uint64_t mem_rss_start_bytes = 0;
    uint64_t mem_rss_end_bytes = 0;
    uint64_t mem_rss_peak_bytes = 0;
    uint64_t mem_phys_start_bytes = 0;
    uint64_t mem_phys_end_bytes = 0;
    uint64_t mem_phys_peak_bytes = 0;
    
};

// Model capabilities inferred from loaded GGUF metadata.
struct tts_model_capabilities {
    bool loaded = false;
    bool supports_voice_clone = false;
    bool supports_named_speakers = false;
    bool supports_instruction = false;
    int32_t speaker_embedding_dim = 0;
    int32_t speaker_count = 0;
    std::string model_type;
};

// Progress callback type
using tts_progress_callback_t = std::function<void(int tokens_generated, int max_tokens)>;

// Main TTS class that orchestrates the full pipeline
class Qwen3TTS {
public:
    Qwen3TTS();
    ~Qwen3TTS();
    
    // Load all models from directory
    // model_dir should contain: transformer.gguf, tokenizer.gguf, vocoder.gguf
    bool load_models(const std::string & model_dir, const std::string & model_name = "");
    
    // Generate speech from text
    // text: input text to synthesize
    // params: generation parameters
    tts_result synthesize(const std::string & text,
                          const tts_params & params = tts_params());
    
    // Generate speech with voice cloning
    // text: input text to synthesize
    // reference_audio: path to reference audio file (WAV, 24kHz)
    // params: generation parameters
    tts_result synthesize_with_voice(const std::string & text,
                                      const std::string & reference_audio,
                                      const tts_params & params = tts_params());
    
    // Generate speech with voice cloning from samples
    // text: input text to synthesize
    // ref_samples: reference audio samples (24kHz, mono, normalized to [-1, 1])
    // n_ref_samples: number of reference samples
    // params: generation parameters
    tts_result synthesize_with_voice(const std::string & text,
                                      const float * ref_samples, int32_t n_ref_samples,
                                      const tts_params & params = tts_params());

    // Generate speech from a precomputed speaker embedding vector
    tts_result synthesize_with_speaker_embedding(const std::string & text,
                                                 const std::vector<float> & speaker_embedding,
                                                 const tts_params & params = tts_params());

    // Extract speaker embedding from reference audio file (WAV)
    bool extract_speaker_embedding(const std::string & reference_audio,
                                   std::vector<float> & speaker_embedding,
                                   int64_t * encode_time_ms = nullptr);
    
    // Set progress callback
    void set_progress_callback(tts_progress_callback_t callback);
    
    // Get error message
    const std::string & get_error() const { return error_msg_; }
    
    // Check if models are loaded
    bool is_loaded() const { return models_loaded_; }

    // List named speakers exposed by the currently loaded model metadata.
    // Returns normalized (lowercase) speaker keys; empty for non-CustomVoice models.
    std::vector<std::string> get_available_speakers() const;

    // Return feature flags for the currently loaded model.
    tts_model_capabilities get_model_capabilities() const;
    
private:
    friend struct pipeline_internal::ops;

    TextTokenizer tokenizer_;
    TTSTransformer transformer_;
    AudioTokenizerEncoder audio_encoder_;
    AudioTokenizerDecoder audio_decoder_;
    
    bool models_loaded_ = false;
    bool encoder_loaded_ = false;
    bool transformer_loaded_ = false;
    bool decoder_loaded_ = false;
    bool low_mem_mode_ = false;
    std::string error_msg_;
    std::string tts_model_path_;
    std::string decoder_model_path_;
    tts_progress_callback_t progress_callback_;
};

// Utility: Load audio file (WAV format)
bool load_audio_file(const std::string & path, std::vector<float> & samples, 
                     int & sample_rate);

// Utility: Save audio file (WAV format)
bool save_audio_file(const std::string & path, const std::vector<float> & samples,
                     int sample_rate);

// Utility: Load speaker embedding from JSON or float32 binary
bool load_speaker_embedding_file(const std::string & path,
                                 std::vector<float> & embedding);

// Utility: Save speaker embedding as JSON (.json) or float32 binary
bool save_speaker_embedding_file(const std::string & path,
                                 const std::vector<float> & embedding);

} // namespace qwen3_tts
