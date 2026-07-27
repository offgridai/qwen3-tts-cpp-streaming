#include "offgrid_tts/Qwen3StreamingTts.h"

#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

namespace {

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    static_assert(!std::is_copy_constructible_v<Qwen3StreamingTts>);
    static_assert(!std::is_copy_assignable_v<Qwen3StreamingTts>);
    static_assert(std::is_nothrow_move_constructible_v<Qwen3StreamingTts>);
    static_assert(std::is_nothrow_move_assignable_v<Qwen3StreamingTts>);

    Qwen3StreamingTts original;
    if (!require(!original.is_loaded(), "new wrapper must not report a loaded model")) return 1;
    if (!require(!original.capabilities().loaded, "new wrapper capabilities must report unloaded")) return 1;
    std::vector<std::vector<float>> batch_audio;
    if (!require(!original.decode_audio_codes_batch({}, batch_audio),
                 "batch decode without a loaded model must fail")) return 1;
    if (!require(!original.last_error().empty(),
                 "failed batch decode must populate last_error")) return 1;
    if (!require(!original.load("__qwen3_missing_model_directory__"), "invalid model directory must fail")) return 1;
    if (!require(!original.last_error().empty(), "failed load must populate last_error")) return 1;

    Qwen3StreamingTts moved(std::move(original));
    if (!require(!moved.last_error().empty(), "move construction must preserve wrapper state")) return 1;

    Qwen3StreamingTts assigned;
    assigned = std::move(moved);
    if (!require(!assigned.last_error().empty(), "move assignment must preserve wrapper state")) return 1;

    std::cout << "wrapper contract: PASS\n";
    return 0;
}
