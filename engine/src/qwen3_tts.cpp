#include "qwen3_tts.h"

namespace qwen3_tts {

Qwen3TTS::Qwen3TTS() = default;

Qwen3TTS::~Qwen3TTS() = default;

void Qwen3TTS::set_progress_callback(tts_progress_callback_t callback) {
    progress_callback_ = callback;
}

bool Qwen3TTS::prime_instruction_cache(const std::string & instruction,
                                       const std::string & cache_key) {
    if (instruction.empty()) {
        error_msg_.clear();
        return true;
    }
    if (!models_loaded_ || !tokenizer_.is_loaded()) {
        error_msg_ = "Cannot prime instruction cache before models are loaded";
        return false;
    }

    std::vector<int32_t> tokens = tokenizer_.encode_instruct(instruction);
    if (tokens.empty()) {
        error_msg_ = "Failed to tokenize instruction for cache priming";
        return false;
    }

    const std::string key = cache_key.empty() ? instruction : cache_key;
    instruction_token_cache_[key] = std::move(tokens);
    error_msg_.clear();
    return true;
}

bool Qwen3TTS::warm_voice_profile(const std::string & warmup_text,
                                  const tts_params & params) {
    (void) warmup_text;

    if (params.instruction.empty()) {
        error_msg_.clear();
        return true;
    }

    const std::string profile_key = !params.voice_profile_key.empty()
        ? params.voice_profile_key
        : (!params.instruction_cache_key.empty() ? params.instruction_cache_key : params.instruction);
    return prime_instruction_cache(params.instruction, profile_key);
}

} // namespace qwen3_tts
