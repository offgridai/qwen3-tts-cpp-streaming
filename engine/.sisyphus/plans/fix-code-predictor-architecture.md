# Fix TTS Transformer Code Predictor Architecture

## Problem Summary

The C++ TTS Transformer implementation has a **fundamental architectural bug** in the code predictor:

**Current (Wrong) Behavior:**
- Generates all 15 codebook codes in a single forward pass
- Codebooks 1-15 are identical across all frames
- Output audio is unintelligible

**Required (Correct) Behavior:**
- Generate codes 1-15 **autoregressively** (one at a time)
- Each code `i` uses embedding of code `i-1` as input
- Code predictor has its own KV cache separate from talker

## Architecture Understanding

### Python Reference Implementation

```
Talker.generate() loop (per frame):
  1. Forward current codec token → get hidden state
  2. Call code_predictor.generate(
       inputs_embeds=[past_hidden, current_token_embedding],
       max_new_tokens=15
     )
     
     Code Predictor generation loop (per codebook):
       For i in 0..14:
         - Use embedding[i] to get input embedding
         - Run transformer forward with KV cache
         - Get logits from lm_head[i]
         - Sample next token
         - Update KV cache
         - Return token i
         
  3. Sum all 16 codec embeddings → next input
  4. Continue to next frame
```

### Key Differences from Current C++

| Aspect | Python | Current C++ | Required C++ |
|--------|--------|-------------|--------------|
| KV Cache | Separate for talker and code predictor | Shared (wrong) | Separate |
| Generation | Autoregressive loop (15 steps) | Single pass | Autoregressive loop |
| Embeddings | Use previous code's embedding | Not using properly | Use previous code's embedding |
| LM Heads | Different head per codebook | Different head per codebook (OK) | Different head per codebook |

## Detailed Work Plan

### Phase 1: Architecture Refactoring (High Priority)

#### Task 1.1: Add Separate KV Cache for Code Predictor
**Files:** `src/tts_transformer.h`, `src/tts_transformer.cpp`

**Changes:**
1. Add `code_pred_kv_cache` struct similar to existing `kv_cache`
2. Add initialization function `init_code_pred_kv_cache()`
3. Add clear function `clear_code_pred_kv_cache()`
4. Update `tts_transformer_state` to include code predictor cache

**Acceptance Criteria:**
- Code predictor KV cache can be initialized independently
- Cache size: `[n_layers=5, n_kv_heads=8, head_dim=128, max_ctx]`
- Memory allocated separately from talker cache

---

#### Task 1.2: Create Autoregressive Code Predictor Graph Builder
**Files:** `src/tts_transformer.cpp`

**Changes:**
1. Create `build_code_pred_step_graph(int32_t n_past, int32_t generation_step)` 
   - Single-step graph (not full sequence)
   - Takes `generation_step` (0-14) to select correct embedding and head
   - Uses KV cache for attention
   
2. Input tensors:
   - `inp_hidden`: [hidden_size] - from talker
   - `inp_code`: [1] - previous code token ID (or 0 for first step)
   - `inp_pos`: [1] - position
   
3. Graph structure:
   ```
   input_embed = code_pred_embd[generation_step](inp_code)
   cur = inp_hidden + input_embed
   
   For each layer:
     - RMSNorm + Attention with KV cache
     - RMSNorm + SwiGLU FFN
     - Residual connections
   
   logits = lm_head[generation_step](cur)
   ```

**Acceptance Criteria:**
- Graph builds successfully for any generation_step (0-14)
- KV cache is properly read/written
- Output is single logits tensor [vocab_size]

---

#### Task 1.3: Implement Autoregressive Generation Loop
**Files:** `src/tts_transformer.cpp`

**Changes:**
Rewrite `predict_codes()` function:

```cpp
bool TTSTransformer::predict_codes_autoregressive(
    const float* hidden,           // [hidden_size] from talker
    const int32_t* prev_codes,     // [15] previous frame's codes 1-15 (or nullptr)
    std::vector<int32_t>& output   // [15] output codes
) {
    output.resize(15);
    
    // Initialize with prev_codes or zeros
    int32_t current_code = (prev_codes != nullptr) ? prev_codes[0] : 0;
    
    for (int step = 0; step < 15; ++step) {
        // Build graph for this step
        struct ggml_cgraph* gf = build_code_pred_step_graph(step);
        
        // Set inputs
        // - hidden: constant across all steps
        // - current_code: previous code (or 0 for step 0)
        // - position: step
        
        // Compute
        // Get logits
        // Argmax to get next_code
        
        output[step] = next_code;
        current_code = next_code;
        
        // KV cache is automatically updated
    }
    
    return true;
}
```

**Acceptance Criteria:**
- Generates 15 codes in a loop
- Each step uses the previous code's value
- KV cache persists across steps
- Output matches Python structure

---

#### Task 1.4: Update Main Generation Loop
**Files:** `src/tts_transformer.cpp`

**Changes:**
Update `generate()` function:

1. Clear code predictor KV cache at start of each frame
2. Call `predict_codes_autoregressive()` instead of `predict_codes()`
3. Properly handle prev_codes for subsequent frames

```cpp
for (int frame = 0; frame < max_len; ++frame) {
    // ... talker forward ...
    
    // Clear code predictor cache for new frame
    clear_code_pred_kv_cache();
    
    // Generate codes 1-15 autoregressively
    std::vector<int32_t> frame_codes_1_15;
    predict_codes_autoregressive(
        hidden.data(),
        (frame > 0) ? prev_codes.data() : nullptr,
        frame_codes_1_15
    );
    
    // Store for next frame
    prev_codes = frame_codes_1_15;
    
    // ... rest of frame processing ...
}
```

**Acceptance Criteria:**
- Each frame generates independent code sequence
- prev_codes properly passed between frames
- Generation completes without errors

---

### Phase 2: Testing & Validation (High Priority)

#### Task 2.1: Create Debug Test for Code Predictor
**Files:** `tests/test_code_predictor.cpp` (new)

**Purpose:** Test code predictor in isolation

**Test Cases:**
1. Single-step prediction (step 0)
2. Multi-step prediction (steps 0-14)
3. Comparison with Python reference

**Acceptance Criteria:**
- Single-step output matches Python
- Multi-step outputs differ (autoregressive nature)
- KV cache properly accumulates

---

#### Task 2.2: Update Transformer Test
**Files:** `tests/test_transformer.cpp`

**Changes:**
1. Update to use new autoregressive API
2. Add frame-by-frame comparison with Python
3. Check that codebooks 1-15 vary across frames

**Acceptance Criteria:**
- First frame codes differ from reference (expected due to randomness)
- Codebooks 1-15 are NOT identical within a frame
- Output shape is correct [n_frames, 16]

---

#### Task 2.3: Full Pipeline Test
**Files:** `tests/test_pipeline.cpp`

**Purpose:** End-to-end test with new transformer

**Acceptance Criteria:**
- Pipeline completes without errors
- Output audio is valid (can be played)
- Audio duration matches expected length

---

### Phase 3: Verification & Comparison (Medium Priority)

#### Task 3.1: Generate Python Reference for Single Frame
**Command:**
```bash
uv run python -c "
from qwen_tts import Qwen3TTSModel
import torch
import numpy as np

model = Qwen3TTSModel.from_pretrained('models/Qwen3-TTS-12Hz-0.6B-Base', device_map='cpu')

# Generate just 1 frame with fixed seed
torch.manual_seed(42)
with torch.no_grad():
    # ... generate 1 frame ...
    codes = model.generate(..., max_new_tokens=1)
    
# Save codes
np.array(codes).tofile('reference/single_frame_codes.bin')
"
```

**Acceptance Criteria:**
- Reference file created
- Contains exactly 16 codes

---

#### Task 3.2: Compare C++ vs Python Single Frame
**Test:** Run C++ transformer to generate 1 frame

**Acceptance Criteria:**
- C++ generates exactly 16 codes
- Codes are in valid range [0, 2047]
- Codebooks 1-15 are different from each other

---

### Phase 4: Integration & Documentation (Medium Priority)

#### Task 4.1: Update Documentation
**Files:** `docs/tensor_mapping.md`, `OPTIMIZATION.md`

**Changes:**
1. Document new code predictor architecture
2. Update performance notes (15x more compute for code predictor)
3. Add architecture diagram

---

#### Task 4.2: Memory Profiling
**Command:**
```bash
/usr/bin/time -v ./build/qwen3-tts-cli ...
```

**Check:**
- Memory increased due to second KV cache (~200MB more)
- Still under 18GB target

---

## Implementation Notes

### Critical Details

1. **Embedding Selection:**
   - Step 0 uses `code_pred_embd[0]` for codebook 1
   - Step 1 uses `code_pred_embd[1]` for codebook 2
   - etc.

2. **LM Head Selection:**
   - Same pattern as embeddings
   - Step 0 uses `lm_head[0]` for codebook 1
   - etc.

3. **KV Cache Management:**
   - Code predictor cache is separate from talker cache
   - Must clear code predictor cache at start of each frame
   - Must NOT clear between steps within a frame

4. **Position IDs:**
   - Use step index (0-14) as position for code predictor
   - This ensures proper attention masking

### Performance Impact

- **Before:** 1 forward pass per frame for code predictor
- **After:** 15 forward passes per frame for code predictor
- **Expected slowdown:** ~15x for code predictor stage
- **Overall impact:** Significant (code predictor becomes bottleneck)

### Risk Mitigation

1. **Test incrementally:** Test single-step first, then multi-step
2. **Compare at each stage:** Compare embeddings, hidden states, logits
3. **Use debug outputs:** Add intermediate tensor outputs for debugging
4. **Keep old implementation:** Comment out, don't delete, for comparison

## Success Criteria

| Criterion | Target | Verification |
|-----------|--------|--------------|
| Code predictor generates 15 steps | Yes | Log output shows 15 iterations |
| Codebooks 1-15 vary within frame | Yes | Check 16 codes are different |
| Codes vary across frames | Yes | Frame N != Frame N+1 |
| Output audio is intelligible | Yes | Human listening test |
| Memory < 18GB | Yes | time -v output |
| Test passes | Yes | ctest output |

## Rollback Plan

If the fix introduces major issues:
1. Comment out new autoregressive code
2. Restore old single-pass code
3. Document findings in issues.md
4. Consider alternative approaches

## Timeline Estimate

| Phase | Tasks | Estimated Time |
|-------|-------|----------------|
| Phase 1 | Architecture refactoring | 3-4 hours |
| Phase 2 | Testing | 2-3 hours |
| Phase 3 | Verification | 1-2 hours |
| Phase 4 | Integration | 1 hour |
| **Total** | | **7-10 hours** |

## Next Steps

1. **Start with Task 1.1** - Add separate KV cache
2. **Delegate to subagent** with category "ultrabrain" for complex logic
3. **Test incrementally** after each task
4. **Document learnings** in notepad after each phase

## Files to Modify

- `src/tts_transformer.h` - Add code predictor cache structure
- `src/tts_transformer.cpp` - Implement autoregressive generation
- `tests/test_transformer.cpp` - Update tests
- `tests/test_code_predictor.cpp` - New test (optional)

## Verification Commands

```bash
# Build
mkdir -p build && cd build && cmake .. && make -j4

# Test transformer
./test_transformer

# Test full pipeline
./qwen3-tts-cli -m ../models -t "Hello world" -o /tmp/test.wav

# Compare audio
uv run python scripts/compare_audio.py --ref reference/output.wav --gen /tmp/test.wav
```
