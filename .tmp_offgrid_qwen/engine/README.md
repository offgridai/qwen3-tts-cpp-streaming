# TTS Engine

`engine/` contains the core native C++ TTS implementation used by this workspace.

It owns:

- text tokenization
- speaker encoding
- transformer inference
- streaming decode
- live playback
- WAV output
- deterministic tests and model tooling

## Build

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 `
  -UseNinja `
  -EnableCuda `
  -EnableCudaGraphs `
  -Configuration Release
```

Primary outputs:

```text
build\Release\tts_engine_cli.exe
build\Release\tts_engine_quantize.exe
```

## Model Setup

The engine expects GGUF artifacts in the workspace-level `models/` directory.

Typical files:

```text
..\models\
  qwen3-tts-0.6b-f16.gguf
  qwen3-tts-1.7b-base-f16.gguf
  qwen3-tts-1.7b-customvoice-f16.gguf
  qwen3-tts-tokenizer-f16.gguf
```

## Basic Usage

From the workspace root:

```powershell
engine\build\Release\tts_engine_cli.exe `
  -m models `
  -t "Hello from the engine layer." `
  -o examples\engine_test.wav
```

Voice cloning with a precomputed speaker embedding:

```powershell
engine\build\Release\tts_engine_cli.exe `
  -m models `
  --model-name qwen3-tts-0.6b-f16.gguf `
  --speaker-embedding reference\alfie_0.6b_f16.json `
  -t "This is generated directly by the engine." `
  -o examples\engine_clone.wav
```

## Layout

```text
src/      Engine source
tests/    Component and regression tests
scripts/  Model conversion, benchmarking, and test helpers
ggml/     GGML dependency source
```

## Notes

- The app harness in `../apps/streaming_cli/` links directly against the engine library.
- Shared runtime assets live at the workspace root so both layers use the same models and references.
