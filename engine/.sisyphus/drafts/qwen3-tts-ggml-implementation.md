# Draft: Qwen3-TTS GGML Implementation

## Requirements (confirmed)

- **Target Model**: Qwen3-TTS-12Hz-0.6B-Base (voice cloning capability)
- **Sample Audio**: ./clone.wav (provided)
- **Reference Text**: "Okay. Yeah. I resent you. I love you. I respect you. But you know what? You blew it! And thanks to you."
- **Quantization**: Q8_0 minimum, additional quant support desired
- **Hardware Constraints**: 24GB RAM, 4 CPU cores
- **Backend Support**: ggml_backend_sched for future multi-backend support
- **Testing Strategy**: Step-by-step comparison with original Python pipeline intermediate results
- **No Large Dependencies**: No onnxruntime, tensorflow - lightweight libraries like kaldi-native-fbank acceptable

## Technical Decisions

### Architecture Understanding (from HuggingFace/GitHub research)

**Qwen3-TTS Pipeline:**
```
Text Input + Reference Audio
       ↓
   Text Tokenizer (BPE - Qwen-style)
       ↓
   Reference Audio → Audio Tokenizer Encoder → Speaker Embedding / Reference Codes
       ↓
   TTS Transformer (Qwen2-based LM) → Speech Codes (multi-codebook)
       ↓
   Audio Tokenizer Decoder (Neural Vocoder) → Waveform
```

**Key Components:**
1. **Qwen3-TTS-Tokenizer-12Hz**: Separate model that:
   - Encodes audio to discrete multi-codebook codes (for reference audio)
   - Decodes discrete codes back to waveform (neural vocoder)
   - 12Hz = 12 frames per second (very low frame rate for efficiency)

2. **TTS Transformer (0.6B)**: 
   - Qwen2-based LM architecture
   - Takes text tokens + reference audio embeddings
   - Generates speech code tokens autoregressively

### Reference Implementation Available

At /root/qwen-3-asr-ggml, we have working GGML code for:
- GGUF model loading (gguf_loader.cpp/h)
- Audio encoder with conv2d + transformer (audio_encoder.cpp/h)
- Text decoder with KV cache (text_decoder.cpp/h) - Qwen2 architecture
- Mel spectrogram computation (mel_spectrogram.cpp/h)
- Audio injection for multimodal (audio_injection.cpp/h)
- CMake build system with GGML integration
- Python GGUF conversion script
- Quantization support (Q8_0, Q4_K)

### Differences from ASR

| ASR | TTS |
|-----|-----|
| Audio → Text | Text → Audio |
| Mel spectrogram input | Text token input |
| Audio encoder needed | Audio decoder (vocoder) needed |
| Text decoder generates text tokens | TTS model generates speech codes |
| Single output (text) | Multi-codebook output (speech codes) |

### TTS-Specific Components to Build

1. **Audio Tokenizer Encoder**: For encoding reference audio to embeddings
   - Likely similar architecture to audio codec encoders
   - Need to extract from Qwen3-TTS-Tokenizer-12Hz

2. **TTS Transformer**: Modified text decoder
   - Input: text tokens + reference audio embeddings
   - Output: speech code tokens (multi-codebook)
   - Similar to ASR's text_decoder but different input/output

3. **Audio Tokenizer Decoder**: Neural vocoder
   - Input: speech code tokens
   - Output: waveform samples
   - Likely uses transposed convolutions / upsampling
   - This is the most novel component

## Research Findings

### From HuggingFace Model Card:
- Discrete multi-codebook LM architecture
- End-to-end speech modeling
- 12Hz frame rate (very efficient)
- Voice cloning from 3-second reference audio
- Supports 10 languages

### From GitHub Repository:
- Python package: qwen_tts
- Uses: torch, soundfile
- Key classes: Qwen3TTSModel, Qwen3TTSTokenizer
- Reference audio processing: ref_audio → embeddings
- x_vector extraction for speaker embedding

### Model Files (from HuggingFace):
- Qwen3-TTS-Tokenizer-12Hz: Separate tokenizer model
- Qwen3-TTS-12Hz-0.6B-Base: Base model for voice cloning

## Test Strategy Decision

- **Infrastructure exists**: Will create test infrastructure
- **User wants tests**: Step-by-step comparison with Python reference
- **Framework**: Manual verification via comparison scripts
- **QA approach**: Compare intermediate outputs at each pipeline stage

### Testing Checkpoints:
1. Text tokenization output
2. Reference audio encoding (speaker embedding)
3. TTS transformer output (speech codes)
4. Audio tokenizer decoder output (waveform)
5. Final audio similarity metrics

## Scope Boundaries

### INCLUDE:
- Full TTS pipeline (text → audio)
- Voice cloning with reference audio
- Q8_0 quantization support
- ggml_backend_sched usage
- Testing suite comparing with Python reference
- GGUF model conversion script
- CLI interface

### EXCLUDE:
- Voice design functionality (instruction-based control)
- Streaming generation (initial implementation is batch)
- GPU backends (initial implementation is CPU only)
- Training/fine-tuning support
- Languages other than English (initial testing)

## Open Questions (Resolved by Research)

1. ~~Architecture details~~ → Understood from HuggingFace/GitHub
2. ~~Multi-codebook handling~~ → Will study Tokenizer-12Hz model structure
3. ~~Vocoder architecture~~ → Extract from model inspection

## Implementation Order

### Phase 1: Foundation
1. Clone dependencies (ggml, llama.cpp for reference)
2. Set up Python environment for reference pipeline
3. Download models and run reference pipeline
4. Save intermediate outputs for testing

### Phase 2: Model Inspection
1. Inspect Tokenizer-12Hz model structure
2. Inspect TTS-12Hz-0.6B-Base model structure
3. Create tensor mapping for GGUF conversion

### Phase 3: GGUF Conversion
1. Implement conversion script (adapt from ASR)
2. Handle multi-component models
3. Add quantization support

### Phase 4: Core Components (C++)
1. Audio Tokenizer Encoder (reference audio → embedding)
2. TTS Transformer (text + ref → speech codes)
3. Audio Tokenizer Decoder (speech codes → waveform)

### Phase 5: Integration
1. Full pipeline integration
2. CLI interface
3. End-to-end testing

### Phase 6: Optimization
1. Memory optimization for 24GB constraint
2. Multi-threading for 4-core CPU
3. Performance benchmarking
