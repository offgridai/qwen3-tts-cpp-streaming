#include "offgrid_tts/Qwen3StreamingTts.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#endif

namespace {

bool ApplyProfile(const std::string& profile, TtsStreamOptions& options) {
    if (profile == "realtime") {
        options.model_identifier = "qwen3-tts-0.6b-f16";
        options.live_preroll_ms = 150;
        options.first_tail_window_frames = 3;
        options.ramp_tail_window_frames = 6;
        options.ramp_tail_window_count = 0;
        options.steady_tail_window_frames = 8;
        options.context_frames = 2;
        options.early_context_frames = 1;
        options.early_context_window_count = 2;
        options.final_context_frames = 3;
        options.adaptive_steady_windows = false;
        options.paced_audio_delivery = false;
        options.steady_split_decode_frames = 0;
        return true;
    }
    if (profile == "memory-saver" || profile == "ultra-low") {
        options.model_identifier = profile == "memory-saver"
            ? "qwen3-tts-0.6b-q5_k"
            : "qwen3-tts-0.6b-q4_k";
        options.live_preroll_ms = profile == "memory-saver" ? 1000 : 2000;
        options.first_tail_window_frames = 3;
        options.ramp_tail_window_frames = 6;
        options.ramp_tail_window_count = 0;
        options.steady_tail_window_frames = 8;
        options.context_frames = 2;
        options.early_context_frames = 1;
        options.early_context_window_count = 2;
        options.final_context_frames = 3;
        options.adaptive_steady_windows = false;
        options.paced_audio_delivery = false;
        options.steady_split_decode_frames = 0;
        return true;
    }
    if (profile == "offgrid-callback") {
        options.live_preroll_ms = 150;
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
        options.steady_split_decode_frames = 0;
        return true;
    }
    return false;
}

void PrintUsage() {
    std::cout
        << "Usage: qwen3_streaming_cli -m models --model-identifier qwen3-tts-0.6b-f16 --speaker-embedding speaker.json -t text -o out.wav\n"
        << "  --voice-design --voice-design-instruct <text>\n"
        << "  --instruction <text> --speaker <name> --language-id <id>\n"
        << "  --max-tokens <n> --max-frames-per-text-token <n> --min-dynamic-tokens <n>\n"
        << "  --temperature <n> --top-k <n> --top-p <n> --cb0-top-p <n>\n"
        << "  --repetition-penalty <n> --seed <n>\n"
        << "  --quiet | --quiet-all | --verbose\n"
        << "  --tts-profile realtime|memory-saver|ultra-low|offgrid-callback\n"
        << "  --play-streaming | --no-play-streaming --live-preroll-ms <ms>\n"
        << "  --first-tail-window-frames <n> --ramp-tail-window-frames <n>\n"
        << "  --ramp-tail-window-count <n> --steady-tail-window-frames <n>\n"
        << "  --context-frames <n> --early-context-frames <n>\n"
        << "  --early-context-window-count <n> --final-context-frames <n>\n"
        << "  --adaptive-steady-windows | --no-adaptive-steady-windows\n"
        << "  --adaptive-min-tail-window-frames <n>\n"
        << "  --adaptive-low-watermark-ms <ms> --adaptive-high-watermark-ms <ms>\n"
        << "  --paced-audio-delivery | --no-paced-audio-delivery\n"
        << "  --delivery-chunk-ms <ms> --delivery-start-buffer-ms <ms>\n"
        << "  --delivery-target-lead-ms <ms>\n"
        << "  --paced-live-playback | --no-paced-live-playback\n"
        << "  --steady-split-decode-frames <n>\n"
        << "  --async-streaming-decode | --no-async-streaming-decode\n"
        << "  --cache-instruction-tokens | --no-cache-instruction-tokens\n"
        << "  --instruction-cache-key <key> --repeat <n>\n"
        << "  --simulate-stream-callback --dump-first-frame-profile --dump-streaming-overlap\n";
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // PowerShell renders native stderr as errors. This CLI's output is diagnostic,
    // so keep all native and C++ logging on the regular stdout stream.
    std::fflush(stderr);
    _dup2(_fileno(stdout), _fileno(stderr));
    SetStdHandle(STD_ERROR_HANDLE, GetStdHandle(STD_OUTPUT_HANDLE));
#endif
    std::cerr.rdbuf(std::cout.rdbuf());

    std::string model_dir = "models";
    std::string speaker_embedding;
    std::string text = "Hello. Welcome to Alfie's Bodega. I'm Alfie. What can I get for you today?";
    bool simulate_stream_callback = false;
    int repeat = 1;

    TtsStreamOptions options;
    options.output_wav = "examples/bridge_test.wav";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
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
        else if (a == "--speaker") options.speaker = next();
        else if (a == "--language-id") options.language_id = std::stoi(next());
        else if (a == "-t" || a == "--text") text = next();
        else if (a == "-o" || a == "--output") options.output_wav = next();
        else if (a == "--model-identifier" || a == "--model-name") options.model_identifier = next();
        else if (a == "--voice-design-instruct" || a == "--instruction" || a == "--instruct") options.instruction = next();
        else if (a == "--max-tokens") options.max_audio_tokens = std::stoi(next());
        else if (a == "--max-frames-per-text-token") options.max_audio_frames_per_text_token = std::stof(next());
        else if (a == "--min-dynamic-tokens") options.min_dynamic_audio_tokens = std::stoi(next());
        else if (a == "--temperature") options.temperature = std::stof(next());
        else if (a == "--top-k") options.top_k = std::stoi(next());
        else if (a == "--top-p") options.top_p = std::stof(next());
        else if (a == "--cb0-top-p") options.cb0_top_p = std::stof(next());
        else if (a == "--repetition-penalty") options.repetition_penalty = std::stof(next());
        else if (a == "--seed") options.seed = std::stoll(next());
        else if (a == "--quiet") { options.print_progress = false; options.print_timing = true; }
        else if (a == "--quiet-all") { options.print_progress = false; options.print_timing = false; }
        else if (a == "--verbose") { options.print_progress = true; options.print_timing = true; }
        else if (a == "--tts-profile") {
            const std::string profile = next();
            if (!ApplyProfile(profile, options)) {
                std::cerr << "Unknown --tts-profile '" << profile << "'.\n";
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
        else if (a == "--repeat") repeat = std::max(1, std::stoi(next()));
        else if (a == "--simulate-stream-callback") simulate_stream_callback = true;
        else if (a == "-h" || a == "--help") { PrintUsage(); return 0; }
        else {
            std::cerr << "Unknown argument: " << a << "\n";
            return 2;
        }
    }

    if (simulate_stream_callback) {
        options.play_streaming = false;
    }

    Qwen3StreamingTts tts;
    if (!tts.load(model_dir, options.model_identifier)) {
        std::cerr << tts.last_error() << "\n";
        return 1;
    }
    if (!speaker_embedding.empty() && !tts.load_speaker_embedding(speaker_embedding)) {
        std::cerr << tts.last_error() << "\n";
        return 1;
    }

    TtsChunkCallback on_chunk;
    if (simulate_stream_callback) {
        on_chunk = [](const TtsStreamChunk&) { return true; };
    }

    const std::filesystem::path base_output = options.output_wav;
    for (int run = 0; run < repeat; ++run) {
        TtsStreamOptions run_options = options;
        if (repeat > 1 && !base_output.empty()) {
            std::filesystem::path run_output = base_output;
            run_output.replace_filename(
                run_output.stem().string() + "_run" + std::to_string(run + 1) + run_output.extension().string());
            run_options.output_wav = run_output.string();
            std::cout << "[repeat] run " << (run + 1) << "/" << repeat
                      << " output=" << run_options.output_wav << "\n";
        }

        if (!tts.synthesize_streaming(text, run_options, on_chunk)) {
            std::cerr << tts.last_error() << "\n";
            return 1;
        }
    }

    return 0;
}
