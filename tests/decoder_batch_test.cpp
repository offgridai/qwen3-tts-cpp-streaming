#include "audio_tokenizer_decoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

int main(int argc, char ** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: qwen3_decoder_batch_test <tokenizer.gguf> [frames]\n";
        return 2;
    }

    qwen3_tts::AudioTokenizerDecoder decoder;
    if (!decoder.load_model(argv[1])) {
        std::cerr << decoder.get_error() << "\n";
        return 1;
    }

    const int frames = argc == 3 ? std::max(1, std::stoi(argv[2])) : 9;
    constexpr int codebooks = 16;
    constexpr int batch_size = 2;
    std::vector<int32_t> codes((size_t) batch_size * frames * codebooks);
    for (int b = 0; b < batch_size; ++b) {
        for (int f = 0; f < frames; ++f) {
            for (int cb = 0; cb < codebooks; ++cb) {
                codes[((size_t) b * frames + f) * codebooks + cb] =
                    (97 * b + 31 * f + 17 * cb + 11) % 2048;
            }
        }
    }

    std::vector<std::vector<float>> singles(batch_size);
    const auto single_start = Clock::now();
    for (int b = 0; b < batch_size; ++b) {
        if (!decoder.decode(codes.data() + (size_t) b * frames * codebooks,
                            frames, singles[(size_t) b])) {
            std::cerr << decoder.get_error() << "\n";
            return 1;
        }
    }
    const auto single_end = Clock::now();

    std::vector<std::vector<float>> batched;
    const auto batch_start = Clock::now();
    if (!decoder.decode_batch(codes.data(), frames, batch_size, batched)) {
        std::cerr << decoder.get_error() << "\n";
        return 1;
    }
    const auto batch_end = Clock::now();

    if (batched.size() != singles.size()) {
        std::cerr << "batch output count mismatch\n";
        return 1;
    }

    float max_delta = 0.0f;
    double squared_error = 0.0;
    size_t compared = 0;
    for (size_t b = 0; b < singles.size(); ++b) {
        if (batched[b].size() != singles[b].size()) {
            std::cerr << "batch output shape mismatch\n";
            return 1;
        }
        for (size_t i = 0; i < singles[b].size(); ++i) {
            const float delta = std::fabs(batched[b][i] - singles[b][i]);
            max_delta = std::max(max_delta, delta);
            squared_error += (double) delta * delta;
            ++compared;
        }
    }

    const double rmse = std::sqrt(squared_error / std::max<size_t>(1, compared));
    const auto single_ms = std::chrono::duration_cast<std::chrono::milliseconds>(single_end - single_start).count();
    const auto batch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(batch_end - batch_start).count();
    std::cout << "{\"frames\":" << frames
              << ",\"batch_size\":" << batch_size
              << ",\"single_ms\":" << single_ms
              << ",\"batch_ms\":" << batch_ms
              << ",\"max_delta\":" << max_delta
              << ",\"rmse\":" << rmse << "}\n";

    // Batched GEMMs can select a different accumulation schedule from serial
    // execution. Guard against request mixing or material waveform drift while
    // allowing the measured floating-point envelope of the physical batch path.
    if (!std::isfinite(max_delta) || max_delta > 0.20f || rmse > 0.01) {
        std::cerr << "batched decoder parity exceeded tolerance\n";
        return 1;
    }
    return 0;
}
