#include "offgrid_tts/Qwen3StreamingTts.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string model_dir = "models";
    std::string speaker_embedding;
    std::string text = "Hello. Welcome to Alfie's Bodega. I'm Alfie. What can I get for you today?";
    bool simulate_stream_callback = false;

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
        else if (a == "--tts-profile") {
            const std::string profile = next();
            if (profile == "realtime") {
                options.model_identifier = "qwen3-tts-0.6b-f16";
                options.live_preroll_ms = 150;
                options.first_tail_window_frames = 3;
                options.ramp_tail_window_frames = 6;
                options.ramp_tail_window_count = 0;
                options.steady_tail_window_frames = 8;
                options.context_frames = 3;
                options.early_context_frames = 0;
                options.early_context_window_count = 0;
                options.final_context_frames = 4;
            } else if (profile == "memory-saver") {
                options.model_identifier = "qwen3-tts-0.6b-q5_k";
                options.live_preroll_ms = 1000;
                options.first_tail_window_frames = 3;
                options.ramp_tail_window_frames = 6;
                options.ramp_tail_window_count = 0;
                options.steady_tail_window_frames = 8;
                options.context_frames = 3;
                options.early_context_frames = 0;
                options.early_context_window_count = 0;
                options.final_context_frames = 4;
            } else if (profile == "ultra-low") {
                options.model_identifier = "qwen3-tts-0.6b-q4_k";
                options.live_preroll_ms = 2000;
                options.first_tail_window_frames = 3;
                options.ramp_tail_window_frames = 6;
                options.ramp_tail_window_count = 0;
                options.steady_tail_window_frames = 8;
                options.context_frames = 3;
                options.early_context_frames = 0;
                options.early_context_window_count = 0;
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
                      << "  --tts-profile realtime|memory-saver|ultra-low\n"
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
    if (simulate_stream_callback) {
        on_chunk = [](const TtsStreamChunk&) {};
    }

    return tts.synthesize_streaming(text, options, on_chunk) ? 0 : 1;
}
