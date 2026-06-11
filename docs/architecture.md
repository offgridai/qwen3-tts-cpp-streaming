# Workspace Architecture

## Purpose

This repository is a native Qwen3 TTS workspace with two explicit layers:

- `engine/`: the core C++ TTS engine
- `apps/streaming_cli/`: a thin streaming harness for experiments, profiling, and integration-style testing

The engine is first-party code in this repository. It is not treated as a vendored third-party subtree.

## High-Level Flow

```text
streaming_cli
    ->
engine C++ API
    ->
tokenizer + optional instruction/speaker conditioning + transformer + vocoder
    ->
24 kHz mono PCM
    ->
live playback and/or WAV output
```

## Model Families

The engine currently recognizes three model families:

- `base`
  - general TTS path
  - may be used with speaker embeddings / cloned prompts depending on model assets
- `custom_voice`
  - speaker-oriented path
  - supports named or embedding-driven voice selection
- `voice_design`
  - instruction-driven persona design
  - does not use speaker embeddings

`instruction` is just the transport field. Its runtime meaning depends on the loaded model family.

## Layer Responsibilities

### `engine/`

Owns:

- GGUF model loading
- tokenizer and prompt assembly
- speaker embedding extraction and loading
- autoregressive speech-code generation
- streaming decode policy
- paced PCM delivery
- optional live playback
- engine CLI

### `apps/streaming_cli/`

Owns:

- harness-facing CLI surface
- profile aliases and test ergonomics
- wrapper defaults for streaming experiments
- VoiceDesign-specific UX checks
- chunk callback wiring for integration-style tests

## Important Structural Decision

The streaming harness links directly against the engine library.

Old shape:

```text
wrapper CLI -> shell out to engine CLI
```

Current shape:

```text
wrapper CLI -> direct engine API call
```

That removes duplicated flag forwarding, subprocess orchestration, and binary-to-binary coupling.

## Streaming Design

The current low-latency path is built around:

- small first decode window
- a short ramp before steady-state windows
- reduced early left-context
- adaptive steady windows when queue depth falls
- paced PCM delivery for downstream consumers

Current default startup/steady policy:

- `first_tail_window_frames=3`
- `ramp_tail_window_frames=5`
- `ramp_tail_window_count=2`
- `steady_tail_window_frames=8`
- `context_frames=3`
- `early_context_frames=2`
- `early_context_window_count=2`
- `adaptive_steady_windows=on`
- `adaptive_min_tail_window_frames=6`
- `delivery_chunk_ms=80`
- `delivery_start_buffer_ms=80`
- `delivery_target_lead_ms=240`

These defaults are aimed at callback-driven consumers such as game/runtime integrations, not just the standalone live player.

## VoiceDesign Support

VoiceDesign is a first-class workflow in the harness:

- the wrapper detects `voice_design` metadata from the engine
- `--voice-design` and `--voice-design-instruct` make the path explicit
- speaker embeddings are rejected for VoiceDesign models
- VoiceDesign requires non-empty instruction text

Recommended usage pattern:

1. use VoiceDesign when creating or auditioning a persona
2. use the faster runtime path for repeated low-latency lines when possible

## Build Layout

```text
root CMakeLists.txt
|-- add_subdirectory(engine)
`-- add_subdirectory(apps/streaming_cli)
```

Primary binaries:

- `tts_engine_cli`
- `qwen3_streaming_cli`

## Shared Assets

Shared runtime assets remain at the workspace root:

- `models/`
- `reference/`
- `examples/`

That keeps engine and harness runs on the same model and reference data.
