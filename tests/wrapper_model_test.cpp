#include "offgrid_tts/Qwen3StreamingTts.h"
#include "qwen3_tts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct Arguments {
    fs::path models;
    std::string model;
    fs::path embedding;
    fs::path output_dir;
};

bool parse_arguments(int argc, char** argv, Arguments& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) return false;
        if (arg == "--models") out.models = argv[++i];
        else if (arg == "--model") out.model = argv[++i];
        else if (arg == "--embedding") out.embedding = argv[++i];
        else if (arg == "--output-dir") out.output_dir = argv[++i];
        else return false;
    }
    return !out.models.empty() && !out.model.empty() && !out.embedding.empty() && !out.output_dir.empty();
}

bool require(bool condition, const std::string& message) {
    if (!condition) std::cerr << "FAIL: " << message << "\n";
    return condition;
}

std::vector<unsigned char> read_bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

TtsStreamOptions acceptance_options(const fs::path& output, const std::string& model) {
    TtsStreamOptions options;
    options.output_wav = output.string();
    options.model_identifier = model;
    options.seed = 42;
    options.print_progress = false;
    options.print_timing = false;
    options.play_streaming = false;
    options.first_tail_window_frames = 5;
    options.ramp_tail_window_frames = 5;
    options.ramp_tail_window_count = 2;
    options.steady_tail_window_frames = 8;
    options.context_frames = 2;
    options.early_context_frames = 1;
    options.early_context_window_count = 2;
    options.final_context_frames = 3;
    options.adaptive_steady_windows = true;
    options.adaptive_min_tail_window_frames = 7;
    options.adaptive_low_watermark_ms = 220;
    options.adaptive_high_watermark_ms = 520;
    options.paced_audio_delivery = true;
    options.delivery_chunk_ms = 240;
    options.delivery_start_buffer_ms = 240;
    options.delivery_target_lead_ms = 520;
    return options;
}

struct Capture {
    std::vector<float> samples;
    int32_t sample_rate = 0;
    int final_callbacks = 0;
    int chunks = 0;
    int64_t first_350_ms = -1;
    int64_t max_gap_ms = 0;
    Clock::time_point start;
    Clock::time_point previous;

    bool on_chunk(const TtsStreamChunk& chunk) {
        const auto now = Clock::now();
        const int64_t wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (chunks > 0) {
            max_gap_ms = std::max<int64_t>(max_gap_ms,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - previous).count());
        }
        previous = now;
        ++chunks;
        if (sample_rate == 0) sample_rate = chunk.sample_rate;
        if (chunk.sample_rate != sample_rate) return false;
        samples.insert(samples.end(), chunk.samples.begin(), chunk.samples.end());
        if (first_350_ms < 0 && samples.size() >= static_cast<size_t>(sample_rate) * 350 / 1000) {
            first_350_ms = wall_ms;
        }
        if (chunk.is_final) ++final_callbacks;
        return true;
    }
};

} // namespace

int main(int argc, char** argv) {
    Arguments args;
    if (!parse_arguments(argc, argv, args)) {
        std::cerr << "Usage: qwen3_wrapper_model_test --models DIR --model NAME --embedding FILE --output-dir DIR\n";
        return 2;
    }
    fs::create_directories(args.output_dir);
    const fs::path first_wav = args.output_dir / "wrapper_contract_a.wav";
    const fs::path second_wav = args.output_dir / "wrapper_contract_b.wav";

    Qwen3StreamingTts tts;
    const auto load_start = Clock::now();
    if (!require(tts.load(args.models.string(), args.model), tts.last_error())) return 1;
    const int64_t load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - load_start).count();
    if (!require(tts.is_loaded(), "load() must leave the model immediately available")) return 1;
    if (!require(tts.capabilities().loaded && tts.capabilities().supports_voice_clone,
                 "loaded base model must report voice-clone capability")) return 1;
    if (!require(tts.load_speaker_embedding(args.embedding.string()), tts.last_error())) return 1;

    const std::string text = "Use a reasonably long paragraph for this consistency test.";
    Capture first;
    first.start = Clock::now();
    TtsStreamOptions first_options = acceptance_options(first_wav, args.model);
    if (!require(tts.synthesize_streaming(text, first_options,
            [&](const TtsStreamChunk& chunk) { return first.on_chunk(chunk); }), tts.last_error())) return 1;
    const int64_t synth_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - first.start).count();

    if (!require(first.sample_rate == 24000, "callback sample rate must be 24 kHz")) return 1;
    if (!require(first.final_callbacks == 1, "final callback must occur exactly once")) return 1;
    if (!require(first.chunks >= 2, "streaming synthesis must deliver multiple chunks")) return 1;
    if (!require(first.first_350_ms >= 0, "callback must deliver at least 350 ms")) return 1;
    if (!require(std::all_of(first.samples.begin(), first.samples.end(), [](float value) { return std::isfinite(value); }),
                 "callback PCM must contain only finite samples")) return 1;

    std::vector<float> wav_samples;
    int wav_rate = 0;
    if (!require(qwen3_tts::load_audio_file(first_wav.string(), wav_samples, wav_rate), "written WAV must be readable")) return 1;
    if (!require(wav_rate == first.sample_rate && wav_samples.size() == first.samples.size(),
                 "callback PCM and WAV must have identical shape")) return 1;
    float max_wav_delta = 0.0f;
    for (size_t i = 0; i < wav_samples.size(); ++i) {
        max_wav_delta = std::max(max_wav_delta, std::fabs(wav_samples[i] - first.samples[i]));
    }
    if (!require(max_wav_delta <= 4.0f / 32768.0f, "callback PCM and quantized WAV differ unexpectedly")) return 1;

    Capture second;
    second.start = Clock::now();
    TtsStreamOptions second_options = acceptance_options(second_wav, args.model);
    if (!require(tts.synthesize_streaming(text, second_options,
            [&](const TtsStreamChunk& chunk) { return second.on_chunk(chunk); }), tts.last_error())) return 1;
    if (!require(read_bytes(first_wav) == read_bytes(second_wav), "fixed-seed synthesis must be byte deterministic")) return 1;

    int cancel_callbacks = 0;
    TtsStreamOptions cancel_options = acceptance_options({}, args.model);
    const bool cancel_result = tts.synthesize_streaming(
        "Cancel this request after its first audio chunk.",
        cancel_options,
        [&](const TtsStreamChunk&) {
            ++cancel_callbacks;
            return false;
        });
    if (!require(!cancel_result && cancel_callbacks == 1, "callback cancellation must stop after the first chunk")) return 1;
    if (!require(tts.last_error().find("requested stop") != std::string::npos,
                 "callback cancellation must return a useful error")) return 1;

    const double audio_ms = 1000.0 * static_cast<double>(first.samples.size()) / first.sample_rate;
    const double rtf = audio_ms > 0.0 ? static_cast<double>(synth_ms) / audio_ms : std::numeric_limits<double>::infinity();
    std::cout << "{\"load_ms\":" << load_ms
              << ",\"first_350_ms\":" << first.first_350_ms
              << ",\"max_callback_gap_ms\":" << first.max_gap_ms
              << ",\"synthesis_ms\":" << synth_ms
              << ",\"audio_ms\":" << audio_ms
              << ",\"rtf\":" << rtf
              << ",\"chunks\":" << first.chunks
              << ",\"max_wav_delta\":" << max_wav_delta
              << ",\"deterministic\":true,\"cancellation\":true}\n";
    return 0;
}
