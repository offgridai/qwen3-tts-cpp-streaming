#include "snake.cuh"
#include "convert.cuh"

#define CUDA_SNAKE_BLOCK_SIZE 256

static __global__ void snake_f32(const float * x, const float * alpha, const float * beta, float * dst, const int64_t ne0, const int64_t ne1, const int64_t n) {
    const int64_t i = (int64_t)blockDim.x * blockIdx.x + threadIdx.x;
    if (i >= n) {
        return;
    }

    // x shape: [ne0, ne1, ne2, ne3]
    // alpha/beta shape: [ne1] (per-channel)
    // We assume 2D or more, where ne1 is the channel dimension.
    // For Snake in Vocos/Qwen3, it's typically [seq_len, channels, batch]
    // But GGML's ne[0] is usually the contiguous dimension (e.g. seq_len).

    const int64_t i0 = i % ne0;
    const int64_t i1 = (i / ne0) % ne1;

    const float val = x[i];
    const float a = alpha[i1];
    const float b = beta[i1];

    // Snake formula: x + (1/b) * sin^2(a * x)
    // To be numerically stable, we use exp(a) and exp(b) if the model was trained with them
    // The reference Python implementation uses:
    // self.alpha = nn.Parameter(torch.ones(1, channels, 1))
    // x + (1 / exp(self.beta)) * torch.pow(torch.sin(exp(self.alpha) * x), 2)
    
    // Wait, let's look at the C++ ref again:
    // struct ggml_tensor * alpha_exp = ggml_exp(ctx, alpha);
    // ...
    // struct ggml_tensor * inv_beta_exp = ggml_exp(ctx, neg_beta);
    
    float ea = expf(a);
    float eb = expf(b);

    dst[i] = val + (1.0f / eb) * powf(sinf(ea * val), 2.0f);
}

void ggml_cuda_op_snake(ggml_backend_cuda_context & ctx, ggml_tensor * dst) {
    const ggml_tensor * src0 = dst->src[0];
    const ggml_tensor * src1 = dst->src[1]; // alpha
    const ggml_tensor * src2 = dst->src[2]; // beta

    void * dst_d = dst->data;
    const float * src0_d = (const float *)src0->data;
    const float * src1_d = (const float *)src1->data;
    const float * src2_d = (const float *)src2->data;

    cudaStream_t stream = ctx.stream();

    GGML_ASSERT(ggml_is_contiguous(dst));
    GGML_ASSERT(ggml_is_contiguous(src0));
    GGML_ASSERT(dst->type == GGML_TYPE_F32);
    GGML_ASSERT(src0->type == GGML_TYPE_F32);

    const int64_t n = ggml_nelements(dst);
    const int64_t ne0 = dst->ne[0];
    const int64_t ne1 = dst->ne[1];

    const int64_t num_blocks = (n + CUDA_SNAKE_BLOCK_SIZE - 1) / CUDA_SNAKE_BLOCK_SIZE;

    snake_f32<<<num_blocks, CUDA_SNAKE_BLOCK_SIZE, 0, stream>>>(src0_d, src1_d, src2_d, (float *)dst_d, ne0, ne1, n);
}
