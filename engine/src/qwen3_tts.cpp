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
    const std::string profile_key = !params.voice_profile_key.empty()
        ? params.voice_profile_key
        : (!params.instruction_cache_key.empty() ? params.instruction_cache_key : params.instruction);

    if (!profile_key.empty() && warmed_voice_profiles_.find(profile_key) != warmed_voice_profiles_.end()) {
        error_msg_.clear();
        return true;
    }

    tts_params warm_params = params;
    warm_params.print_progress = false;
    warm_params.print_timing = false;
    warm_params.play_streaming = false;
    warm_params.audio_chunk_callback = nullptr;
    warm_params.dump_first_frame_profile = false;
    warm_params.dump_streaming_overlap = false;
    warm_params.max_audio_tokens = warm_params.max_audio_tokens > 0
        ? std::min<int32_t>(warm_params.max_audio_tokens, 64)
        : 64;
    warm_params.cache_instruction_tokens = true;

    const std::string effective_text = warmup_text.empty() ? "Hello." : warmup_text;
    tts_result result = synthesize(effective_text, warm_params);
    if (!result.success) {
        error_msg_ = result.error_msg.empty() ? "Voice profile warmup failed" : result.error_msg;
        return false;
    }

    if (!profile_key.empty()) {
        warmed_voice_profiles_.insert(profile_key);
    }
    error_msg_.clear();
    return true;
}

} // namespace qwen3_tts
