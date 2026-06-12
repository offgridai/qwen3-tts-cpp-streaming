#include "qwen3_tts.h"
#include "pipeline/pipeline_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace qwen3_tts {
using pipeline_internal::format_bytes;
using pipeline_internal::get_process_memory_snapshot;
using pipeline_internal::get_time_ms;
using pipeline_internal::log_memory_usage;
using pipeline_internal::ops;
using pipeline_internal::process_memory_snapshot;
using pipeline_internal::resample_linear;


namespace {

int32_t clamp_positive(int32_t v, int32_t fallback) {
    return v > 0 ? v : fallback;
}

// Qwen3 12Hz tokenizer output is effectively 80ms per code frame at 24 kHz,
// with a fixed decoder trim visible in current outputs:
//
//   4 frames  ->  7125 samples
//   16 frames -> 30165 samples
//
// That is n * 1920 - 555. Use this only to drop local context from
// tail-context streaming windows; the actual emitted sample count still comes
// from the decoder output.
size_t qwen3_decoder_samples_for_frames(int32_t n_frames, int32_t sample_rate) {
    if (n_frames <= 0 || sample_rate <= 0) {
        return 0;
    }
    const int64_t samples_per_frame = (int64_t) sample_rate * 80 / 1000;
    const int64_t decoder_trim = 555;
    const int64_t samples = (int64_t) n_frames * samples_per_frame - decoder_trim;
    return samples > 0 ? (size_t) samples : 0;
}

double qwen3_samples_to_ms(size_t n_samples, int32_t sample_rate) {
    if (sample_rate <= 0) {
        return 0.0;
    }
    return 1000.0 * (double) n_samples / (double) sample_rate;
}

int32_t clamp_window_frames(int32_t value, int32_t min_value, int32_t max_value) {
    if (max_value < min_value) {
        return min_value;
    }
    return std::max(min_value, std::min(value, max_value));
}


class StreamingAudioPlayer {
public:
    StreamingAudioPlayer() = default;
    ~StreamingAudioPlayer() { close(); }

    bool open(int32_t sample_rate, int64_t stream_start_ms, int32_t live_preroll_ms = 0) {
#ifdef _WIN32
        close();
        stream_start_ms_ = stream_start_ms;
        sample_rate_ = sample_rate;
        preroll_samples_ = live_preroll_ms > 0 ? (size_t) (((int64_t) sample_rate * live_preroll_ms) / 1000) : 0;
        startup_min_samples_ = preroll_samples_ > 0 ? std::max(preroll_samples_, preroll_samples_ * 2) : 0;
        playback_started_ = preroll_samples_ == 0;
        starvation_active_ = false;
        preroll_buffer_.clear();
        submitted_samples_total_.store(0);
        if (sample_rate <= 0) { last_error_ = "invalid sample rate"; return false; }
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 1;
        fmt.nSamplesPerSec = (DWORD) sample_rate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = (WORD) (fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        MMRESULT mm = waveOutOpen(&wave_out_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
        if (mm != MMSYSERR_NOERROR) {
            last_error_ = "waveOutOpen failed";
            wave_out_ = nullptr;
            return false;
        }
        stopping_ = false;
        worker_ = std::thread([this]() { worker_loop(); });
        return true;
#else
        (void) sample_rate; (void) stream_start_ms; (void) live_preroll_ms;
        last_error_ = "live streaming playback is only implemented on Windows";
        return false;
#endif
    }

    bool write(const std::vector<float> & samples, size_t offset, size_t count) {
        if (offset >= samples.size() || count == 0) { return true; }
        return write(samples.data() + offset, count);
    }

    bool write(const float * samples, size_t count) {
#ifdef _WIN32
        if (!wave_out_ || !samples || count == 0) { return true; }

        std::vector<int16_t> pcm(count);
        for (size_t i = 0; i < count; ++i) {
            float v = samples[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm[i] = (int16_t) (v * 32767.0f);
        }

        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) { return true; }

            if (!playback_started_ && preroll_samples_ > 0) {
                preroll_buffer_.insert(preroll_buffer_.end(), pcm.begin(), pcm.end());
                if (preroll_buffer_.size() < preroll_samples_ || preroll_buffer_.size() < startup_min_samples_) {
                    return true;
                }
                queue_.push_back(std::move(preroll_buffer_));
                preroll_buffer_.clear();
                playback_started_ = true;
                notify = true;
            } else {
                queue_.push_back(std::move(pcm));
                notify = true;
            }
        }
        if (notify) {
            cv_.notify_one();
        }
        return true;
#else
        (void) samples; (void) count;
        return true;
#endif
    }

    void drain() {
#ifdef _WIN32
        if (!wave_out_) { return; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!preroll_buffer_.empty()) {
                queue_.push_back(std::move(preroll_buffer_));
                preroll_buffer_.clear();
                playback_started_ = true;
            }
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
#endif
    }

    void close() {
#ifdef _WIN32
        if (!wave_out_) { return; }
        drain();
        waveOutClose(wave_out_);
        wave_out_ = nullptr;
#endif
    }

    const std::string & last_error() const { return last_error_; }
    int64_t first_submit_ms() const { return first_submit_ms_.load(); }
    int64_t first_starvation_ms() const { return first_starvation_ms_.load(); }
    int32_t starvation_events() const { return starvation_events_.load(); }
    double queued_audio_ms() const {
#ifdef _WIN32
        std::lock_guard<std::mutex> lock(mutex_);
        return qwen3_samples_to_ms(queued_samples_locked(), sample_rate_);
#else
        return 0.0;
#endif
    }
    double queued_audio_at_first_submit_ms() const {
#ifdef _WIN32
        return qwen3_samples_to_ms((size_t) std::max<int64_t>(0, queued_samples_at_first_submit_.load()), sample_rate_);
#else
        return 0.0;
#endif
    }

    void reset_timing_base(int64_t stream_start_ms) {
#ifdef _WIN32
        stream_start_ms_ = stream_start_ms;
        first_submit_ms_.store(-1);
        first_starvation_ms_.store(-1);
        starvation_events_.store(0);
        queued_samples_at_first_submit_.store(0);
        starvation_active_ = false;
#else
        (void) stream_start_ms;
#endif
    }

private:
#ifdef _WIN32
    struct PendingChunk { std::vector<int16_t> pcm; WAVEHDR hdr{}; };

    size_t estimated_played_samples_unlocked() const {
        const int64_t first_submit = first_submit_ms_.load();
        if (first_submit < 0 || sample_rate_ <= 0) {
            return 0;
        }
        const int64_t elapsed_ms = std::max<int64_t>(0, stream_start_ms_ > 0 ? (get_time_ms() - stream_start_ms_ - first_submit) : 0);
        return (size_t) (((int64_t) sample_rate_ * elapsed_ms) / 1000);
    }

    size_t queued_samples_locked() const {
        size_t total = preroll_buffer_.size();
        for (const auto & pcm : queue_) {
            total += pcm.size();
        }
        const int64_t submitted_total = submitted_samples_total_.load();
        const size_t played = estimated_played_samples_unlocked();
        const size_t pending_remaining = submitted_total > 0
            ? (size_t) std::max<int64_t>(0, submitted_total - (int64_t) played)
            : 0;
        return total + pending_remaining;
    }

    void retire_completed(bool wait_all) {
        if (!wave_out_) { return; }
        for (;;) {
            bool retired_any = false;
            for (auto it = pending_.begin(); it != pending_.end();) {
                WAVEHDR * hdr = &(*it)->hdr;
                if (hdr->dwFlags & WHDR_DONE) {
                    waveOutUnprepareHeader(wave_out_, hdr, sizeof(WAVEHDR));
                    it = pending_.erase(it);
                    retired_any = true;
                } else {
                    ++it;
                }
            }
            if (!wait_all || pending_.empty()) { break; }
            if (!retired_any) { Sleep(5); }
        }
    }

    bool submit_pcm(std::vector<int16_t> pcm) {
        if (!wave_out_ || pcm.empty()) { return true; }
        const size_t pcm_samples = pcm.size();
        auto chunk = std::make_unique<PendingChunk>();
        chunk->pcm = std::move(pcm);
        chunk->hdr.lpData = reinterpret_cast<LPSTR>(chunk->pcm.data());
        chunk->hdr.dwBufferLength = (DWORD) (chunk->pcm.size() * sizeof(int16_t));
        MMRESULT mm = waveOutPrepareHeader(wave_out_, &chunk->hdr, sizeof(WAVEHDR));
        if (mm != MMSYSERR_NOERROR) {
            last_error_ = "waveOutPrepareHeader failed";
            return false;
        }
        mm = waveOutWrite(wave_out_, &chunk->hdr, sizeof(WAVEHDR));
        if (mm != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(wave_out_, &chunk->hdr, sizeof(WAVEHDR));
            last_error_ = "waveOutWrite failed";
            return false;
        }
        submitted_samples_total_.fetch_add((int64_t) pcm_samples);
        int64_t expected = -1;
        const int64_t submitted_ms = stream_start_ms_ > 0 ? (get_time_ms() - stream_start_ms_) : 0;
        if (first_submit_ms_.compare_exchange_strong(expected, submitted_ms)) {
            size_t queued_samples = pcm_samples;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queued_samples += preroll_buffer_.size();
                for (const auto & queued_pcm : queue_) {
                    queued_samples += queued_pcm.size();
                }
            }
            queued_samples_at_first_submit_.store((int64_t) queued_samples);
            fprintf(stderr, "  [player] playback_start wall_ms=%lld queued_audio_ms=%.1f\n",
                    (long long) submitted_ms,
                    qwen3_samples_to_ms(queued_samples, sample_rate_));
        }
        pending_.push_back(std::move(chunk));
        return true;
    }

    void worker_loop() {
        for (;;) {
            std::vector<int16_t> pcm;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (!queue_.empty() || !pending_.empty()) {
                    starvation_active_ = false;
                }
                if (!stopping_ &&
                    playback_started_ &&
                    first_submit_ms_.load() >= 0 &&
                    queued_samples_locked() == 0 &&
                    !starvation_active_) {
                    const int64_t starvation_ms = stream_start_ms_ > 0 ? (get_time_ms() - stream_start_ms_) : 0;
                    int64_t expected = -1;
                    first_starvation_ms_.compare_exchange_strong(expected, starvation_ms);
                    const int32_t event_index = starvation_events_.fetch_add(1) + 1;
                    starvation_active_ = true;
                    fprintf(stderr, "  [player] starvation event=%d wall_ms=%lld\n",
                            event_index,
                            (long long) starvation_ms);
                }
                cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                    return stopping_ || !queue_.empty();
                });
                if (!queue_.empty()) {
                    pcm = std::move(queue_.front());
                    queue_.pop_front();
                } else if (stopping_) {
                    lock.unlock();
                    retire_completed(true);
                    break;
                }
            }

            retire_completed(false);
            if (!pcm.empty()) {
                if (!submit_pcm(std::move(pcm))) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.clear();
                    stopping_ = true;
                }
            }
        }
    }

    HWAVEOUT wave_out_ = nullptr;
    std::deque<std::vector<int16_t>> queue_;
    std::deque<std::unique_ptr<PendingChunk>> pending_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopping_ = false;
    int32_t sample_rate_ = 0;
    size_t preroll_samples_ = 0;
    size_t startup_min_samples_ = 0;
    std::vector<int16_t> preroll_buffer_;
    bool playback_started_ = true;
    int64_t stream_start_ms_ = 0;
    std::atomic<int64_t> first_submit_ms_{-1};
    std::atomic<int64_t> first_starvation_ms_{-1};
    std::atomic<int32_t> starvation_events_{0};
    std::atomic<int64_t> queued_samples_at_first_submit_{0};
    std::atomic<int64_t> submitted_samples_total_{0};
    bool starvation_active_ = false;
#endif
    std::string last_error_;
};

} // namespace

tts_result Qwen3TTS::synthesize(const std::string & text,
                                const tts_params & params) {
    tts_result result;

    if (!models_loaded_) {
        result.error_msg = "Models not loaded";
        return result;
    }

    if (!params.speaker.empty()) {
        std::vector<float> speaker_embedding;
        if (!transformer_.get_named_speaker_embedding(params.speaker, speaker_embedding)) {
            result.error_msg = "Failed to resolve speaker '" + params.speaker + "': " + transformer_.get_error();
            return result;
        }
        if (params.print_progress) {
            fprintf(stderr, "Using named speaker: %s (%zu floats)\n",
                    params.speaker.c_str(), speaker_embedding.size());
        }
        return ops::synthesize_internal(*this, text, speaker_embedding.data(), params, result);
    }

    if (transformer_.get_config().tts_model_type == "custom_voice") {
        result.error_msg = "CustomVoice model requires --speaker, --reference, or --speaker-embedding";
        return result;
    }

    return ops::synthesize_internal(*this, text, nullptr, params, result);
}

tts_result Qwen3TTS::synthesize_with_voice(const std::string & text,
                                           const std::string & reference_audio,
                                           const tts_params & params) {
    tts_result result;

    std::vector<float> ref_samples;
    int ref_sample_rate;
    if (!load_audio_file(reference_audio, ref_samples, ref_sample_rate)) {
        result.error_msg = "Failed to load reference audio: " + reference_audio;
        return result;
    }

    const int target_rate = 24000;
    if (ref_sample_rate != target_rate) {
        fprintf(stderr, "Resampling audio from %d Hz to %d Hz...\n", ref_sample_rate, target_rate);
        std::vector<float> resampled;
        resample_linear(ref_samples.data(), (int) ref_samples.size(), ref_sample_rate, resampled, target_rate);
        ref_samples = std::move(resampled);
    }

    return synthesize_with_voice(text, ref_samples.data(), (int32_t) ref_samples.size(), params);
}

tts_result Qwen3TTS::synthesize_with_voice(const std::string & text,
                                           const float * ref_samples, int32_t n_ref_samples,
                                           const tts_params & params) {
    tts_result result;

    if (!models_loaded_) {
        result.error_msg = "Models not loaded";
        return result;
    }

    if (!encoder_loaded_) {
        if (tts_model_path_.empty()) {
            result.error_msg = "Internal error: missing TTS model path for lazy encoder load";
            return result;
        }
        int64_t t_encoder_load_start = get_time_ms();
        if (!audio_encoder_.load_model(tts_model_path_)) {
            result.error_msg = "Failed to load speaker encoder: " + audio_encoder_.get_error();
            return result;
        }
        encoder_loaded_ = true;
        if (params.print_timing) {
            fprintf(stderr, "  Speaker encoder lazy-loaded in %lld ms\n",
                    (long long) (get_time_ms() - t_encoder_load_start));
            log_memory_usage("voice/after-encoder-load");
        }
    }

    int64_t t_encode_start = get_time_ms();
    std::vector<float> speaker_embedding;

    if (!audio_encoder_.encode(ref_samples, n_ref_samples, speaker_embedding)) {
        result.error_msg = "Failed to extract speaker embedding: " + audio_encoder_.get_error();
        return result;
    }
    result.t_encode_ms = get_time_ms() - t_encode_start;

    const int expected_dim = transformer_.get_config().hidden_size;
    if ((int) speaker_embedding.size() != expected_dim) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Speaker embedding dimension mismatch after extraction: got %zu, expected %d",
                 speaker_embedding.size(), expected_dim);
        result.error_msg = buf;
        return result;
    }

    if (params.print_progress) {
        fprintf(stderr, "Speaker embedding extracted: %zu floats\n", speaker_embedding.size());
    }

    return ops::synthesize_internal(*this, text, speaker_embedding.data(), params, result);
}

tts_result Qwen3TTS::synthesize_with_speaker_embedding(const std::string & text,
                                                       const std::vector<float> & speaker_embedding,
                                                       const tts_params & params) {
    tts_result result;

    if (!models_loaded_) {
        result.error_msg = "Models not loaded";
        return result;
    }

    const int expected_dim = transformer_.get_config().hidden_size;
    if ((int) speaker_embedding.size() != expected_dim) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Speaker embedding dimension mismatch: got %zu, expected %d",
                 speaker_embedding.size(), expected_dim);
        result.error_msg = buf;
        return result;
    }

    if (params.print_progress) {
        fprintf(stderr, "Using provided speaker embedding: %zu floats\n", speaker_embedding.size());
    }

    result.t_encode_ms = 0;
    return ops::synthesize_internal(*this, text, speaker_embedding.data(), params, result);
}

bool Qwen3TTS::extract_speaker_embedding(const std::string & reference_audio,
                                         std::vector<float> & speaker_embedding,
                                         int64_t * encode_time_ms) {
    if (!models_loaded_) {
        error_msg_ = "Models not loaded";
        return false;
    }

    std::vector<float> ref_samples;
    int ref_sample_rate = 0;
    if (!load_audio_file(reference_audio, ref_samples, ref_sample_rate)) {
        error_msg_ = "Failed to load reference audio: " + reference_audio;
        return false;
    }

    const int target_rate = 24000;
    if (ref_sample_rate != target_rate) {
        fprintf(stderr, "Resampling audio from %d Hz to %d Hz...\n", ref_sample_rate, target_rate);
        std::vector<float> resampled;
        resample_linear(ref_samples.data(), (int) ref_samples.size(), ref_sample_rate, resampled, target_rate);
        ref_samples = std::move(resampled);
    }

    if (!encoder_loaded_) {
        if (tts_model_path_.empty()) {
            error_msg_ = "Internal error: missing TTS model path for lazy encoder load";
            return false;
        }
        int64_t t_encoder_load_start = get_time_ms();
        if (!audio_encoder_.load_model(tts_model_path_)) {
            error_msg_ = "Failed to load speaker encoder: " + audio_encoder_.get_error();
            return false;
        }
        encoder_loaded_ = true;
        fprintf(stderr, "  Speaker encoder lazy-loaded in %lld ms\n",
                (long long) (get_time_ms() - t_encoder_load_start));
        log_memory_usage("voice/after-encoder-load");
    }

    const int64_t t_encode_start = get_time_ms();
    if (!audio_encoder_.encode(ref_samples.data(), (int32_t) ref_samples.size(), speaker_embedding)) {
        error_msg_ = "Failed to extract speaker embedding: " + audio_encoder_.get_error();
        return false;
    }

    const int expected_dim = transformer_.get_config().hidden_size;
    if ((int) speaker_embedding.size() != expected_dim) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Speaker embedding dimension mismatch after extraction: got %zu, expected %d",
                 speaker_embedding.size(), expected_dim);
        error_msg_ = buf;
        return false;
    }

    if (encode_time_ms) {
        *encode_time_ms = get_time_ms() - t_encode_start;
    }
    return true;
}

tts_result pipeline_internal::ops::synthesize_internal(Qwen3TTS & self,
                                                       const std::string & text,
                                                       const float * speaker_embedding,
                                                       const tts_params & params,
                                                       tts_result & result) {
    int64_t t_total_start = get_time_ms();
    const bool trace_stream_summary = params.print_progress || params.print_timing || params.dump_first_frame_profile;
    const bool trace_stream_detail = params.print_progress || params.dump_streaming_overlap;
    const bool trace_callback_detail = params.dump_streaming_overlap;

    if (trace_stream_summary) {
        fprintf(stderr, "\n[stream] request_accepted wall_ms=0 text_bytes=%zu has_instruction=%s\n",
                text.size(),
                params.instruction.empty() ? "no" : "yes");
    }
    auto sample_memory = [&](const char * stage) {
        process_memory_snapshot mem;
        if (!get_process_memory_snapshot(mem)) {
            return;
        }
        if (result.mem_rss_start_bytes == 0) {
            result.mem_rss_start_bytes = mem.rss_bytes;
            result.mem_phys_start_bytes = mem.phys_footprint_bytes;
        }
        result.mem_rss_end_bytes = mem.rss_bytes;
        result.mem_phys_end_bytes = mem.phys_footprint_bytes;
        if (mem.rss_bytes > result.mem_rss_peak_bytes) {
            result.mem_rss_peak_bytes = mem.rss_bytes;
        }
        if (mem.phys_footprint_bytes > result.mem_phys_peak_bytes) {
            result.mem_phys_peak_bytes = mem.phys_footprint_bytes;
        }
        if (params.print_timing) {
            fprintf(stderr, "  [mem] %-24s rss=%s  phys=%s\n",
                    stage,
                    format_bytes(mem.rss_bytes).c_str(),
                    format_bytes(mem.phys_footprint_bytes).c_str());
        }
    };
    sample_memory("synth/start");

    int64_t t_tokenize_start = get_time_ms();
    std::vector<int32_t> text_tokens = self.tokenizer_.encode_for_tts(text);
    std::vector<int32_t> instruct_tokens;
    if (!params.instruction.empty()) {
        const std::string instruction_cache_key = params.instruction_cache_key.empty()
            ? params.instruction
            : params.instruction_cache_key;
        if (params.cache_instruction_tokens) {
            auto found = self.instruction_token_cache_.find(instruction_cache_key);
            if (found != self.instruction_token_cache_.end()) {
                instruct_tokens = found->second;
                if (params.print_progress) {
                    fprintf(stderr, "Instruction token cache hit: key=%s tokens=%zu\n",
                            instruction_cache_key.c_str(),
                            instruct_tokens.size());
                }
            } else {
                instruct_tokens = self.tokenizer_.encode_instruct(params.instruction);
                if (!instruct_tokens.empty()) {
                    self.instruction_token_cache_[instruction_cache_key] = instruct_tokens;
                    if (params.print_progress) {
                        fprintf(stderr, "Instruction token cache store: key=%s tokens=%zu\n",
                                instruction_cache_key.c_str(),
                                instruct_tokens.size());
                    }
                }
            }
        } else {
            instruct_tokens = self.tokenizer_.encode_instruct(params.instruction);
        }
    }
    result.t_tokenize_ms = get_time_ms() - t_tokenize_start;
    sample_memory("synth/after-tokenize");

    if (text_tokens.empty()) {
        result.error_msg = "Failed to tokenize text";
        return result;
    }
    if (!params.instruction.empty() && instruct_tokens.empty()) {
        result.error_msg = "Failed to tokenize instruction";
        return result;
    }

    if (params.print_progress) {
        fprintf(stderr, "Text tokenized: %zu tokens\n", text_tokens.size());
        if (!instruct_tokens.empty()) {
            fprintf(stderr, "Instruction tokenized: %zu tokens\n", instruct_tokens.size());
        }
        fprintf(stderr, "  Tokens: ");
        for (size_t i = 0; i < std::min(text_tokens.size(), (size_t) 10); ++i) {
            fprintf(stderr, "%d ", text_tokens[i]);
        }
        if (text_tokens.size() > 10) fprintf(stderr, "...");
        fprintf(stderr, "\n");
    }

    int64_t t_generate_start = get_time_ms();
    int64_t excluded_timing_ms = 0;
    if (!self.transformer_loaded_) {
        int64_t t_reload_start = get_time_ms();
        if (!self.transformer_.load_model(self.tts_model_path_)) {
            result.error_msg = "Failed to reload TTS transformer: " + self.transformer_.get_error();
            return result;
        }
        self.transformer_loaded_ = true;
        if (params.print_timing) {
            fprintf(stderr, "  Transformer reloaded in %lld ms\n",
                    (long long) (get_time_ms() - t_reload_start));
            sample_memory("synth/after-transformer-reload");
        }
    }
    self.transformer_.clear_kv_cache();

    std::vector<int32_t> speech_codes;
    int n_codebooks = self.transformer_.get_config().n_codebooks;
    int n_frames = 0;

    int64_t t_decode_start = 0;
    int64_t streaming_decode_ms = 0;
    int64_t live_playback_wait_ms = 0;

    if (params.streaming_generate) {
        if (!self.decoder_loaded_) {
            int64_t t_decoder_load_start = get_time_ms();
            if (self.decoder_model_path_.empty()) {
                result.error_msg = "Internal error: missing vocoder model path";
                return result;
            }
            if (!self.audio_decoder_.load_model(self.decoder_model_path_)) {
                result.error_msg = "Failed to load vocoder: " + self.audio_decoder_.get_error();
                return result;
            }
            self.decoder_loaded_ = true;
            if (params.print_timing) {
                fprintf(stderr, "  Vocoder lazy-loaded in %lld ms\n",
                        (long long) (get_time_ms() - t_decoder_load_start));
                sample_memory("synth/after-vocoder-load");
            }
        }

        const int32_t first_window = clamp_positive(params.first_tail_window_frames, 3);
        const int32_t ramp_window = clamp_positive(params.ramp_tail_window_frames, 6);
        const int32_t ramp_window_count = std::max<int32_t>(0, params.ramp_tail_window_count);
        const int32_t steady_window = clamp_positive(params.steady_tail_window_frames, 8);
        const int32_t context_frames = std::max<int32_t>(0, params.context_frames);
        const int32_t early_context_frames = params.early_context_frames > 0
            ? std::max<int32_t>(0, params.early_context_frames)
            : context_frames;
        const int32_t early_context_window_count = std::max<int32_t>(0, params.early_context_window_count);
        const int32_t final_context_frames = std::max<int32_t>(context_frames, params.final_context_frames > 0 ? params.final_context_frames : context_frames);
        const int32_t sample_rate = self.audio_decoder_.get_config().sample_rate;
        const size_t live_preroll_samples = (size_t) std::max<int64_t>(0, ((int64_t) sample_rate * std::max<int32_t>(0, params.live_preroll_ms)) / 1000);
        const size_t delivery_chunk_samples = (size_t) std::max<int64_t>(1, ((int64_t) sample_rate * std::max<int32_t>(1, params.delivery_chunk_ms)) / 1000);
        const size_t delivery_start_buffer_samples = (size_t) std::max<int64_t>(1, ((int64_t) sample_rate * std::max<int32_t>(1, params.delivery_start_buffer_ms)) / 1000);
        const size_t delivery_target_lead_samples = (size_t) std::max<int64_t>(0, ((int64_t) sample_rate * std::max<int32_t>(0, params.delivery_target_lead_ms)) / 1000);
        const int32_t steady_split_decode_frames = std::max<int32_t>(0, params.steady_split_decode_frames);
        const bool adaptive_windows = params.adaptive_steady_windows;
        const int32_t adaptive_min_window = clamp_window_frames(
            clamp_positive(params.adaptive_min_tail_window_frames, 4), 1, steady_window);
        const int32_t adaptive_low_watermark_ms = std::max<int32_t>(0, params.adaptive_low_watermark_ms);
        const int32_t adaptive_high_watermark_ms = std::max<int32_t>(
            adaptive_low_watermark_ms,
            params.adaptive_high_watermark_ms > 0 ? params.adaptive_high_watermark_ms : adaptive_low_watermark_ms);

        if (params.prewarm_streaming && params.print_progress) {
            fprintf(stderr, "Streaming prewarm requested but ignored in simple-and-fast mode\n");
        }

        int32_t last_enqueued_frame = 0;
        int32_t last_completed_frame = 0;
        int32_t next_decode_end = first_window;
        int32_t stream_window_index = 0;
        std::atomic<bool> stream_error{false};
        std::string stream_error_msg;
        const size_t playable_50ms_samples = (size_t) (((int64_t) sample_rate * 50) / 1000);
        const size_t playable_150ms_samples = (size_t) (((int64_t) sample_rate * 150) / 1000);
        const size_t playable_200ms_samples = (size_t) (((int64_t) sample_rate * 200) / 1000);
        int64_t first_decode_ms = -1;
        int64_t first_window_queued_ms = -1;
        int64_t first_pcm_ready_ms = -1;
        int64_t first_playable_50ms_ready_ms = -1;
        int64_t first_playable_150ms_ready_ms = -1;
        int64_t first_playable_200ms_ready_ms = -1;
        int64_t first_playback_enqueue_ms = -1;
        int64_t first_audio_ready_ms = -1;
        int64_t prev_window_ready_ms = -1;
        int64_t first_window_ready_ms = -1;
        int64_t second_window_gap_ms = -1;
        int64_t max_window_gap_ms = -1;
        std::atomic<int64_t> emitted_audio_samples{0};
        std::atomic<int32_t> last_selected_tail_window{steady_window};
        tts_generation_first_frame_profile first_frame_profile{};

        std::unique_ptr<StreamingAudioPlayer> live_player;
        if (params.play_streaming) {
            live_player = std::make_unique<StreamingAudioPlayer>();
            const int32_t player_preroll_ms = (params.paced_audio_delivery && params.paced_live_playback) ? 0 : params.live_preroll_ms;
            if (!live_player->open(sample_rate, t_generate_start, player_preroll_ms)) {
                fprintf(stderr, "Warning: live streaming playback unavailable: %s\n",
                        live_player->last_error().c_str());
                live_player.reset();
            }
        }
        const bool paced_live_playback = params.paced_audio_delivery && params.paced_live_playback && (bool) live_player;
        const bool paced_delivery = params.paced_audio_delivery && ((bool) params.audio_chunk_callback || paced_live_playback);
        const bool callback_driven_delivery = (bool) params.audio_chunk_callback && !paced_live_playback;
        const size_t effective_delivery_start_buffer_samples =
            paced_live_playback ? std::max(delivery_start_buffer_samples, live_preroll_samples)
                                : (callback_driven_delivery
                                    ? std::min(delivery_start_buffer_samples, delivery_chunk_samples)
                                    : delivery_start_buffer_samples);

        if (params.print_progress) {
            fprintf(stderr,
                    "\nIncremental tail-context streaming generate/decode enabled%s%s:\n"
                    "  first_tail_window_frames=%d ramp_tail_window_frames=%d ramp_tail_window_count=%d steady_tail_window_frames=%d context_frames=%d early_context_frames=%d early_context_window_count=%d final_context_frames=%d live_preroll_ms=%d adaptive_steady_windows=%s adaptive_min_tail_window_frames=%d adaptive_low_watermark_ms=%d adaptive_high_watermark_ms=%d paced_audio_delivery=%s paced_live_playback=%s delivery_chunk_ms=%d delivery_start_buffer_ms=%d delivery_target_lead_ms=%d steady_split_decode_frames=%d\n"
                    "   index       ctx       new       end    frames   decode_ms     dropped    appended  status\n",
                    params.async_streaming_decode ? " (async decode)" : "",
                    live_player ? " (live playback)" : "",
                    first_window, ramp_window, ramp_window_count, steady_window, context_frames, early_context_frames, early_context_window_count, final_context_frames, params.live_preroll_ms,
                    adaptive_windows ? "on" : "off",
                    adaptive_min_window,
                    adaptive_low_watermark_ms,
                    adaptive_high_watermark_ms,
                    paced_delivery ? "on" : "off",
                    paced_live_playback ? "on" : "off",
                    params.delivery_chunk_ms,
                    (int32_t) qwen3_samples_to_ms(effective_delivery_start_buffer_samples, sample_rate),
                    params.delivery_target_lead_ms,
                    steady_split_decode_frames);
        }

        struct PacedDeliveryState {
            std::mutex mutex;
            std::condition_variable cv;
            std::vector<float> buffered;
            size_t emitted_samples = 0;
            size_t first_emit_samples = 0;
            bool finalized = false;
            bool failed = false;
            bool playback_started = false;
            int64_t first_emit_clock_ms = -1;
            int64_t first_emit_wall_ms = -1;
            int64_t next_emit_due_ms = -1;
            int64_t previous_emit_clock_ms = -1;
            int64_t previous_emit_wall_ms = -1;
            int64_t second_emit_gap_ms = -1;
            int64_t max_emit_gap_ms = -1;
            int32_t chunk_index = 0;
            int32_t delivered_chunks = 0;
            std::string error_msg;
        } delivery_state;

        auto emit_delivery_chunk = [&](const float * chunk, size_t count, bool is_final, int64_t wall_ms) -> bool {
            const double audio_ms = qwen3_samples_to_ms(count, sample_rate);
            const int64_t chunk_gap_ms = delivery_state.previous_emit_wall_ms >= 0
                ? (wall_ms - delivery_state.previous_emit_wall_ms)
                : wall_ms;
            if (delivery_state.previous_emit_wall_ms >= 0) {
                if (delivery_state.second_emit_gap_ms < 0) {
                    delivery_state.second_emit_gap_ms = chunk_gap_ms;
                }
                if (chunk_gap_ms > delivery_state.max_emit_gap_ms) {
                    delivery_state.max_emit_gap_ms = chunk_gap_ms;
                }
            }
            delivery_state.previous_emit_clock_ms = t_generate_start + wall_ms;
            delivery_state.previous_emit_wall_ms = wall_ms;
            if (params.audio_chunk_callback) {
                if (trace_callback_detail) {
                    fprintf(stderr,
                            "[deliver] chunk_index=%d samples=%zu audio_ms=%.1f wall_ms_since_request=%lld wall_ms_since_prev_chunk=%lld final=%s\n",
                            delivery_state.chunk_index,
                            count,
                            audio_ms,
                            (long long) wall_ms,
                            (long long) chunk_gap_ms,
                            is_final ? "yes" : "no");
                }
                if (!params.audio_chunk_callback(chunk, (int32_t) count, sample_rate, is_final)) {
                    delivery_state.error_msg = "Audio chunk callback requested stop";
                    delivery_state.failed = true;
                    return false;
                }
            }
            if (paced_live_playback && live_player && count > 0) {
                if (first_playback_enqueue_ms < 0) {
                    first_playback_enqueue_ms = wall_ms;
                }
                const double queued_before_ms = live_player->queued_audio_ms();
                if (trace_stream_detail) {
                    fprintf(stderr,
                            "[stream] player_write chunk_index=%d samples=%zu audio_ms=%.1f wall_ms_since_request=%lld wall_ms_since_prev_window=%lld cumulative_audio_ms=%.1f player_queued_audio_ms(before)=%.1f paced=yes\n",
                            delivery_state.chunk_index,
                            count,
                            audio_ms,
                            (long long) wall_ms,
                            (long long) chunk_gap_ms,
                            qwen3_samples_to_ms((size_t) std::max<int64_t>(0, emitted_audio_samples.load()), sample_rate),
                            queued_before_ms);
                }
                if (!live_player->write(chunk, count)) {
                    delivery_state.error_msg = live_player->last_error().empty()
                        ? "Live player write failed"
                        : ("Live player write failed: " + live_player->last_error());
                    delivery_state.failed = true;
                    return false;
                }
                if (trace_stream_detail) {
                    fprintf(stderr,
                            "[stream] player_queue_after_write chunk_index=%d player_queued_audio_ms=%.1f paced=yes\n",
                            delivery_state.chunk_index,
                            live_player->queued_audio_ms());
                }
            }
            ++delivery_state.delivered_chunks;
            ++delivery_state.chunk_index;
            return true;
        };

        std::thread delivery_thread;
        if (paced_delivery) {
            const int64_t delivery_chunk_interval_ms = std::max<int64_t>(
                1,
                (int64_t) std::llround(qwen3_samples_to_ms(delivery_chunk_samples, sample_rate)));
            const int64_t callback_burst_gap_ms = callback_driven_delivery
                ? std::max<int64_t>(10, delivery_chunk_interval_ms / 4)
                : delivery_chunk_interval_ms;
            delivery_thread = std::thread([&]() {
                for (;;) {
                    std::vector<float> chunk;
                    bool is_final = false;
                    int64_t wall_ms = 0;
                    {
                        std::unique_lock<std::mutex> lock(delivery_state.mutex);
                        size_t emit_count = 0;
                        for (;;) {
                            if (delivery_state.failed) {
                                break;
                            }

                            const size_t available = delivery_state.buffered.size() - delivery_state.emitted_samples;
                            if (available == 0) {
                                if (delivery_state.finalized) {
                                    break;
                                }
                                delivery_state.cv.wait_for(lock, std::chrono::milliseconds(5));
                                continue;
                            }

                            const int64_t now_ms = get_time_ms();
                            if (!delivery_state.playback_started) {
                                if (!delivery_state.finalized && available < effective_delivery_start_buffer_samples) {
                                    delivery_state.cv.wait_for(lock, std::chrono::milliseconds(5));
                                    continue;
                                }

                                emit_count = std::min(available, delivery_chunk_samples);
                                delivery_state.playback_started = true;
                                delivery_state.first_emit_samples = emit_count;
                                delivery_state.first_emit_clock_ms = now_ms;
                                delivery_state.next_emit_due_ms = now_ms + delivery_chunk_interval_ms;
                                break;
                            }

                            if (callback_driven_delivery) {
                                if (delivery_state.finalized && available <= delivery_chunk_samples) {
                                    emit_count = available;
                                    break;
                                }

                                if (available < effective_delivery_start_buffer_samples) {
                                    if (delivery_state.finalized) {
                                        emit_count = available;
                                        break;
                                    }
                                    delivery_state.cv.wait_for(lock, std::chrono::milliseconds(5));
                                    continue;
                                }

                                const int64_t elapsed_ms = std::max<int64_t>(0, now_ms - delivery_state.first_emit_clock_ms);
                                const size_t elapsed_samples = (size_t) (((int64_t) sample_rate * elapsed_ms) / 1000);
                                const size_t emitted_ahead_samples = delivery_state.emitted_samples > elapsed_samples
                                    ? (delivery_state.emitted_samples - elapsed_samples)
                                    : 0;
                                const int64_t burst_due_ms = delivery_state.previous_emit_clock_ms >= 0
                                    ? (delivery_state.previous_emit_clock_ms + callback_burst_gap_ms)
                                    : now_ms;
                                const size_t callback_refill_target_samples =
                                    std::max(delivery_chunk_samples, delivery_target_lead_samples);
                                const size_t refill_needed_samples = emitted_ahead_samples >= callback_refill_target_samples
                                    ? delivery_chunk_samples
                                    : std::max(delivery_chunk_samples, callback_refill_target_samples - emitted_ahead_samples);

                                if (now_ms >= burst_due_ms) {
                                    emit_count = std::min(available, refill_needed_samples);
                                    delivery_state.next_emit_due_ms = std::max<int64_t>(
                                        delivery_state.next_emit_due_ms,
                                        now_ms + callback_burst_gap_ms);
                                    break;
                                }

                                if (delivery_state.next_emit_due_ms > now_ms) {
                                    const int64_t wait_until_ms = std::min(delivery_state.next_emit_due_ms, burst_due_ms);
                                    delivery_state.cv.wait_for(lock, std::chrono::milliseconds(
                                        std::max<int64_t>(1, wait_until_ms - now_ms)));
                                    continue;
                                }

                                emit_count = std::min(available, refill_needed_samples);
                                delivery_state.next_emit_due_ms = std::max<int64_t>(
                                    delivery_state.next_emit_due_ms + callback_burst_gap_ms,
                                    now_ms + callback_burst_gap_ms);
                                break;
                            }

                            if (delivery_state.finalized && available <= delivery_chunk_samples) {
                                emit_count = available;
                                break;
                            }

                            if (available < delivery_chunk_samples) {
                                delivery_state.cv.wait_for(lock, std::chrono::milliseconds(5));
                                continue;
                            }

                            const int64_t elapsed_ms = std::max<int64_t>(0, now_ms - delivery_state.first_emit_clock_ms);
                            const size_t elapsed_samples = (size_t) (((int64_t) sample_rate * elapsed_ms) / 1000);
                            const size_t allowed_samples = delivery_state.first_emit_samples + elapsed_samples + delivery_target_lead_samples;
                            if (delivery_state.finalized || delivery_state.emitted_samples < allowed_samples) {
                                emit_count = delivery_chunk_samples;
                                break;
                            }

                            delivery_state.cv.wait_for(lock, std::chrono::milliseconds(5));
                        }

                        if (delivery_state.failed) {
                            break;
                        }

                        const size_t available = delivery_state.buffered.size() - delivery_state.emitted_samples;
                        if (available == 0 && delivery_state.finalized) {
                            break;
                        }

                        if (emit_count == 0) {
                            continue;
                        }

                        is_final = delivery_state.finalized && (delivery_state.emitted_samples + emit_count >= delivery_state.buffered.size());
                        chunk.assign(delivery_state.buffered.begin() + (ptrdiff_t) delivery_state.emitted_samples,
                                     delivery_state.buffered.begin() + (ptrdiff_t) (delivery_state.emitted_samples + emit_count));
                        delivery_state.emitted_samples += emit_count;
                        wall_ms = std::max<int64_t>(0, get_time_ms() - t_generate_start);
                        if (delivery_state.first_emit_wall_ms < 0) {
                            delivery_state.first_emit_wall_ms = wall_ms;
                        }
                    }

                    if (!emit_delivery_chunk(chunk.data(), chunk.size(), is_final, wall_ms)) {
                        delivery_state.cv.notify_all();
                        break;
                    }
                }
            });
        }

        struct DecodeJob {
            int32_t index = 0;
            int32_t ctx_start = 0;
            int32_t new_start = 0;
            int32_t end_frame = 0;
            int32_t local_frames = 0;
            bool is_final = false;
            int64_t queued_ms = 0;
            std::vector<int32_t> local_codes;
        };

        auto process_decode_job = [&](const DecodeJob & job) -> bool {
            std::vector<float> decoded;
            const int64_t dequeue_ms = get_time_ms() - t_generate_start;
            const int64_t decode_start_ms = get_time_ms();
            if (job.index == 0) {
                if (trace_stream_detail) {
                    fprintf(stderr, "[stream] first_decode_start wall_ms=%lld queued_ms=%lld local_frames=%d\n",
                            (long long) decode_start_ms,
                            (long long) job.queued_ms,
                            job.local_frames);
                }
            }
            if (!self.audio_decoder_.decode(job.local_codes.data(), job.local_frames, decoded)) {
                stream_error_msg = "Failed streaming decode: " + self.audio_decoder_.get_error();
                stream_error.store(true);
                return false;
            }
            const int64_t decode_end_abs_ms = get_time_ms();
            const int64_t decode_ms = decode_end_abs_ms - decode_start_ms;
            const int64_t decode_end_ms = decode_end_abs_ms - t_generate_start;
            streaming_decode_ms += decode_ms;
            if (first_decode_ms < 0) {
                first_decode_ms = decode_ms;
                first_pcm_ready_ms = get_time_ms() - t_generate_start;
                first_audio_ready_ms = first_pcm_ready_ms;
            }
            const int64_t window_ready_ms = get_time_ms() - t_generate_start;
            const int64_t window_gap_ms = prev_window_ready_ms >= 0 ? (window_ready_ms - prev_window_ready_ms) : window_ready_ms;
            if (first_window_ready_ms < 0) {
                first_window_ready_ms = window_ready_ms;
            }
            if (prev_window_ready_ms >= 0) {
                if (second_window_gap_ms < 0) {
                    second_window_gap_ms = window_gap_ms;
                }
                if (window_gap_ms > max_window_gap_ms) {
                    max_window_gap_ms = window_gap_ms;
                }
            }

            const int32_t dropped_context_frames = job.new_start - job.ctx_start;
            const size_t drop_samples = std::min(decoded.size(),
                                                 qwen3_decoder_samples_for_frames(dropped_context_frames, sample_rate));
            const size_t appended_samples = decoded.size() - drop_samples;
            const size_t append_offset = result.audio.size();
            result.audio.insert(result.audio.end(), decoded.begin() + (ptrdiff_t) drop_samples, decoded.end());
            const size_t total_samples = result.audio.size();
            emitted_audio_samples.store((int64_t) total_samples);
            const double audio_ms = qwen3_samples_to_ms(appended_samples, sample_rate);
            const double cumulative_audio_ms = qwen3_samples_to_ms(total_samples, sample_rate);
            if (first_playable_50ms_ready_ms < 0 && total_samples >= playable_50ms_samples) {
                first_playable_50ms_ready_ms = get_time_ms() - t_generate_start;
            }
            if (first_playable_150ms_ready_ms < 0 && total_samples >= playable_150ms_samples) {
                first_playable_150ms_ready_ms = get_time_ms() - t_generate_start;
            }
            if (first_playable_200ms_ready_ms < 0 && total_samples >= playable_200ms_samples) {
                first_playable_200ms_ready_ms = get_time_ms() - t_generate_start;
            }
            if (paced_delivery && appended_samples > 0) {
                {
                    std::lock_guard<std::mutex> lock(delivery_state.mutex);
                    delivery_state.buffered.insert(delivery_state.buffered.end(),
                                                  decoded.begin() + (ptrdiff_t) drop_samples,
                                                  decoded.end());
                }
                delivery_state.cv.notify_one();
            }
            if (live_player && appended_samples > 0 && !paced_live_playback) {
                if (first_playback_enqueue_ms < 0) {
                    first_playback_enqueue_ms = get_time_ms() - t_generate_start;
                }
                if (trace_stream_detail) {
                    fprintf(stderr,
                            "[stream] player_write window_index=%d samples=%zu audio_ms=%.1f wall_ms_since_request=%lld wall_ms_since_prev_window=%lld cumulative_audio_ms=%.1f player_queued_audio_ms(before)=%.1f paced=no\n",
                            job.index,
                            appended_samples,
                            audio_ms,
                            (long long) window_ready_ms,
                            (long long) window_gap_ms,
                            cumulative_audio_ms,
                            live_player->queued_audio_ms());
                }
                if (!live_player->write(result.audio, append_offset, appended_samples)) {
                    fprintf(stderr, "Warning: live streaming playback write failed: %s\n",
                            live_player->last_error().c_str());
                    live_player.reset();
                } else {
                    if (trace_stream_detail) {
                        fprintf(stderr,
                                "[stream] player_queue_after_write window_index=%d player_queued_audio_ms=%.1f paced=no\n",
                                job.index,
                                live_player->queued_audio_ms());
                    }
                }
            }

            if (params.print_progress) {
                fprintf(stderr, "%8d  %8d  %8d  %8d  %8d  %9lld  %10zu  %10zu  %s\n",
                        job.index,
                        job.ctx_start,
                        job.new_start,
                        job.end_frame,
                        job.local_frames,
                        (long long) decode_ms,
                        drop_samples,
                        appended_samples,
                        job.is_final ? "ok final" : "ok");
                if (params.dump_streaming_overlap) {
                    fprintf(stderr, "          overlap queued=%lldms dequeue=%lldms wait=%lldms decode_end=%lldms audio_end=%.1fms\n",
                            (long long) job.queued_ms,
                            (long long) dequeue_ms,
                            (long long) (dequeue_ms - job.queued_ms),
                            (long long) decode_end_ms,
                            result.sample_rate > 0 ? (1000.0 * (double) result.audio.size() / (double) result.sample_rate) : 0.0);
                }
            }
            if (trace_stream_detail) {
                fprintf(stderr,
                        "[stream] window_available window_index=%d samples=%zu audio_ms=%.1f wall_ms_since_request=%lld wall_ms_since_prev_window=%lld cumulative_audio_ms=%.1f player_queued_audio_ms=%.1f\n",
                        job.index,
                        appended_samples,
                        audio_ms,
                        (long long) window_ready_ms,
                        (long long) window_gap_ms,
                        cumulative_audio_ms,
                        live_player ? live_player->queued_audio_ms() : 0.0);
            }

            prev_window_ready_ms = window_ready_ms;
            last_completed_frame = job.end_frame;
            return true;
        };

        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::deque<DecodeJob> decode_queue;
        bool producer_done = false;
        std::thread decode_thread;

        auto estimated_player_queued_audio_ms = [&]() -> double {
            if (live_player) {
                return live_player->queued_audio_ms();
            }
            const double emitted_ms = qwen3_samples_to_ms((size_t) std::max<int64_t>(0, emitted_audio_samples.load()), sample_rate);
            const double elapsed_ms = (double) std::max<int64_t>(0, get_time_ms() - t_generate_start);
            return std::max(0.0, emitted_ms - elapsed_ms);
        };

        auto select_steady_window_frames = [&]() -> int32_t {
            if (!adaptive_windows || steady_window <= adaptive_min_window) {
                last_selected_tail_window.store(steady_window);
                return steady_window;
            }

            const double queued_audio_ms = estimated_player_queued_audio_ms();
            int32_t selected = steady_window;
            if (queued_audio_ms <= (double) adaptive_low_watermark_ms) {
                selected = adaptive_min_window;
            } else if (queued_audio_ms < (double) adaptive_high_watermark_ms) {
                const double span_ms = (double) std::max(1, adaptive_high_watermark_ms - adaptive_low_watermark_ms);
                const double normalized = (queued_audio_ms - (double) adaptive_low_watermark_ms) / span_ms;
                const double scaled = (double) adaptive_min_window + normalized * (double) (steady_window - adaptive_min_window);
                selected = clamp_window_frames((int32_t) (scaled + 0.5), adaptive_min_window, steady_window);
            }

            last_selected_tail_window.store(selected);
            return selected;
        };

        int32_t ramp_windows_remaining = ramp_window_count;
        int32_t early_context_windows_remaining = early_context_window_count;
        auto select_next_tail_window_frames = [&]() -> int32_t {
            if (ramp_windows_remaining > 0) {
                --ramp_windows_remaining;
                last_selected_tail_window.store(ramp_window);
                return ramp_window;
            }
            return select_steady_window_frames();
        };

        auto submit_decode_job = [&](DecodeJob && job) -> bool {
            if (params.async_streaming_decode) {
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    decode_queue.push_back(std::move(job));
                }
                queue_cv.notify_one();
                return true;
            }
            return process_decode_job(job);
        };

        auto enqueue_tail_window = [&](const std::vector<int32_t> & all_codes, int32_t end_frame, bool is_final) -> bool {
            if (end_frame <= last_enqueued_frame) {
                return true;
            }
            const int32_t new_start = last_enqueued_frame;
            int32_t effective_context_frames = is_final ? final_context_frames : context_frames;
            if (!is_final && early_context_windows_remaining > 0) {
                effective_context_frames = early_context_frames;
                --early_context_windows_remaining;
            }
            const int32_t ctx_start = std::max<int32_t>(0, new_start - effective_context_frames);
            const int32_t local_frames = end_frame - ctx_start;
            if (local_frames <= 0) {
                return true;
            }
            const auto make_job = [&](int32_t job_ctx_start, int32_t job_new_start, int32_t job_end_frame, bool job_final) {
                DecodeJob job;
                job.index = stream_window_index++;
                job.queued_ms = get_time_ms() - t_generate_start;
                if (job.index == 0 && first_window_queued_ms < 0) {
                    first_window_queued_ms = job.queued_ms;
                }
                job.ctx_start = job_ctx_start;
                job.new_start = job_new_start;
                job.end_frame = job_end_frame;
                job.local_frames = job_end_frame - job_ctx_start;
                job.is_final = job_final;
                const int32_t * local_codes = all_codes.data() + (size_t) job_ctx_start * n_codebooks;
                job.local_codes.assign(local_codes, local_codes + (size_t) job.local_frames * n_codebooks);
                return job;
            };

            last_enqueued_frame = end_frame;

            if (!is_final && new_start > 0 && steady_split_decode_frames > 0 && (end_frame - new_start) > steady_split_decode_frames) {
                int32_t split_start = new_start;
                while (split_start < end_frame) {
                    const int32_t split_end = std::min(end_frame, split_start + steady_split_decode_frames);
                    const int32_t split_ctx_start = std::max<int32_t>(0, split_start - effective_context_frames);
                    DecodeJob split_job = make_job(split_ctx_start, split_start, split_end, false);
                    if (!submit_decode_job(std::move(split_job))) {
                        return false;
                    }
                    split_start = split_end;
                }
                return true;
            }

            DecodeJob job = make_job(ctx_start, new_start, end_frame, is_final);
            return submit_decode_job(std::move(job));
        };

        if (params.async_streaming_decode) {
            decode_thread = std::thread([&]() {
                for (;;) {
                    DecodeJob job;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        queue_cv.wait(lock, [&]() { return producer_done || !decode_queue.empty(); });
                        if (decode_queue.empty()) {
                            if (producer_done) {
                                break;
                            }
                            continue;
                        }
                        job = std::move(decode_queue.front());
                        decode_queue.pop_front();
                    }
                    if (!process_decode_job(job)) {
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        decode_queue.clear();
                        producer_done = true;
                        queue_cv.notify_all();
                        break;
                    }
                }
            });
        }

        auto finish_decode_worker = [&]() {
            if (params.async_streaming_decode) {
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    producer_done = true;
                }
                queue_cv.notify_all();
                if (decode_thread.joinable()) {
                    decode_thread.join();
                }
            }
            if (paced_delivery) {
                {
                    std::lock_guard<std::mutex> lock(delivery_state.mutex);
                    delivery_state.finalized = true;
                }
                delivery_state.cv.notify_all();
                if (delivery_thread.joinable()) {
                    delivery_thread.join();
                }
                if (delivery_state.failed) {
                    stream_error_msg = delivery_state.error_msg.empty() ? "Paced delivery failed" : delivery_state.error_msg;
                    stream_error.store(true);
                }
            }
            if (live_player) {
                const int64_t live_drain_start = get_time_ms();
                live_player->drain();
                live_playback_wait_ms += get_time_ms() - live_drain_start;
            }
        };

        auto on_frame = [&](const std::vector<int32_t> & all_codes, int32_t frames_available, bool is_final) -> bool {
            if (stream_error.load()) {
                return false;
            }
            if (is_final) {
                return enqueue_tail_window(all_codes, frames_available, true);
            }

            // Queue the first tail window immediately on the first callback that
            // satisfies it. This avoids any steady-window scheduling path delaying
            // first PCM after the first generated frame is already available.
            if (last_enqueued_frame == 0 && frames_available >= first_window) {
                if (!enqueue_tail_window(all_codes, first_window, false)) {
                    return false;
                }
                next_decode_end = first_window + select_next_tail_window_frames();
            }

            while (frames_available >= next_decode_end) {
                if (!enqueue_tail_window(all_codes, next_decode_end, false)) {
                    return false;
                }
                next_decode_end += select_next_tail_window_frames();
            }
            return true;
        };

        // Treat the generation callback path as the hot streaming request window.
        // All setup above (optional prewarm, live audio device open, worker thread
        // startup, logging) can happen at conversation/session start. Reset the
        // latency base immediately before the real autoregressive generation begins
        // so first-window queue/PCM/playback numbers measure the actual hot path.
        t_generate_start = get_time_ms();
        if (trace_stream_summary) {
            fprintf(stderr, "[stream] hot_request_start wall_ms=0\n");
        }
        if (live_player) {
            live_player->reset_timing_base(t_generate_start);
        }

        bool generate_ok = self.transformer_.generate_streaming(text_tokens.data(), (int32_t) text_tokens.size(),
                                                                speaker_embedding, params.max_audio_tokens, speech_codes,
                                                                on_frame,
                                                                params.language_id, params.repetition_penalty,
                                                                params.temperature, params.top_k,
                                                                instruct_tokens.empty() ? nullptr : instruct_tokens.data(),
                                                                (int32_t) instruct_tokens.size(),
                                                                params.dump_first_frame_profile ? &first_frame_profile : nullptr);
        finish_decode_worker();

        if (!generate_ok) {
            result.error_msg = stream_error.load() ? stream_error_msg
                                            : "Failed to generate speech codes: " + self.transformer_.get_error();
            return result;
        }
        if (stream_error.load()) {
            result.error_msg = stream_error_msg;
            return result;
        }

        result.t_generate_ms = get_time_ms() - t_generate_start - streaming_decode_ms - live_playback_wait_ms;
        if (result.t_generate_ms < 0) { result.t_generate_ms = 0; }
        result.t_decode_ms = streaming_decode_ms;
        sample_memory("synth/after-generate-streaming");

        n_frames = (int) speech_codes.size() / n_codebooks;

        if (params.print_progress) {
            fprintf(stderr, "Speech codes generated: %d frames x %d codebooks\n", n_frames, n_codebooks);
            fprintf(stderr, "\nIncremental streaming summary:\n");
            fprintf(stderr, "  windows queued/completed:    %d / %d\n", stream_window_index, last_completed_frame > 0 ? stream_window_index : 0);
            fprintf(stderr, "  first window queued:         %lld ms\n", (long long) first_window_queued_ms);
            fprintf(stderr, "  first tail decode:           %lld ms\n", (long long) first_decode_ms);
            fprintf(stderr, "  first PCM ready:             %lld ms\n", (long long) first_pcm_ready_ms);
            fprintf(stderr, "  first 50ms playable:         %lld ms\n", (long long) first_playable_50ms_ready_ms);
            fprintf(stderr, "  first 150ms playable:        %lld ms\n", (long long) first_playable_150ms_ready_ms);
            fprintf(stderr, "  first 200ms playable:        %lld ms\n", (long long) first_playable_200ms_ready_ms);
            if (live_player) {
                const int64_t first_submit = live_player->first_submit_ms();
                fprintf(stderr, "  first playback enqueue:      %lld ms\n", (long long) first_playback_enqueue_ms);
                fprintf(stderr, "  first playback submit:       %lld ms\n", (long long) first_submit);
                fprintf(stderr, "  queued audio at start:       %.1f ms\n", live_player->queued_audio_at_first_submit_ms());
                fprintf(stderr, "  underruns/starvations:       %d\n", live_player->starvation_events());
                if (live_player->first_starvation_ms() >= 0) {
                    fprintf(stderr, "  first starvation:            %lld ms\n", (long long) live_player->first_starvation_ms());
                }
                if (params.live_preroll_ms > 0) {
                    fprintf(stderr, "  live playback preroll:       %d ms\n", params.live_preroll_ms);
                }
            }
            fprintf(stderr, "  first audio ready:           %lld ms (legacy: first PCM ready)\n", (long long) first_audio_ready_ms);
            if (params.dump_first_frame_profile) {
                fprintf(stderr, "\nFirst-frame generation profile:\n");
                fprintf(stderr, "  prefill build complete:      %.1f ms\n", first_frame_profile.prefill_build_ms);
                fprintf(stderr, "  prefill forward complete:    %.1f ms\n", first_frame_profile.prefill_forward_ms);
                fprintf(stderr, "  first CB0 sampled:           %.1f ms\n", first_frame_profile.first_cb0_sample_ms);
                fprintf(stderr, "  first codebook set ready:    %.1f ms\n", first_frame_profile.first_code_predictor_ms);
                fprintf(stderr, "  first frame callback:        %.1f ms\n", first_frame_profile.first_frame_callback_ms);
                fprintf(stderr, "  first window queued:         %lld ms\n", (long long) first_window_queued_ms);
                if (first_pcm_ready_ms >= 0) {
                    fprintf(stderr, "  first decode + playback gap: %lld ms\n", (long long) (first_pcm_ready_ms - first_window_queued_ms));
                }
            }
            fprintf(stderr, "  streaming decode total:      %lld ms\n", (long long) streaming_decode_ms);
            fprintf(stderr, "  generated audio samples:     %zu\n", result.audio.size());
            fprintf(stderr, "  last tail window size:       %d frames\n", last_selected_tail_window.load());
            if (paced_delivery) {
                fprintf(stderr, "  paced chunks delivered:      %d\n", delivery_state.delivered_chunks);
                fprintf(stderr, "  first paced chunk:           %lld ms\n", (long long) delivery_state.first_emit_wall_ms);
                fprintf(stderr, "  second paced gap:            %lld ms\n", (long long) delivery_state.second_emit_gap_ms);
                fprintf(stderr, "  max paced gap:               %lld ms\n", (long long) delivery_state.max_emit_gap_ms);
            }
            if (live_playback_wait_ms > 0) {
                fprintf(stderr, "  live playback drain wait:    %lld ms (excluded from synthesis timing)\n",
                        (long long) live_playback_wait_ms);
            }
            fprintf(stderr, "\nStreaming cadence table:\n");
            fprintf(stderr, "  first playable ms:           %lld\n", (long long) first_playable_150ms_ready_ms);
            fprintf(stderr, "  first window ms:             %lld\n", (long long) first_window_ready_ms);
            fprintf(stderr, "  second window gap ms:        %lld\n", (long long) second_window_gap_ms);
            fprintf(stderr, "  max window gap ms:           %lld\n", (long long) max_window_gap_ms);
            if (live_player) {
                fprintf(stderr, "  queued audio at playback:    %.1f ms\n", live_player->queued_audio_at_first_submit_ms());
                fprintf(stderr, "  underruns:                   %d\n", live_player->starvation_events());
            } else {
                fprintf(stderr, "  queued audio at playback:    0.0 ms\n");
                fprintf(stderr, "  underruns:                   0\n");
            }
        }

        if (n_frames == 0) {
            result.error_msg = "No speech codes generated";
            return result;
        }
        if (result.audio.empty()) {
            result.error_msg = "Streaming decode produced no audio";
            return result;
        }
    } else {
        if (!self.transformer_.generate(text_tokens.data(), (int32_t) text_tokens.size(),
                                        speaker_embedding, params.max_audio_tokens, speech_codes,
                                        params.language_id, params.repetition_penalty,
                                        params.temperature, params.top_k,
                                        instruct_tokens.empty() ? nullptr : instruct_tokens.data(),
                                        (int32_t) instruct_tokens.size())) {
            result.error_msg = "Failed to generate speech codes: " + self.transformer_.get_error();
            return result;
        }
        result.t_generate_ms = get_time_ms() - t_generate_start;
        sample_memory("synth/after-generate");

        n_frames = (int) speech_codes.size() / n_codebooks;

        if (params.print_progress) {
            fprintf(stderr, "Speech codes generated: %d frames x %d codebooks\n", n_frames, n_codebooks);
        }

        if (n_frames == 0) {
            result.error_msg = "No speech codes generated";
            return result;
        }

        if (self.low_mem_mode_) {
            self.transformer_.unload_model();
            self.transformer_loaded_ = false;
            sample_memory("synth/after-transformer-unload");
        }

        t_decode_start = get_time_ms();
        if (!self.decoder_loaded_) {
            int64_t t_decoder_load_start = get_time_ms();
            if (self.decoder_model_path_.empty()) {
                result.error_msg = "Internal error: missing vocoder model path";
                return result;
            }
            if (!self.audio_decoder_.load_model(self.decoder_model_path_)) {
                result.error_msg = "Failed to load vocoder: " + self.audio_decoder_.get_error();
                return result;
            }
            self.decoder_loaded_ = true;
            if (params.print_timing) {
                fprintf(stderr, "  Vocoder lazy-loaded in %lld ms\n",
                        (long long) (get_time_ms() - t_decoder_load_start));
                sample_memory("synth/after-vocoder-load");
            }
        }

        if (!self.audio_decoder_.decode(speech_codes.data(), n_frames, result.audio)) {
            result.error_msg = "Failed to decode speech codes: " + self.audio_decoder_.get_error();
            return result;
        }
        result.t_decode_ms = get_time_ms() - t_decode_start;
        sample_memory("synth/after-decode");
    }

    if (self.low_mem_mode_) {
        self.audio_decoder_.unload_model();
        self.decoder_loaded_ = false;
        sample_memory("synth/after-vocoder-unload");
    }

    result.sample_rate = self.audio_decoder_.get_config().sample_rate;
    result.success = true;
    result.t_total_ms = get_time_ms() - t_total_start - live_playback_wait_ms - excluded_timing_ms;
    if (result.t_total_ms < 0) { result.t_total_ms = 0; }
    sample_memory("synth/end");

    if (params.print_timing) {
        const double audio_sec = result.sample_rate > 0
            ? (double) result.audio.size() / (double) result.sample_rate : 0.0;
        const double wall_sec = (double) result.t_total_ms / 1000.0;
        const double realtime_factor = audio_sec > 0.0 ? wall_sec / audio_sec : 0.0;
        const double x_realtime = wall_sec > 0.0 ? audio_sec / wall_sec : 0.0;
        fprintf(stderr, "\nTiming:\n");
        fprintf(stderr, "  Tokenization:    %lld ms\n", (long long) result.t_tokenize_ms);
        fprintf(stderr, "  Speaker encode:  %lld ms\n", (long long) result.t_encode_ms);
        fprintf(stderr, "  Code generation: %lld ms\n", (long long) result.t_generate_ms);
        fprintf(stderr, "  Vocoder decode:  %lld ms\n", (long long) result.t_decode_ms);
        fprintf(stderr, "  Total:           %lld ms\n", (long long) result.t_total_ms);
        fprintf(stderr, "  Audio duration:  %.2f s\n", audio_sec);
        fprintf(stderr, "  Throughput:      %.2fx realtime (RTF=%.3f)\n", x_realtime, realtime_factor);
        fprintf(stderr, "\nMemory:\n");
        fprintf(stderr, "  RSS start/end:   %s -> %s\n",
                format_bytes(result.mem_rss_start_bytes).c_str(),
                format_bytes(result.mem_rss_end_bytes).c_str());
        fprintf(stderr, "  RSS peak:        %s\n",
                format_bytes(result.mem_rss_peak_bytes).c_str());
        fprintf(stderr, "  Phys start/end:  %s -> %s\n",
                format_bytes(result.mem_phys_start_bytes).c_str(),
                format_bytes(result.mem_phys_end_bytes).c_str());
        fprintf(stderr, "  Phys peak:       %s\n",
                format_bytes(result.mem_phys_peak_bytes).c_str());
    }

    if (trace_stream_summary) {
        fprintf(stderr, "[stream] synthesis_completed wall_ms=%lld success=%s audio_ms=%.1f\n",
                (long long) result.t_total_ms,
                result.success ? "yes" : "no",
                qwen3_samples_to_ms(result.audio.size(), result.sample_rate));
    }

    return result;
}

} // namespace qwen3_tts
