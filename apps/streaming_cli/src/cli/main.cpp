#include "offgrid_tts/Qwen3StreamingTts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#endif

namespace {

struct BatchNamedPath {
    std::string id;
    std::filesystem::path path;
};

bool IsSafeBatchId(const std::string& id) {
    if (id.empty()) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-';
    });
}

bool ReadBatchNamedPaths(const std::filesystem::path& list_path,
                         const char* kind,
                         std::vector<BatchNamedPath>& out) {
    std::ifstream input(list_path);
    if (!input) {
        std::cerr << "Failed to open batch " << kind << " list: " << list_path << "\n";
        return false;
    }
    const std::filesystem::path base = std::filesystem::absolute(list_path).parent_path();
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            std::cerr << "Batch " << kind << " list line " << line_number
                      << " must be <id><TAB><path>.\n";
            return false;
        }
        BatchNamedPath row;
        row.id = line.substr(0, tab);
        row.path = std::filesystem::path(line.substr(tab + 1));
        if (!IsSafeBatchId(row.id) || row.path.empty()) {
            std::cerr << "Invalid batch " << kind << " list line " << line_number << ".\n";
            return false;
        }
        if (row.path.is_relative()) row.path = base / row.path;
        row.path = std::filesystem::absolute(row.path).lexically_normal();
        if (!std::filesystem::exists(row.path)) {
            std::cerr << "Batch " << kind << " path does not exist: " << row.path << "\n";
            return false;
        }
        if (std::any_of(out.begin(), out.end(), [&](const BatchNamedPath& existing) {
                return existing.id == row.id;
            })) {
            std::cerr << "Duplicate batch " << kind << " id: " << row.id << "\n";
            return false;
        }
        out.push_back(std::move(row));
    }
    if (out.empty()) {
        std::cerr << "Batch " << kind << " list is empty: " << list_path << "\n";
        return false;
    }
    return true;
}

bool ReadTextFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    out = buffer.str();
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
    return !out.empty();
}

bool ParseBatchSeeds(const std::string& value, std::vector<int64_t>& out) {
    std::istringstream input(value);
    std::string part;
    try {
        while (std::getline(input, part, ',')) {
            if (part.empty()) continue;
            size_t consumed = 0;
            const int64_t seed = std::stoll(part, &consumed);
            if (consumed != part.size()) return false;
            if (std::find(out.begin(), out.end(), seed) == out.end()) out.push_back(seed);
        }
    } catch (const std::exception&) {
        return false;
    }
    return !out.empty();
}

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
        << "  --batch-transcript-list <tsv> --batch-voice-list <tsv>\n"
        << "  --batch-seeds <comma-list> --batch-output-dir <path>\n"
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
    std::string batch_transcript_list;
    std::string batch_voice_list;
    std::string batch_seeds_text;
    std::string batch_output_dir;

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
        else if (a == "--batch-transcript-list") batch_transcript_list = next();
        else if (a == "--batch-voice-list") batch_voice_list = next();
        else if (a == "--batch-seeds") batch_seeds_text = next();
        else if (a == "--batch-output-dir") batch_output_dir = next();
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

    const bool batch_requested = !batch_transcript_list.empty()
        || !batch_voice_list.empty()
        || !batch_seeds_text.empty()
        || !batch_output_dir.empty();
    if (batch_requested && (batch_transcript_list.empty()
        || batch_voice_list.empty()
        || batch_seeds_text.empty()
        || batch_output_dir.empty())) {
        std::cerr << "Batch mode requires transcript list, voice list, seeds, and output directory.\n";
        return 2;
    }

    std::vector<BatchNamedPath> batch_transcripts;
    std::vector<BatchNamedPath> batch_voices;
    std::vector<int64_t> batch_seeds;
    if (batch_requested) {
        if (!ReadBatchNamedPaths(batch_transcript_list, "transcript", batch_transcripts)) {
            return 2;
        }
        if (!ReadBatchNamedPaths(batch_voice_list, "voice", batch_voices)) {
            return 2;
        }
        if (!ParseBatchSeeds(batch_seeds_text, batch_seeds)) {
            std::cerr << "Invalid or empty --batch-seeds value.\n";
            return 2;
        }
    }

    Qwen3StreamingTts tts;
    if (!tts.load(model_dir, options.model_identifier)) {
        std::cerr << tts.last_error() << "\n";
        return 1;
    }
    if (!batch_requested && !speaker_embedding.empty() && !tts.load_speaker_embedding(speaker_embedding)) {
        std::cerr << tts.last_error() << "\n";
        return 1;
    }

    TtsChunkCallback on_chunk;
    if (simulate_stream_callback) {
        on_chunk = [](const TtsStreamChunk&) { return true; };
    }

    if (batch_requested) {
        const std::filesystem::path output_dir = std::filesystem::absolute(batch_output_dir);
        std::error_code ec;
        std::filesystem::create_directories(output_dir, ec);
        if (ec) {
            std::cerr << "Failed to create batch output directory: " << ec.message() << "\n";
            return 1;
        }
        std::ofstream index(output_dir / "batch_results.tsv", std::ios::trunc);
        if (!index) {
            std::cerr << "Failed to create batch result index.\n";
            return 1;
        }
        index << "transcript_id\tvoice_id\tseed\toutput_wav\n";
        const size_t total = batch_transcripts.size() * batch_voices.size() * batch_seeds.size();
        size_t completed = 0;
        for (const BatchNamedPath& voice : batch_voices) {
            if (!tts.load_speaker_embedding(voice.path.string())) {
                std::cerr << tts.last_error() << "\n";
                return 1;
            }
            for (const BatchNamedPath& transcript : batch_transcripts) {
                std::string batch_text;
                if (!ReadTextFile(transcript.path, batch_text)) {
                    std::cerr << "Failed to read batch transcript: " << transcript.path << "\n";
                    return 1;
                }
                for (const int64_t seed : batch_seeds) {
                    TtsStreamOptions run_options = options;
                    run_options.seed = seed;
                    const std::string filename = transcript.id + "__" + voice.id
                        + "__seed_" + std::to_string(seed) + ".wav";
                    run_options.output_wav = (output_dir / filename).string();
                    std::cout << "[batch] " << (completed + 1) << "/" << total
                              << " transcript=" << transcript.id
                              << " voice=" << voice.id
                              << " seed=" << seed << "\n";
                    if (!tts.synthesize_streaming(batch_text, run_options, on_chunk)) {
                        std::cerr << tts.last_error() << "\n";
                        return 1;
                    }
                    index << transcript.id << '\t' << voice.id << '\t' << seed
                          << '\t' << filename << '\n';
                    index.flush();
                    ++completed;
                }
            }
        }
        std::cout << "[batch] completed " << completed << " utterances\n";
        return 0;
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
