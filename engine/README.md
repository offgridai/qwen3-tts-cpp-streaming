# TTS Engine

`engine/` contains the core native C++ TTS implementation used by this workspace.

It owns:

- text tokenization
- speaker encoding
- transformer inference
- streaming decode
- streaming hint-track emission
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

## Streaming Hint Track

The engine now exposes optional streaming hint callbacks through
`engine/src/qwen3_tts.h`.

Primary types:

- `tts_stream_hint_header`
- `tts_stream_hint_chunk`
- `tts_hint_energy_class`

Primary callbacks on `tts_params`:

- `stream_hint_header_callback`
- `stream_hint_chunk_callback`

The header callback fires before the first streamed audio chunk and exposes:

- `sample_rate`
- `model_type`
- `has_instruction`
- `has_speaker_conditioning`

The chunk callback exposes:

- `chunk_index`
- `codec_frame_start`
- `codec_frame_end`
- `audio_sample_start`
- `audio_sample_end`
- `audio_start_sec`
- `audio_end_sec`
- `rms_energy`
- `peak_energy`
- `zero_crossing_rate`
- `energy_class`
- `text_progress`
- `text_token_index_estimate`
- `text_progress_confidence`
- `is_text_progress_experimental`
- `is_paced_chunk`
- `is_final`

Scope note:

- this is a timing/provenance track, not a linguistic alignment layer
- the engine does not emit words, phonemes, visemes, or final lipsync timings
- `audio_sample_end` and `audio_end_sec` are exclusive-end
- `text_progress` is experimental and should be treated as a soft monotonic prior, not authority
- it is derived from full text-token projection similarity against per-frame talker hidden states, not from forced alignment

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
