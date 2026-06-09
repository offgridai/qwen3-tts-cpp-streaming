# Workspace Architecture

## Purpose

This repository is organized as a native TTS workspace with a clear split between:

- `engine/`: the core synthesis engine
- `apps/streaming_cli/`: a thin streaming harness used for profiling and regression testing

The engine is part of the repository's primary codebase. It is no longer described or laid out as a third-party subtree.

## High-Level Flow

```text
streaming harness CLI
        ↓
engine C++ API
        ↓
tokenizer + optional speaker prompt + transformer + vocoder
        ↓
24 kHz mono PCM / WAV output
```

## Model Families

The engine currently recognizes three distinct model families:

- `base`
  - clone-oriented path
  - supports speaker embeddings / reference-audio-derived prompts
- `custom_voice`
  - named-speaker path
  - may also accept style instructions depending on model metadata
- `voice_design`
  - instruction-driven persona/voice design path
  - does not use speaker embeddings

This distinction matters because `instruction` is only the transport field. The runtime meaning depends on the loaded model family.

## Layer Responsibilities

### `engine/`

Owns:

- GGUF model loading
- tokenizer
- instruction tokenization and prompt assembly
- speaker embedding extraction and reuse
- autoregressive code generation
- streaming decode strategy
- live playback support
- native engine CLI
- correctness tests and model tooling

### `apps/streaming_cli/`

Owns:

- harness-specific CLI surface
- profile aliases like `realtime`, `memory-saver`, and `ultra-low`
- explicit VoiceDesign CLI ergonomics
- wrapper-specific defaults for latency experiments
- integration-oriented packaging of the engine API

## Important Change

The wrapper no longer launches the engine CLI as a subprocess.

Old path:

```text
wrapper CLI → shell out to engine CLI
```

Current path:

```text
wrapper CLI → direct link to engine library
```

That removes:

- executable path discovery
- command-string assembly
- duplicated flag forwarding
- brittle runtime coupling between two binaries

## VoiceDesign Support

VoiceDesign is now exposed as a first-class wrapper feature:

- the wrapper detects `voice_design` model metadata from the engine
- `--voice-design` and `--voice-design-instruct` make the intended path explicit
- speaker embeddings are rejected for VoiceDesign models
- a VoiceDesign model requires non-empty instruction text

The current recommended usage pattern is:

1. use `voice_design` when you want to create or audition a new persona
2. use the faster 0.6B runtime path for repeated low-latency lines when possible

## Build Layout

```text
root CMakeLists.txt
├── add_subdirectory(engine)
└── add_subdirectory(apps/streaming_cli)
```

This produces two primary executables:

- `tts_engine_cli`
- `qwen3_streaming_cli`

## Shared Assets

The workspace keeps shared runtime data at the root:

- `models/`
- `reference/`
- `examples/`

That lets both layers operate on the same assets without maintaining duplicate copies under the engine or wrapper.

## Consolidation Decisions

To reduce redundancy:

- duplicated top-level engine scripts were removed
- the engine remains the canonical home for model setup, benchmarking, and engine test tooling
- the app layer contains only wrapper-specific source and build files

## Near-Term Roadmap

Remaining cleanup that still makes sense after this restructuring:

1. Add a true reusable "voice design then clone" workflow
2. Add a streaming chunk callback API to the engine layer
3. Move more wrapper presets into explicit config objects
4. Add harness-specific integration tests for VoiceDesign and profile validation
