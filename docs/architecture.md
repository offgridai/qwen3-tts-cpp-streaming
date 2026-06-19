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
24 kHz mono PCM + optional streaming hint metadata
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
- streaming hint-track emission
- optional live playback
- engine CLI

### `apps/streaming_cli/`

Owns:

- harness-facing CLI surface
- profile aliases and test ergonomics
- wrapper defaults for streaming experiments
- VoiceDesign-specific UX checks
- chunk callback wiring for integration-style tests
- wrapper-side hint metadata transport

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

The same streaming path now also supports an optional hint track intended for
downstream runtime timing systems.

The hint track is deliberately non-linguistic. It carries:

- stream header metadata such as sample rate, model type, and conditioning presence
- exact emitted sample ranges
- codec-frame provenance for each emitted chunk
- cheap PCM-derived evidence such as RMS, peak, and zero-crossing rate
- a heuristic energy class: `unknown`, `silence`, `speech_like`, or `burst_like`
- an experimental monotonic `text_progress` hint derived from full text-token projection similarity against per-frame talker hidden states

The hint track does not attempt to provide:

- words
- phonemes
- visemes
- forced alignment
- final lipsync timings

Those belong in downstream systems that sit above the TTS engine.

Current default startup/steady policy:

- `first_tail_window_frames=5`
- `ramp_tail_window_frames=6`
- `ramp_tail_window_count=0`
- `steady_tail_window_frames=8`
- `context_frames=3`
- `early_context_frames=2`
- `early_context_window_count=2`
- `adaptive_steady_windows=off`
- `adaptive_min_tail_window_frames=6`
- `delivery_chunk_ms=40`
- `delivery_start_buffer_ms=40`
- `delivery_target_lead_ms=350`
- `steady_split_decode_frames=4`

These defaults are a balanced standalone profile. They work well for the built-in local player and remain a reasonable baseline for integrations, but they are not the most aggressive callback-oriented settings.

For callback-driven consumers such as Offgrid/LineCoach, the CLI now exposes an explicit `offgrid-callback` profile:

- `first_tail_window_frames=3`
- `ramp_tail_window_frames=6`
- `ramp_tail_window_count=0`
- `steady_tail_window_frames=8`
- `context_frames=2`
- `early_context_frames=1`
- `early_context_window_count=2`
- `final_context_frames=3`
- `adaptive_steady_windows=off`
- `delivery_chunk_ms=80`
- `delivery_start_buffer_ms=80`
- `delivery_target_lead_ms=300`
- `steady_split_decode_frames=0`

That profile is intended for clients that maintain their own playback queue and want smaller callback arrivals. It is not recommended for the standalone CLI live player, which performs better with the balanced default profile.

## Hint Track Semantics

The current hint payload is aligned to emitted audio chunks, not to a separate
phonetic or linguistic timeline.

Important conventions:

- `audio_sample_start` and `audio_sample_end` refer to absolute emitted sample positions
- `audio_sample_end` is exclusive
- `audio_start_sec` and `audio_end_sec` are derived directly from those sample offsets
- `codec_frame_start` and `codec_frame_end` describe the generated codec-frame interval
  that contributed newly emitted audio to the chunk
- `is_paced_chunk=true` means the chunk came from paced subchunk delivery rather than
  a single direct decode-window emission
- `text_progress` is a soft monotonic hint in `[0, 1]`, not a word or phoneme alignment result
- `text_token_index_estimate` is an estimate over the encoded text-token sequence only
- `text_progress_confidence` should be treated as a conservative heuristic confidence, not a calibrated probability

Because the engine supports overlap trimming and paced subchunk slicing, a single
callback chunk is not guaranteed to correspond one-to-one with a single codec frame.
Frame provenance is therefore expressed as a range.

## VoiceDesign Support

VoiceDesign is a first-class workflow in the harness:

- the wrapper detects `voice_design` metadata from the engine
- `--voice-design` and `--voice-design-instruct` make the path explicit
- speaker embeddings are rejected for VoiceDesign models
- VoiceDesign requires non-empty instruction text
- fixed-profile VoiceDesign can reuse cached instruction tokens
- fixed-profile VoiceDesign can do a one-time warmup pass keyed by profile identity

Recommended usage pattern:

1. use VoiceDesign when creating or auditioning a persona
2. when a persona remains fixed across many lines, reuse a stable cache key
3. optionally pay one warmup request once per profile before the hot line path

These two reuse mechanisms improve repeated-request startup cost, but they do not materially alter steady streaming decode cadence. Burstiness remains primarily a decode/vocoder scheduling concern.

If the app changes the instruction text per line, do not force all of those lines through the same instruction-token cache key. That would intentionally reuse stale instruction tokens. In that case, keep exact-instruction token caching and reserve the stable profile key for one-time persona warmup.

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
