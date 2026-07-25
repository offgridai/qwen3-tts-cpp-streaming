# TTS engine

`engine/` contains the production C++17 Qwen3-TTS implementation. It owns GGUF loading, text tokenization, speaker embedding extraction, autoregressive codec generation, vocoder decode, streaming PCM callbacks, WAV I/O, the C++ API, engine CLI, and quantizer.

Build from the repository root so the engine and streaming wrapper share one GGML configuration:

```powershell
powershell -ExecutionPolicy Bypass -File engine\build.ps1 `
  -UseNinja -EnableCuda -EnableCudaGraphs -Configuration Release
```

Or build the full workspace targets directly:

```powershell
cmake --build build-ninja-cuda --target `
  tts_engine_cli qwen3_tts_streaming qwen3_streaming_cli tts_engine_quantize
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

- `src/qwen3_tts.h`: C++ pipeline and streaming callback API.
- `src/tts_transformer.h`: transformer internals used by the pipeline.
- `src/audio_tokenizer_encoder.h`: speaker encoder.
- `src/audio_tokenizer_decoder.h`: codec decoder/vocoder.

The default CTest layer contains fast wrapper contract checks. Model-backed streaming, fidelity, cadence, and reliability checks are orchestrated by `tools/acceptance.py` from the repository root.
