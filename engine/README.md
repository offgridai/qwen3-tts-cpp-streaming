# TTS engine

`engine/` contains the production C++17 Qwen3-TTS implementation. It owns GGUF loading, text tokenization, speaker embedding extraction, autoregressive codec generation, vocoder decode, streaming callbacks and hints, WAV I/O, the C/C++ APIs, JNI bridge, engine CLI, and quantizer.

Build from the repository root so the engine and streaming wrapper share one GGML configuration:

```powershell
powershell -ExecutionPolicy Bypass -File engine\build.ps1 `
  -UseNinja -EnableCuda -EnableCudaGraphs -Configuration Release
```

Or build the full workspace targets directly:

```powershell
cmake --build build-ninja-cuda --target `
  tts_engine_cli qwen3_streaming_cli tts_engine_quantize
```

Run the engine CLI from the repository root:

```powershell
build-ninja-cuda\engine\tts_engine_cli.exe `
  -m models `
  --model-name qwen3-tts-0.6b-f16.gguf `
  -t "Hello from the engine." `
  -o examples\engine.wav
```

The engine defaults to incremental generation/decode. Use `--batch` for a single full vocoder decode and `--help` for the complete option list.

Public headers:

- `src/qwen3_tts.h`: C++ pipeline API and streaming hint types.
- `src/qwen3_tts_c.h`: C ABI.
- `src/tts_transformer.h`: transformer internals used by the pipeline.
- `src/audio_tokenizer_encoder.h`: speaker encoder.
- `src/audio_tokenizer_decoder.h`: codec decoder/vocoder.

The repository intentionally has no separate component-test layer. Validation uses production target builds plus short model-backed batch and streaming smoke synthesis.
