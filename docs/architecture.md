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
tokenizer + speaker encoder + transformer + vocoder
        ↓
24 kHz mono PCM / WAV output
```

## Layer Responsibilities

### `engine/`

Owns:

- GGUF model loading
- tokenizer
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

## Roadmap

Remaining cleanup that still makes sense after this restructuring:

1. Rename internal namespaces and source file names away from `qwen3_tts`
2. Add a true streaming chunk callback API to the engine layer
3. Move more wrapper presets into explicit config objects
4. Add app-specific tests for the harness layer
