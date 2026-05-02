#include "audio_tokenizer_decoder.h"
#include "decoder/decoder_state_internal.h"

#include <chrono>
#include <cstdio>

namespace qwen3_tts {

bool AudioTokenizerDecoder::decode(const int32_t * codes, int32_t n_frames,
                                    std::vector<float> & samples) {
    auto & model = impl_->model;
    auto & state = impl_->state;
    auto & error_msg = impl_->error_msg;
    auto & codebook_input_bufs = impl_->codebook_input_bufs;
    auto & positions_buf = impl_->positions_buf;

    if (!model.ctx) {
        error_msg = "Model not loaded";
        return false;
    }

    const auto & cfg = model.config;

    if (!decoder_internal::ops::ensure_cached_decode_graph(*this, n_frames)) {
        return false;
    }

    struct ggml_cgraph * gf = state.decode_graph;

    if (!ggml_backend_sched_alloc_graph(state.sched, gf)) {
        error_msg = "Failed to allocate graph";
        return false;
    }

    if ((int32_t) codebook_input_bufs.size() != cfg.n_codebooks) {
        codebook_input_bufs.assign(cfg.n_codebooks, {});
    }
    for (int cb = 0; cb < cfg.n_codebooks; ++cb) {
        codebook_input_bufs[cb].resize(n_frames);
    }

    for (int f = 0; f < n_frames; ++f) {
        const int32_t * frame_codes = codes + (size_t) f * cfg.n_codebooks;
        for (int cb = 0; cb < cfg.n_codebooks; ++cb) {
            codebook_input_bufs[cb][f] = frame_codes[cb];
        }
    }

    for (int cb = 0; cb < cfg.n_codebooks; ++cb) {
        ggml_backend_tensor_set(state.decode_code_tensors[cb], codebook_input_bufs[cb].data(), 0,
                                (size_t) n_frames * sizeof(int32_t));
    }

    if ((int32_t) positions_buf.size() != n_frames) {
        positions_buf.resize(n_frames);
        for (int i = 0; i < n_frames; ++i) {
            positions_buf[i] = i;
        }
    }
    if (state.decode_positions_tensor) {
        ggml_backend_tensor_set(state.decode_positions_tensor, positions_buf.data(), 0,
                                (size_t) n_frames * sizeof(int32_t));
    }

    if (ggml_backend_sched_graph_compute(state.sched, gf) != GGML_STATUS_SUCCESS) {
        error_msg = "Failed to compute graph";
        ggml_backend_sched_reset(state.sched);
        return false;
    }

    struct ggml_tensor * audio_tensor = state.decode_audio_tensor;
    if (!audio_tensor) {
        error_msg = "Failed to find audio tensor";
        ggml_backend_sched_reset(state.sched);
        return false;
    }

    int64_t n_samples = audio_tensor->ne[0];
    samples.resize(n_samples);
    ggml_backend_tensor_get(audio_tensor, samples.data(), 0, n_samples * sizeof(float));

    ggml_backend_sched_reset(state.sched);

    return true;
}

bool AudioTokenizerDecoder::profile_decode(const int32_t * codes, int32_t n_frames,
                                           std::vector<decoder_profile_row> & rows) {
    rows.clear();

    auto & model = impl_->model;
    auto & state = impl_->state;
    auto & error_msg = impl_->error_msg;
    auto & codebook_input_bufs = impl_->codebook_input_bufs;
    auto & positions_buf = impl_->positions_buf;

    if (!model.ctx) {
        error_msg = "Model not loaded";
        return false;
    }
    if (!codes || n_frames <= 0) {
        error_msg = "Invalid decoder profile input";
        return false;
    }

    const auto & cfg = model.config;
    if ((int32_t) codebook_input_bufs.size() != cfg.n_codebooks) {
        codebook_input_bufs.assign(cfg.n_codebooks, {});
    }
    for (int cb = 0; cb < cfg.n_codebooks; ++cb) {
        codebook_input_bufs[cb].resize(n_frames);
    }
    for (int f = 0; f < n_frames; ++f) {
        const int32_t * frame_codes = codes + (size_t) f * cfg.n_codebooks;
        for (int cb = 0; cb < cfg.n_codebooks; ++cb) {
            codebook_input_bufs[cb][f] = frame_codes[cb];
        }
    }
    positions_buf.resize(n_frames);
    for (int i = 0; i < n_frames; ++i) {
        positions_buf[i] = i;
    }

    struct stage_spec {
        decoder_internal::profile_stage stage;
        const char * name;
    };

    const stage_spec stages[] = {
        {decoder_internal::profile_stage::vq_output, "rvq_lookup_project"},
        {decoder_internal::profile_stage::pre_conv, "pre_conv"},
        {decoder_internal::profile_stage::pre_tfm, "pre_transformer"},
        {decoder_internal::profile_stage::upsample_frontend, "frontend_upsample_x4"},
        {decoder_internal::profile_stage::dec0, "decoder_pre_conv"},
        {decoder_internal::profile_stage::dec1, "decoder_block_1_x8"},
        {decoder_internal::profile_stage::dec2, "decoder_block_2_x5"},
        {decoder_internal::profile_stage::dec3, "decoder_block_3_x4"},
        {decoder_internal::profile_stage::dec4, "decoder_block_4_x3"},
        {decoder_internal::profile_stage::final_audio, "final_conv_tanh"},
    };

    int64_t prev_ms = 0;

    for (const stage_spec & spec : stages) {
        struct ggml_context * graph_ctx = nullptr;
        struct ggml_cgraph * gf = decoder_internal::ops::build_graph_impl(*this, n_frames, &graph_ctx, spec.stage);
        if (!gf || !graph_ctx) {
            error_msg = std::string("Failed to build decoder profile graph for ") + spec.name;
            if (graph_ctx) {
                ggml_free(graph_ctx);
            }
            ggml_backend_sched_reset(state.sched);
            return false;
        }

        if (!ggml_backend_sched_alloc_graph(state.sched, gf)) {
            error_msg = std::string("Failed to allocate decoder profile graph for ") + spec.name;
            ggml_free(graph_ctx);
            ggml_backend_sched_reset(state.sched);
            return false;
        }

        for (int cb = 0; cb < cfg.n_codebooks; ++cb) {
            char name[32];
            snprintf(name, sizeof(name), "codes_cb%d", cb);
            struct ggml_tensor * code_tensor = ggml_graph_get_tensor(gf, name);
            if (!code_tensor) {
                error_msg = std::string("Failed to find decoder profile input tensor ") + name;
                ggml_free(graph_ctx);
                ggml_backend_sched_reset(state.sched);
                return false;
            }
            ggml_backend_tensor_set(code_tensor, codebook_input_bufs[cb].data(), 0,
                                    (size_t) n_frames * sizeof(int32_t));
        }

        if (struct ggml_tensor * positions = ggml_graph_get_tensor(gf, "positions")) {
            ggml_backend_tensor_set(positions, positions_buf.data(), 0,
                                    (size_t) n_frames * sizeof(int32_t));
        }

        const auto t0 = std::chrono::steady_clock::now();
        if (ggml_backend_sched_graph_compute(state.sched, gf) != GGML_STATUS_SUCCESS) {
            error_msg = std::string("Failed to compute decoder profile graph for ") + spec.name;
            ggml_free(graph_ctx);
            ggml_backend_sched_reset(state.sched);
            return false;
        }
        const auto t1 = std::chrono::steady_clock::now();
        const int64_t cumulative_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        struct ggml_tensor * out = ggml_graph_get_tensor(gf, "profile_output");
        if (!out) {
            out = ggml_graph_get_tensor(gf, "audio");
        }

        decoder_profile_row row;
        row.stage = spec.name;
        row.cumulative_ms = cumulative_ms;
        row.delta_ms = cumulative_ms - prev_ms;
        prev_ms = cumulative_ms;
        if (out) {
            for (int i = 0; i < GGML_MAX_DIMS; ++i) {
                if (out->ne[i] > 1 || i == 0) {
                    row.shape.push_back(out->ne[i]);
                }
            }
            row.bytes = ggml_nbytes(out);
        }
        rows.push_back(row);

        ggml_backend_sched_reset(state.sched);
        ggml_free(graph_ctx);
    }

    return true;
}

} // namespace qwen3_tts
