#include "offgrid_tts/Qwen3StreamingTts.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string model_dir = "models";
    std::string speaker_embedding;
    std::string text = "Hello. Welcome to Alfie's Bodega. I'm Alfie. What can I get for you today?";

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
        else if (a == "--speaker-embedding") speaker_embedding = next();
        else if (a == "-t" || a == "--text") text = next();
        else if (a == "-o" || a == "--output") options.output_wav = next();
        else if (a == "--model-identifier" || a == "--model-name") options.model_identifier = next();
        else if (a == "--instruction" || a == "--instruct") options.instruction = next();
        else if (a == "--tts-profile") {
            const std::string profile = next();
            if (profile == "realtime") {
                options.model_identifier = "qwen3-tts-0.6b-f16";
                options.live_preroll_ms = 250;
                options.first_tail_window_frames = 1;
                options.steady_tail_window_frames = 12;
                options.context_frames = 4;
                options.final_context_frames = 4;
            } else if (profile == "memory-saver") {
                options.model_identifier = "qwen3-tts-0.6b-q5_k";
                options.live_preroll_ms = 1000;
                options.first_tail_window_frames = 1;
                options.steady_tail_window_frames = 12;
                options.context_frames = 4;
                options.final_context_frames = 4;
            } else if (profile == "ultra-low") {
                options.model_identifier = "qwen3-tts-0.6b-q4_k";
                options.live_preroll_ms = 2000;
                options.first_tail_window_frames = 1;
                options.steady_tail_window_frames = 12;
                options.context_frames = 4;
                options.final_context_frames = 4;
            } else {
                std::cerr << "Unknown --tts-profile '" << profile << "'. Expected realtime, memory-saver, or ultra-low.\n";
                return 2;
            }
        }
        else if (a == "--live-preroll-ms") options.live_preroll_ms = std::stoi(next());
        else if (a == "--dump-first-frame-profile") options.dump_first_frame_profile = true;
        else if (a == "--dump-streaming-overlap") options.dump_streaming_overlap = true;
        else if (a == "--first-tail-window-frames") options.first_tail_window_frames = std::stoi(next());
        else if (a == "--steady-tail-window-frames") options.steady_tail_window_frames = std::stoi(next());
        else if (a == "--context-frames") options.context_frames = std::stoi(next());
        else if (a == "--final-context-frames") options.final_context_frames = std::stoi(next());
        else if (a == "-h" || a == "--help") {
            std::cout << "Usage: qwen3_streaming_cli -m models --model-identifier qwen3-tts-0.6b-f16 --speaker-embedding speaker.json -t text -o out.wav\n"
                      << "  --tts-profile realtime|memory-saver|ultra-low\n"
                      << "  --live-preroll-ms <ms>\n"
                      << "  --dump-streaming-overlap\n";
            return 0;
        }
    }

    Qwen3StreamingTts tts;
    if (!tts.load(model_dir)) return 1;
    if (!speaker_embedding.empty() && !tts.load_speaker_embedding(speaker_embedding)) return 1;

    return tts.synthesize_streaming(text, options, [](const TtsStreamChunk&) {}) ? 0 : 1;
}
