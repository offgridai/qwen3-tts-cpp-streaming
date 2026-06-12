#include "offgrid_tts/Qwen3StreamingTts.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
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

namespace {

int64_t cli_now_ms() {
#ifdef _WIN32
    return (int64_t) GetTickCount64();
#else
    using clock = std::chrono::steady_clock;
    return (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now().time_since_epoch()).count();
#endif
}

#ifdef _WIN32
class WaveOutPcmPlayer {
public:
    ~WaveOutPcmPlayer() { close(); }

    bool open(int32_t sample_rate) {
        close();
        sample_rate_ = sample_rate;
        starvation_active_ = false;
        submitted_samples_total_ = 0;
        first_submit_logged_ = false;
        stopping_ = false;
        queue_.clear();
        retire_completed(true);

        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 1;
        fmt.nSamplesPerSec = (DWORD) sample_rate_;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = (WORD) (fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        MMRESULT mm = waveOutOpen(&wave_out_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
        if (mm != MMSYSERR_NOERROR) {
            last_error_ = "waveOutOpen failed";
            wave_out_ = nullptr;
            return false;
        }

        worker_ = std::thread([this]() { worker_loop(); });
        return true;
    }

    bool write(std::vector<int16_t> pcm) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return false;
            }
            if (!pcm.empty()) {
                queue_.push_back(std::move(pcm));
            }
        }
        cv_.notify_all();
        return true;
    }

    void drain() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty() && pending_headers_ == 0) {
                    break;
                }
            }
            Sleep(10);
        }
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }

        if (wave_out_) {
            waveOutReset(wave_out_);
            waveOutClose(wave_out_);
            wave_out_ = nullptr;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        pending_.clear();
        pending_headers_ = 0;
        starvation_active_ = false;
        stopping_ = false;
        submitted_samples_total_ = 0;
        first_submit_clock_ms_ = 0;
        first_submit_logged_ = false;
    }

    int starvation_events() const { return starvation_events_; }
    const std::string & last_error() const { return last_error_; }
    double queued_audio_ms() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sample_rate_ <= 0) {
            return 0.0;
        }
        return 1000.0 * (double) queued_samples_locked() / (double) sample_rate_;
    }

private:
    static int64_t get_time_ms() {
        return (int64_t) GetTickCount64();
    }

    void worker_loop() {
        while (true) {
            std::vector<int16_t> pcm;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                retire_completed_unlocked();

                const size_t queued_now = queued_samples_locked();
                if (!stopping_ && first_submit_logged_ && queued_now == 0 && !starvation_active_) {
                    starvation_active_ = true;
                    ++starvation_events_;
                    std::cerr << "[waveout-player] starvation event=" << starvation_events_ << "\n";
                }

                cv_.wait_for(lock, std::chrono::milliseconds(5), [&]() {
                    return stopping_ || !queue_.empty();
                });

                retire_completed_unlocked();

                if (!queue_.empty()) {
                    pcm = std::move(queue_.front());
                    queue_.pop_front();
                    if (starvation_active_) {
                        starvation_active_ = false;
                        std::cerr << "[waveout-player] resume buffered_ms="
                                  << (1000.0 * (double) queued_samples_locked() / (double) sample_rate_) << "\n";
                    }
                } else if (stopping_) {
                    break;
                } else {
                    continue;
                }
            }

            if (!submit_chunk(std::move(pcm))) {
                return;
            }
        }

        retire_completed(true);
    }

    bool submit_chunk(std::vector<int16_t> chunk) {
        if (!wave_out_ || chunk.empty()) {
            return true;
        }

        auto pending = std::make_unique<PendingChunk>();
        pending->pcm = std::move(chunk);
        pending->hdr.lpData = reinterpret_cast<LPSTR>(pending->pcm.data());
        pending->hdr.dwBufferLength = (DWORD) (pending->pcm.size() * sizeof(int16_t));
        if (waveOutPrepareHeader(wave_out_, &pending->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            last_error_ = "waveOutPrepareHeader failed";
            return false;
        }
        if (waveOutWrite(wave_out_, &pending->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            last_error_ = "waveOutWrite failed";
            waveOutUnprepareHeader(wave_out_, &pending->hdr, sizeof(WAVEHDR));
            return false;
        }

        submitted_samples_total_ += (int64_t) pending->pcm.size();
        if (!first_submit_logged_) {
            first_submit_clock_ms_ = get_time_ms();
            first_submit_logged_ = true;
            const double queued_ms = (1000.0 * (double) queued_samples() / (double) sample_rate_);
            std::cerr << "[waveout-player] playback_start buffered_ms=" << queued_ms << "\n";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(std::move(pending));
        pending_headers_ = (int) pending_.size();
        return true;
    }

    size_t queued_samples() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queued_samples_locked();
    }

    size_t queued_samples_locked() const {
        size_t total = 0;
        for (const auto & queued_pcm : queue_) {
            total += queued_pcm.size();
        }
        total += submitted_remaining_samples_locked();
        return total;
    }

    size_t submitted_remaining_samples_locked() const {
        return (size_t) std::max<int64_t>(0, submitted_samples_total_ - estimated_played_samples_locked());
    }

    int64_t estimated_played_samples_locked() const {
        if (!first_submit_logged_ || sample_rate_ <= 0) {
            return 0;
        }
        const int64_t elapsed_ms = get_time_ms() - first_submit_clock_ms_;
        return ((int64_t) sample_rate_ * std::max<int64_t>(0, elapsed_ms)) / 1000;
    }

    void retire_completed(bool wait_all = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        retire_completed_unlocked(wait_all);
    }

    void retire_completed_unlocked(bool wait_all = false) {
        for (;;) {
            bool retired_any = false;
            auto it = pending_.begin();
            while (it != pending_.end()) {
                WAVEHDR * hdr = &(*it)->hdr;
                if ((hdr->dwFlags & WHDR_DONE) == 0) {
                    ++it;
                    continue;
                }
                waveOutUnprepareHeader(wave_out_, hdr, sizeof(WAVEHDR));
                it = pending_.erase(it);
                retired_any = true;
            }
            pending_headers_ = (int) pending_.size();
            if (!wait_all || pending_.empty()) {
                break;
            }
            if (!retired_any) {
                Sleep(5);
            }
        }
    }

    struct PendingChunk {
        std::vector<int16_t> pcm;
        WAVEHDR hdr{};
    };

    HWAVEOUT wave_out_ = nullptr;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<int16_t>> queue_;
    std::deque<std::unique_ptr<PendingChunk>> pending_;
    int32_t sample_rate_ = 24000;
    bool starvation_active_ = false;
    bool stopping_ = false;
    int starvation_events_ = 0;
    int pending_headers_ = 0;
    int64_t submitted_samples_total_ = 0;
    int64_t first_submit_clock_ms_ = 0;
    bool first_submit_logged_ = false;
    std::string last_error_;
};

class LineCoachProxyAudioPlayer {
public:
    ~LineCoachProxyAudioPlayer() { close(); }

    bool open(int32_t sample_rate,
              int32_t preroll_ms,
              int32_t buffer_floor_ms,
              int32_t coalesce_ms,
              int32_t max_burst_ms,
              int64_t request_start_ms = 0) {
        close();
        sample_rate_ = sample_rate;
        bytes_per_second_ = sample_rate_ * (int32_t) sizeof(int16_t);
        initial_preroll_samples_ = align_samples(ms_to_samples(sample_rate_, preroll_ms));
        maintain_floor_samples_ = align_samples(ms_to_samples(sample_rate_, buffer_floor_ms));
        underrun_floor_samples_ = align_samples(ms_to_samples(sample_rate_, std::clamp(std::max(buffer_floor_ms / 2, 80), 80, 160)));
        resume_preroll_samples_ = align_samples(ms_to_samples(sample_rate_, std::clamp(std::max(preroll_ms, buffer_floor_ms), 100, 500)));
        coalesce_ms_ = coalesce_ms;
        max_burst_ms_ = max_burst_ms;
        request_start_ms_ = request_start_ms > 0 ? request_start_ms : get_time_ms();
        stream_open_ = true;
        if (!sink_.open(sample_rate_)) {
            last_error_ = sink_.last_error();
            return false;
        }
        return true;
    }

    void push(const TtsStreamChunk & chunk) {
        if (sample_rate_ <= 0) {
            if (!open(chunk.sample_rate, 350, 250, 40, 500)) {
                return;
            }
        }

        std::vector<int16_t> pcm;
        pcm.reserve(chunk.samples.size());
        for (float v : chunk.samples) {
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm.push_back((int16_t) (v * 32767.0f));
        }

        int received_window_index = 0;
        size_t chunk_samples = pcm.size();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sample_rate_ != chunk.sample_rate && chunk.sample_rate > 0) {
                sample_rate_ = chunk.sample_rate;
                bytes_per_second_ = sample_rate_ * (int32_t) sizeof(int16_t);
            }
            pending_output_pcm_.insert(pending_output_pcm_.end(), pcm.begin(), pcm.end());
            received_window_index = ++received_output_chunk_count_;
            if (chunk.is_final) {
                stream_open_ = false;
            }
        }

        std::cerr << "[linecoach-proxy] window_available index=" << received_window_index
                  << " samples=" << chunk_samples
                  << " audio_ms=" << samples_to_ms(chunk_samples)
                  << " queued_audio_ms=" << samples_to_ms(get_estimated_buffered_playback_samples())
                  << " stream_open=" << (is_stream_open() ? "true" : "false")
                  << "\n";

        flush_pending_output_pcm(false);
        start_output_playback_if_ready(false);
        update_output_playback_recovery();

        if (chunk.is_final) {
            flush_pending_output_pcm(true);
            start_output_playback_if_ready(true);
            update_output_playback_recovery();
        }
    }

    void drain() {
        flush_pending_output_pcm(true);
        start_output_playback_if_ready(true);
        sink_.drain();
    }

    void close() {
        sink_.close();
        std::lock_guard<std::mutex> lock(mutex_);
        pending_output_pcm_.clear();
        pending_output_pcm_read_offset_ = 0;
        playback_started_ = false;
        playback_paused_for_underrun_ = false;
        stream_open_ = false;
        playback_preroll_filled_marked_ = false;
        received_output_chunk_count_ = 0;
        submitted_output_chunk_count_ = 0;
        submitted_output_samples_ = 0;
        output_playback_start_ms_ = 0;
        output_playback_resume_ms_ = 0;
        consumed_playback_ms_ = 0.0;
        output_underrun_zero_since_ms_ = 0;
        request_start_ms_ = 0;
        sample_rate_ = 0;
        bytes_per_second_ = 0;
    }

    const std::string & last_error() const { return last_error_; }

private:
    static int64_t get_time_ms() {
        return (int64_t) GetTickCount64();
    }

    static size_t ms_to_samples(int32_t sample_rate, int32_t ms) {
        if (sample_rate <= 0 || ms <= 0) {
            return 0;
        }
        return (size_t) (((int64_t) sample_rate * ms) / 1000);
    }

    size_t align_samples(size_t samples) const {
        return samples;
    }

    double samples_to_ms(size_t samples) const {
        if (sample_rate_ <= 0) {
            return 0.0;
        }
        return 1000.0 * (double) samples / (double) sample_rate_;
    }

    bool is_stream_open() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stream_open_;
    }

    size_t get_current_output_playback_samples_unlocked() const {
        if (!playback_started_) {
            return 0;
        }
        double playback_ms = consumed_playback_ms_;
        if (!playback_paused_for_underrun_ && output_playback_resume_ms_ > 0) {
            playback_ms += (double) std::max<int64_t>(0, get_time_ms() - output_playback_resume_ms_);
        }
        return (size_t) (((double) sample_rate_ * playback_ms) / 1000.0);
    }

    size_t get_estimated_buffered_playback_samples_unlocked() const {
        const size_t pending_samples = pending_output_pcm_.size() > pending_output_pcm_read_offset_
            ? (pending_output_pcm_.size() - pending_output_pcm_read_offset_)
            : 0;
        size_t submitted_remaining_samples = submitted_output_samples_;
        if (playback_started_) {
            const size_t consumed_samples = get_current_output_playback_samples_unlocked();
            submitted_remaining_samples = submitted_output_samples_ > consumed_samples
                ? (submitted_output_samples_ - consumed_samples)
                : 0;
        }
        return submitted_remaining_samples + pending_samples;
    }

    size_t get_estimated_buffered_playback_samples() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return get_estimated_buffered_playback_samples_unlocked();
    }

    void flush_pending_output_pcm(bool force_flush_all) {
        std::vector<int16_t> chunk;
        int submit_index = 0;
        int received_windows = 0;
        size_t pending_remaining_samples = 0;
        size_t submitted_chunk_samples = 0;
        size_t total_submitted_samples = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t pending_samples = pending_output_pcm_.size() > pending_output_pcm_read_offset_
                ? (pending_output_pcm_.size() - pending_output_pcm_read_offset_)
                : 0;
            if (pending_samples == 0) {
                return;
            }
            if (!playback_started_ && !force_flush_all && pending_samples < initial_preroll_samples_) {
                return;
            }

            const size_t samples_to_write = pending_samples;
            submitted_chunk_samples = samples_to_write;
            chunk.assign(pending_output_pcm_.begin() + (ptrdiff_t) pending_output_pcm_read_offset_,
                         pending_output_pcm_.begin() + (ptrdiff_t) (pending_output_pcm_read_offset_ + samples_to_write));
            pending_output_pcm_read_offset_ += samples_to_write;
            submitted_output_samples_ += samples_to_write;
            total_submitted_samples = submitted_output_samples_;
            submit_index = ++submitted_output_chunk_count_;
            received_windows = received_output_chunk_count_;

            if (pending_output_pcm_read_offset_ >= pending_output_pcm_.size()) {
                pending_output_pcm_.clear();
                pending_output_pcm_read_offset_ = 0;
            } else if (pending_output_pcm_read_offset_ >= 65536) {
                pending_output_pcm_.erase(pending_output_pcm_.begin(),
                                          pending_output_pcm_.begin() + (ptrdiff_t) pending_output_pcm_read_offset_);
                pending_output_pcm_read_offset_ = 0;
            }
            pending_remaining_samples = pending_output_pcm_.size() > pending_output_pcm_read_offset_
                ? (pending_output_pcm_.size() - pending_output_pcm_read_offset_)
                : 0;
        }

        if (!sink_.write(std::move(chunk))) {
            last_error_ = sink_.last_error();
            return;
        }

        const size_t submitted_samples = get_estimated_buffered_playback_samples();
        std::cerr << "[TTS_TRACE] T6 audio_submitted_to_soundwave"
                  << " submit_index=" << submit_index
                  << " submit_audio_ms=" << samples_to_ms(submitted_chunk_samples)
                  << " total_submitted_audio_ms=" << samples_to_ms(total_submitted_samples)
                  << " total_buffered_audio_ms=" << samples_to_ms(submitted_samples)
                  << " received_windows=" << received_windows
                  << " pending_remaining_audio_ms=" << samples_to_ms(pending_remaining_samples)
                  << "\n";
    }

    void start_output_playback_if_ready(bool force_start) {
        size_t available_samples = 0;
        bool should_mark_preroll = false;
        bool should_start = false;
        size_t required_preroll_samples = 0;
        size_t required_startup_samples = 0;
        size_t started_after_window = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            available_samples = get_estimated_buffered_playback_samples_unlocked();
            if (available_samples == 0) {
                return;
            }
            required_preroll_samples = initial_preroll_samples_;
            required_startup_samples = playback_started_ ? resume_preroll_samples_ : required_preroll_samples;
            if (!force_start && available_samples < required_startup_samples) {
                return;
            }
            if (!playback_started_ && !playback_preroll_filled_marked_ && available_samples >= required_preroll_samples) {
                playback_preroll_filled_marked_ = true;
                should_mark_preroll = true;
            }
            if (!playback_started_) {
                playback_started_ = true;
                playback_paused_for_underrun_ = false;
                output_playback_start_ms_ = get_time_ms();
                output_playback_resume_ms_ = output_playback_start_ms_;
                consumed_playback_ms_ = 0.0;
                output_underrun_zero_since_ms_ = 0;
                started_after_window = (size_t) received_output_chunk_count_;
                should_start = true;
            }
        }

        if (should_mark_preroll) {
            std::cerr << "[linecoach-proxy] playback_preroll_filled"
                      << " wall_ms_since_request=" << std::max<int64_t>(0, get_time_ms() - request_start_ms_)
                      << " queued_audio_ms=" << samples_to_ms(available_samples)
                      << " required_audio_ms=" << samples_to_ms(required_preroll_samples)
                      << "\n";
        }
        if (should_start) {
            std::cerr << "[TTS_TRACE] T7 playback_started"
                      << " wall_ms_since_request=" << std::max<int64_t>(0, get_time_ms() - request_start_ms_)
                      << " queued_audio_ms=" << samples_to_ms(available_samples)
                      << " live_preroll_ms=" << samples_to_ms(required_preroll_samples)
                      << " started_after_window=" << started_after_window
                      << " submitted_windows=" << submitted_output_chunk_count_
                      << " force=" << (force_start ? "true" : "false")
                      << "\n";
        }
    }

    void pause_output_playback_for_underrun() {
        size_t buffered_samples = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!playback_started_) {
                return;
            }
            buffered_samples = get_estimated_buffered_playback_samples_unlocked();
            std::cerr << "[TTS_TRACE] T8 low_buffer_warning"
                      << " wall_ms_since_request=" << std::max<int64_t>(0, get_time_ms() - request_start_ms_)
                      << " buffered_audio_ms=" << samples_to_ms(buffered_samples)
                      << " stream_open=" << (stream_open_ ? "true" : "false")
                      << " paused=" << (playback_paused_for_underrun_ ? "true" : "false")
                      << "\n";
            if (playback_paused_for_underrun_) {
                return;
            }
            consumed_playback_ms_ = samples_to_ms(get_current_output_playback_samples_unlocked());
            output_playback_resume_ms_ = 0;
            playback_paused_for_underrun_ = true;
            output_underrun_zero_since_ms_ = 0;
        }

        std::cerr << "[TTS_TRACE] T8b playback_paused_for_underrun"
                  << " wall_ms_since_request=" << std::max<int64_t>(0, get_time_ms() - request_start_ms_)
                  << " consumed_audio_ms=" << consumed_playback_ms_
                  << " buffered_audio_ms=" << samples_to_ms(buffered_samples)
                  << "\n";
    }

    void update_output_playback_recovery() {
        bool should_resume = false;
        bool should_warn = false;
        bool should_pause = false;
        size_t buffered_samples = 0;
        bool stream_open = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!playback_started_) {
                return;
            }
            buffered_samples = get_estimated_buffered_playback_samples_unlocked();
            stream_open = stream_open_;
            if (playback_paused_for_underrun_) {
                const bool can_resume = buffered_samples > 0 && (!stream_open_ || buffered_samples >= resume_preroll_samples_);
                if (can_resume) {
                    playback_paused_for_underrun_ = false;
                    output_playback_resume_ms_ = get_time_ms();
                    output_underrun_zero_since_ms_ = 0;
                    should_resume = true;
                }
            } else if (stream_open_ && buffered_samples == 0) {
                const int64_t now_ms = get_time_ms();
                if (output_underrun_zero_since_ms_ <= 0) {
                    output_underrun_zero_since_ms_ = now_ms;
                } else if (now_ms - output_underrun_zero_since_ms_ >= 50) {
                    should_pause = true;
                }
            } else {
                output_underrun_zero_since_ms_ = 0;
                if (stream_open_ && buffered_samples <= underrun_floor_samples_) {
                    should_warn = true;
                }
            }
        }

        if (should_pause) {
            pause_output_playback_for_underrun();
            return;
        }
        if (should_resume) {
            std::cerr << "[TTS_TRACE] T9 playback_resumed_after_underrun"
                      << " wall_ms_since_request=" << std::max<int64_t>(0, get_time_ms() - request_start_ms_)
                      << " buffered_audio_ms=" << samples_to_ms(buffered_samples)
                      << " stream_open=" << (stream_open ? "true" : "false")
                      << "\n";
            return;
        }
        if (should_warn) {
            std::cerr << "[TTS_TRACE] T8 low_buffer_warning"
                      << " wall_ms_since_request=" << std::max<int64_t>(0, get_time_ms() - request_start_ms_)
                      << " buffered_audio_ms=" << samples_to_ms(buffered_samples)
                      << " stream_open=" << (stream_open ? "true" : "false")
                      << " paused=false\n";
        }
    }

    WaveOutPcmPlayer sink_;
    mutable std::mutex mutex_;
    std::vector<int16_t> pending_output_pcm_;
    size_t pending_output_pcm_read_offset_ = 0;
    int32_t sample_rate_ = 0;
    int32_t bytes_per_second_ = 0;
    size_t initial_preroll_samples_ = 0;
    size_t maintain_floor_samples_ = 0;
    size_t underrun_floor_samples_ = 0;
    size_t resume_preroll_samples_ = 0;
    int32_t coalesce_ms_ = 0;
    int32_t max_burst_ms_ = 0;
    bool playback_started_ = false;
    bool playback_paused_for_underrun_ = false;
    bool stream_open_ = false;
    bool playback_preroll_filled_marked_ = false;
    int received_output_chunk_count_ = 0;
    int submitted_output_chunk_count_ = 0;
    size_t submitted_output_samples_ = 0;
    int64_t output_playback_start_ms_ = 0;
    int64_t output_playback_resume_ms_ = 0;
    double consumed_playback_ms_ = 0.0;
    int64_t output_underrun_zero_since_ms_ = 0;
    int64_t request_start_ms_ = 0;
    std::string last_error_;
};
#endif

} // namespace

int main(int argc, char** argv) {
    std::string model_dir = "models";
    std::string speaker_embedding;
    std::string text = "Hello. Welcome to Alfie's Bodega. I'm Alfie. What can I get for you today?";
    bool simulate_stream_callback = false;
    bool callback_playback = false;
    int callback_preroll_ms = 350;
    int callback_buffer_floor_ms = 250;
    int callback_coalesce_ms = 40;
    int callback_max_burst_ms = 500;
    int repeat = 1;

    TtsStreamOptions options;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "-m" || a == "--model") model_dir = next();
        else if (a == "--voice-design") options.voice_design = true;
        else if (a == "--speaker-embedding") speaker_embedding = next();
        else if (a == "-t" || a == "--text") text = next();
        else if (a == "-o" || a == "--output") options.output_wav = next();
        else if (a == "--model-identifier" || a == "--model-name") options.model_identifier = next();
        else if (a == "--voice-design-instruct") options.instruction = next();
        else if (a == "--instruction" || a == "--instruct") options.instruction = next();
        else if (a == "--max-tokens") options.max_audio_tokens = std::stoi(next());
        else if (a == "--temperature") options.temperature = std::stof(next());
        else if (a == "--top-k") options.top_k = std::stoi(next());
        else if (a == "--top-p") options.top_p = std::stof(next());
        else if (a == "--repetition-penalty") options.repetition_penalty = std::stof(next());
        else if (a == "--quiet") { options.print_progress = false; options.print_timing = true; }
        else if (a == "--quiet-all") { options.print_progress = false; options.print_timing = false; }
        else if (a == "--verbose") { options.print_progress = true; options.print_timing = true; }
        else if (a == "--callback-playback" || a == "--linecoach-proxy-playback") callback_playback = true;
        else if (a == "--callback-preroll-ms") callback_preroll_ms = std::stoi(next());
        else if (a == "--callback-buffer-floor-ms") callback_buffer_floor_ms = std::stoi(next());
        else if (a == "--callback-coalesce-ms") callback_coalesce_ms = std::stoi(next());
        else if (a == "--callback-max-burst-ms") callback_max_burst_ms = std::stoi(next());
        else if (a == "--tts-profile") {
            const std::string profile = next();
            if (profile == "realtime") {
                options.model_identifier = "qwen3-tts-0.6b-f16";
                options.live_preroll_ms = 150;
                options.first_tail_window_frames = 3;
                options.ramp_tail_window_frames = 5;
                options.ramp_tail_window_count = 2;
                options.steady_tail_window_frames = 8;
                options.context_frames = 3;
                options.early_context_frames = 2;
                options.early_context_window_count = 2;
                options.final_context_frames = 4;
            } else if (profile == "memory-saver") {
                options.model_identifier = "qwen3-tts-0.6b-q5_k";
                options.live_preroll_ms = 1000;
                options.first_tail_window_frames = 3;
                options.ramp_tail_window_frames = 5;
                options.ramp_tail_window_count = 2;
                options.steady_tail_window_frames = 8;
                options.context_frames = 3;
                options.early_context_frames = 2;
                options.early_context_window_count = 2;
                options.final_context_frames = 4;
            } else if (profile == "ultra-low") {
                options.model_identifier = "qwen3-tts-0.6b-q4_k";
                options.live_preroll_ms = 2000;
                options.first_tail_window_frames = 3;
                options.ramp_tail_window_frames = 5;
                options.ramp_tail_window_count = 2;
                options.steady_tail_window_frames = 8;
                options.context_frames = 3;
                options.early_context_frames = 2;
                options.early_context_window_count = 2;
                options.final_context_frames = 4;
            } else if (profile == "offgrid-callback") {
                options.live_preroll_ms = 150;
                options.first_tail_window_frames = 3;
                options.ramp_tail_window_frames = 6;
                options.ramp_tail_window_count = 0;
                options.steady_tail_window_frames = 12;
                options.context_frames = 2;
                options.early_context_frames = 1;
                options.early_context_window_count = 2;
                options.final_context_frames = 4;
                options.adaptive_steady_windows = false;
                options.delivery_chunk_ms = 40;
                options.delivery_start_buffer_ms = 40;
                options.delivery_target_lead_ms = 400;
                options.steady_split_decode_frames = 6;
            } else {
                std::cerr << "Unknown --tts-profile '" << profile << "'. Expected realtime, memory-saver, ultra-low, or offgrid-callback.\n";
                return 2;
            }
        }
        else if (a == "--live-preroll-ms") options.live_preroll_ms = std::stoi(next());
        else if (a == "--play-streaming") options.play_streaming = true;
        else if (a == "--no-play-streaming") options.play_streaming = false;
        else if (a == "--dump-first-frame-profile") options.dump_first_frame_profile = true;
        else if (a == "--dump-streaming-overlap") options.dump_streaming_overlap = true;
        else if (a == "--first-tail-window-frames") options.first_tail_window_frames = std::stoi(next());
        else if (a == "--ramp-tail-window-frames") options.ramp_tail_window_frames = std::stoi(next());
        else if (a == "--ramp-tail-window-count") options.ramp_tail_window_count = std::stoi(next());
        else if (a == "--steady-tail-window-frames") options.steady_tail_window_frames = std::stoi(next());
        else if (a == "--context-frames") options.context_frames = std::stoi(next());
        else if (a == "--early-context-frames") options.early_context_frames = std::stoi(next());
        else if (a == "--early-context-window-count") options.early_context_window_count = std::stoi(next());
        else if (a == "--final-context-frames") options.final_context_frames = std::stoi(next());
        else if (a == "--adaptive-steady-windows") options.adaptive_steady_windows = true;
        else if (a == "--no-adaptive-steady-windows") options.adaptive_steady_windows = false;
        else if (a == "--adaptive-min-tail-window-frames") options.adaptive_min_tail_window_frames = std::stoi(next());
        else if (a == "--adaptive-low-watermark-ms") options.adaptive_low_watermark_ms = std::stoi(next());
        else if (a == "--adaptive-high-watermark-ms") options.adaptive_high_watermark_ms = std::stoi(next());
        else if (a == "--paced-audio-delivery") options.paced_audio_delivery = true;
        else if (a == "--no-paced-audio-delivery") options.paced_audio_delivery = false;
        else if (a == "--delivery-chunk-ms") options.delivery_chunk_ms = std::stoi(next());
        else if (a == "--delivery-start-buffer-ms") options.delivery_start_buffer_ms = std::stoi(next());
        else if (a == "--delivery-target-lead-ms") options.delivery_target_lead_ms = std::stoi(next());
        else if (a == "--paced-live-playback") options.paced_live_playback = true;
        else if (a == "--no-paced-live-playback") options.paced_live_playback = false;
        else if (a == "--steady-split-decode-frames") options.steady_split_decode_frames = std::stoi(next());
        else if (a == "--async-streaming-decode") options.async_streaming_decode = true;
        else if (a == "--no-async-streaming-decode") options.async_streaming_decode = false;
        else if (a == "--cache-instruction-tokens") options.cache_instruction_tokens = true;
        else if (a == "--no-cache-instruction-tokens") options.cache_instruction_tokens = false;
        else if (a == "--instruction-cache-key") options.instruction_cache_key = next();
        else if (a == "--warm-voice-profile") options.warm_voice_profile = true;
        else if (a == "--warm-voice-profile-key") options.warm_voice_profile_key = next();
        else if (a == "--warmup-text") options.warmup_text = next();
        else if (a == "--repeat") repeat = std::max(1, std::stoi(next()));
        else if (a == "--simulate-stream-callback") simulate_stream_callback = true;
        else if (a == "-h" || a == "--help") {
            std::cout << "Usage: qwen3_streaming_cli -m models --model-identifier qwen3-tts-0.6b-f16 --speaker-embedding speaker.json -t text -o out.wav\n"
                      << "  --voice-design\n"
                      << "  --voice-design-instruct <text>\n"
                      << "  --instruction <text>\n"
                      << "  --max-tokens <int>\n"
                      << "  --temperature <float>\n"
                      << "  --top-k <int>\n"
                      << "  --top-p <float>\n"
                      << "  --repetition-penalty <float>\n"
                      << "  --quiet | --quiet-all | --verbose\n"
                      << "  --tts-profile realtime|memory-saver|ultra-low|offgrid-callback\n"
                      << "  --callback-playback | --linecoach-proxy-playback\n"
                      << "  --callback-preroll-ms <ms>\n"
                      << "  --callback-buffer-floor-ms <ms>\n"
                      << "  --callback-coalesce-ms <ms>\n"
                      << "  --callback-max-burst-ms <ms>\n"
                      << "  --play-streaming | --no-play-streaming\n"
                      << "  --live-preroll-ms <ms>\n"
                      << "  --ramp-tail-window-frames <n>\n"
                      << "  --ramp-tail-window-count <n>\n"
                      << "  --early-context-frames <n>\n"
                      << "  --early-context-window-count <n>\n"
                      << "  --adaptive-steady-windows | --no-adaptive-steady-windows\n"
                      << "  --adaptive-min-tail-window-frames <n>\n"
                      << "  --adaptive-low-watermark-ms <ms>\n"
                      << "  --adaptive-high-watermark-ms <ms>\n"
                      << "  --paced-audio-delivery | --no-paced-audio-delivery\n"
                      << "  --delivery-chunk-ms <ms>\n"
                      << "  --delivery-start-buffer-ms <ms>\n"
                      << "  --delivery-target-lead-ms <ms>\n"
                      << "  --paced-live-playback | --no-paced-live-playback\n"
                      << "  --steady-split-decode-frames <n>\n"
                      << "  --async-streaming-decode | --no-async-streaming-decode\n"
                      << "  --cache-instruction-tokens | --no-cache-instruction-tokens\n"
                      << "  --instruction-cache-key <key>\n"
                      << "  --warm-voice-profile\n"
                      << "  --warm-voice-profile-key <key>\n"
                      << "  --warmup-text <text>\n"
                      << "  --repeat <n>\n"
                      << "  --simulate-stream-callback\n"
                      << "  --dump-streaming-overlap\n"
                      << "\n"
                      << "VoiceDesign example:\n"
                      << "  qwen3_streaming_cli -m models --voice-design --model-name qwen3-tts-1.7b-voicedesign-f16 "
                         "--voice-design-instruct \"A calm, deep male narrator.\" -t \"I was not expecting visitors this late.\" "
                         "-o examples\\voice_design.wav\n";
            return 0;
        }
    }

    Qwen3StreamingTts tts;
    if (!tts.load(model_dir)) return 1;
    if (!speaker_embedding.empty() && !tts.load_speaker_embedding(speaker_embedding)) return 1;

    TtsChunkCallback on_chunk;
    auto make_callback_playback = [&](int64_t request_start_ms) -> TtsChunkCallback {
#ifdef _WIN32
        auto player = std::make_shared<LineCoachProxyAudioPlayer>();
        auto opened = std::make_shared<bool>(false);
        return [player, opened, request_start_ms, callback_preroll_ms, callback_buffer_floor_ms, callback_coalesce_ms, callback_max_burst_ms](const TtsStreamChunk& chunk) mutable {
            if (!*opened) {
                *opened = true;
                if (!player->open(chunk.sample_rate,
                                  callback_preroll_ms,
                                  callback_buffer_floor_ms,
                                  callback_coalesce_ms,
                                  callback_max_burst_ms,
                                  request_start_ms)) {
                    std::cerr << "Callback playback unavailable: " << player->last_error() << "\n";
                    return;
                }
            }
            player->push(chunk);
            if (chunk.is_final) {
                player->drain();
            }
        };
#else
        (void) request_start_ms;
        return {};
#endif
    };
    if (callback_playback) {
#ifdef _WIN32
        options.play_streaming = false;
#else
        std::cerr << "--callback-playback is only supported on Windows.\n";
        return 2;
#endif
    } else if (simulate_stream_callback) {
        on_chunk = [](const TtsStreamChunk&) {};
    }

    const std::filesystem::path base_output = options.output_wav.empty()
        ? std::filesystem::path("examples/bridge_test.wav")
        : std::filesystem::path(options.output_wav);

    for (int run = 0; run < repeat; ++run) {
        TtsStreamOptions run_options = options;
        if (repeat > 1) {
            std::filesystem::path run_output = base_output;
            const std::string stem = run_output.stem().string();
            const std::string ext = run_output.extension().string();
            run_output.replace_filename(stem + "_run" + std::to_string(run + 1) + ext);
            run_options.output_wav = run_output.string();
            std::cout << "[repeat] run " << (run + 1) << "/" << repeat
                      << " output=" << run_options.output_wav << "\n";
        }

        const int64_t run_request_start_ms = cli_now_ms();
        if (callback_playback) {
            on_chunk = make_callback_playback(run_request_start_ms);
        }

        if (!tts.synthesize_streaming(text, run_options, on_chunk)) {
            return 1;
        }
    }

    return 0;
}
