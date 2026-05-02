#include "offgrid_tts/Qwen3StreamingTts.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string model_dir = "models";
    std::string speaker_embedding = "reference/alfie_speaker.json";
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
        else if (a == "--dump-first-frame-profile") options.dump_first_frame_profile = true;
        else if (a == "--first-tail-window-frames") options.first_tail_window_frames = std::stoi(next());
        else if (a == "--steady-tail-window-frames") options.steady_tail_window_frames = std::stoi(next());
        else if (a == "--context-frames") options.context_frames = std::stoi(next());
        else if (a == "--final-context-frames") options.final_context_frames = std::stoi(next());
        else if (a == "-h" || a == "--help") {
            std::cout << "Usage: qwen3_streaming_cli -m models --speaker-embedding speaker.json -t text -o out.wav\n";
            return 0;
        }
    }

    Qwen3StreamingTts tts;
    if (!tts.load(model_dir)) return 1;
    if (!tts.load_speaker_embedding(speaker_embedding)) return 1;

    return tts.synthesize_streaming(text, options, [](const TtsStreamChunk&) {}) ? 0 : 1;
}
