# qwen3-tts-cpp-streaming

Native streaming TTS workspace with two explicit layers:

- `engine/`: the core C++ TTS engine
- `apps/streaming_cli/`: the thin streaming test harness

The repository no longer treats the engine as a vendored third-party dependency. It is part of the workspace and builds directly with the app layer.

## Repository Layout

```text
apps/
  streaming_cli/     Thin wrapper CLI for streaming experiments
engine/              Core TTS engine, CLI, tests, and model tooling
docs/                Workspace-level architecture and benchmark notes
examples/            Generated WAV examples
models/              Shared GGUF model artifacts
reference/           Shared speaker embeddings and reference audio
```

## Build

### 1. Build the core engine CLI

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\engine\build.ps1 `
  -UseNinja `
  -EnableCuda `
  -EnableCudaGraphs `
  -Configuration Release
```

Expected engine output:

```text
engine\build\Release\tts_engine_cli.exe
```

### 2. Build the streaming harness

```powershell
cmake -S . -B build `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DGGML_CUDA=ON

cmake --build build --config Release --target qwen3_streaming_cli
```

Expected harness output:

```text
build\Release\qwen3_streaming_cli.exe
```

The top-level build adds both `engine/` and `apps/streaming_cli/`, so you can also build both from the root workspace:

```powershell
cmake --build build --config Release --target tts_engine_cli qwen3_streaming_cli
```

## Models

Shared models live under `models/`.

Common files:

```text
models\
  qwen3-tts-0.6b-f16.gguf
  qwen3-tts-1.7b-base-f16.gguf
  qwen3-tts-1.7b-customvoice-f16.gguf
  qwen3-tts-tokenizer-f16.gguf
```

The engine keeps its own conversion and setup tooling under `engine/scripts/`.

## Runtime

### Engine CLI

Basic engine invocation:

```powershell
engine\build\Release\tts_engine_cli.exe `
  -m models `
  -t "Hello from the engine layer." `
  -o examples\engine_test.wav
```

### Streaming Harness

The wrapper now links directly to the engine library instead of launching a sibling executable via `std::system`.

Example:

```powershell
build\Release\qwen3_streaming_cli.exe `
  -m models `
  --model-identifier qwen3-tts-0.6b-f16 `
  --speaker-embedding reference\alfie_0.6b_f16.json `
  -t "Hello. Welcome to Alfie's Bodega. How can I help you today?" `
  --instruct "happy" `
  -o examples\alfie_06b_f16.wav
```

Realtime-oriented preset:

```powershell
build\Release\qwen3_streaming_cli.exe `
  -m models `
  --tts-profile realtime `
  --speaker-embedding reference\alfie_0.6b_f16.json `
  -t "Hello. Welcome to Alfie's Bodega. How can I help you today?" `
  -o examples\alfie_realtime.wav
```

VoiceDesign example:

```powershell
build\Release\qwen3_streaming_cli.exe `
  -m models `
  --voice-design `
  --model-name qwen3-tts-1.7b-voicedesign-f16 `
  --voice-design-instruct "A calm, deep male narrator with a restrained delivery." `
  -t "I was not expecting visitors this late." `
  -o examples\voice_design.wav
```

VoiceDesign notes:

- The VoiceDesign workflow uses the distinct `qwen3-tts-1.7b-voicedesign-f16.gguf` model family.
- `--voice-design-instruct` is the primary control surface for VoiceDesign runs.
- Speaker embeddings are rejected for VoiceDesign models.
- If a VoiceDesign model is loaded, the wrapper auto-detects it and enforces the correct input rules.

## Design Notes

- `engine/` owns model loading, generation, streaming decode, playback behavior, and audio file output.
- `apps/streaming_cli/` owns harness presets, CLI ergonomics, and experiment-oriented defaults.
- Shared assets remain top-level so both layers use the same `models/`, `reference/`, and `examples/` directories.

See [docs/architecture.md](C:/git/qwen3-tts-cpp-streaming/docs/architecture.md) for the current workspace architecture.
