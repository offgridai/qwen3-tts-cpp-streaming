#include "tts_transformer.h"
#include "transformer/transformer_state_internal.h"

namespace qwen3_tts {

TTSTransformer::TTSTransformer()
    : impl_(std::make_unique<tts_transformer_private>()) {
}

TTSTransformer::~TTSTransformer() {
    unload_model();
}

const tts_transformer_config & TTSTransformer::get_config() const {
    return impl_->model.config;
}

void free_transformer_model(tts_transformer_model & model) {
    if (model.buffer) {
        ggml_backend_buffer_free(model.buffer);
        model.buffer = nullptr;
    }
    if (model.ctx) {
        ggml_free(model.ctx);
        model.ctx = nullptr;
    }
    model.tensors.clear();
    model.layers.clear();
    model.code_pred_layers.clear();
    model.code_pred_small_to_mtp_weight = nullptr;
    model.code_pred_small_to_mtp_bias = nullptr;
    model.code_pred_output_norm = nullptr;
    model.code_pred_embd.clear();
    model.code_pred_head.clear();
}

void free_tts_kv_cache(tts_kv_cache & cache) {
    if (cache.buffer) {
        ggml_backend_buffer_free(cache.buffer);
        cache.buffer = nullptr;
    }
    if (cache.ctx) {
        ggml_free(cache.ctx);
        cache.ctx = nullptr;
    }
    cache.k_cache.clear();
    cache.v_cache.clear();
    cache.n_ctx = 0;
    cache.n_used = 0;
}

} // namespace qwen3_tts
