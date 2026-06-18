# qwen3-tts-cpp-streaming

Native Qwen3 TTS workspace with two explicit layers:

- `engine/`: the core C++ TTS engine
- `apps/streaming_cli/`: a thin streaming harness for experiments and integration-style validation

The engine is part of this repository's primary codebase. It is not treated as a vendored third-party dependency.

## Repository Layout

```text
apps/
  streaming_cli/     Thin wrapper CLI for streaming and callback experiments
docs/                Architecture and baseline notes
engine/              Core TTS engine, engine CLI, and pipeline code
examples/            Generated WAV outputs
models/              Shared GGUF model artifacts
reference/           Speaker embeddings and reference assets
```

## What This Repository Is For

This workspace is primarily for:

- native local TTS experimentation
- streaming decode and pacing work
- VoiceDesign testing
- validating callback-driven streaming behavior before integrating into another product
- exposing streaming hint metadata for downstream timing/lipsync systems

The most useful artifact for downstream integration work is usually `qwen3_streaming_cli`, because it exercises the same engine path while exposing streaming-oriented controls and diagnostics.

## Build

Use a Visual Studio Developer PowerShell / Developer Command Prompt, or import the
MSVC toolchain environment before invoking CMake. A plain PowerShell session can
find `cl.exe` but still miss the standard include/library paths, which produces
errors such as `cannot open include file: 'stdbool.h'`.

### Visual Studio x64 build

Configure:

```powershell
cmake -S . -B build-vs2022-x64 `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DQWEN3_TTS_COREML=OFF `
  -DQWEN3_TTS_EMBED_GGML=ON `
  -DQWEN3_TTS_CUDA=ON `
  -DGGML_CUDA=ON `
  -DGGML_CUDA_GRAPHS=ON
```

Build the wrapper CLI:

```powershell
cmake --build build-vs2022-x64 --config Release --target qwen3_streaming_cli
```

Build the engine CLI too:

```powershell
cmake --build build-vs2022-x64 --config Release --target tts_engine_cli qwen3_streaming_cli
```

Expected outputs:

```text
build-vs2022-x64\apps\streaming_cli\Release\qwen3_streaming_cli.exe
build-vs2022-x64\engine\Release\tts_engine_cli.exe
```

### Ninja build

If you already use the Ninja build tree in this repo, run the build from a Visual
Studio developer shell:

```powershell
cmake --build build-ninja-cuda --target qwen3_streaming_cli
```

From a plain PowerShell session, this equivalent wrapper also works:

```powershell
$vsdev = '"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -host_arch=x64 -arch=x64 >nul'
$cmake = '"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build-ninja-cuda --target qwen3_streaming_cli'
cmd /c "$vsdev && $cmake"
```

## Windows Runtime DLLs

On Windows, `qwen3_streaming_cli.exe` needs the `ggml*.dll` files beside it or on `PATH`.

If the app reports a missing `ggml-base.dll`, copy the DLLs from:

```text
build-vs2022-x64\bin\Release\
```

into:

```text
build-vs2022-x64\apps\streaming_cli\Release\
```

or add `build-vs2022-x64\bin\Release` to `PATH` before launching.

## Models

Shared models live under `models/`.

Common files in active use:

```text
models\
  qwen3-tts-0.6b-f16.gguf
  qwen3-tts-1.7b-base-f16.gguf
  qwen3-tts-1.7b-customvoice-f16.gguf
  qwen3-tts-1.7b-voicedesign-f16.gguf
  qwen3-tts-tokenizer-f16.gguf
```

VoiceDesign uses the distinct `qwen3-tts-1.7b-voicedesign-f16.gguf` model family.

## Running

### Engine CLI

Basic engine invocation:

```powershell
build-vs2022-x64\engine\Release\tts_engine_cli.exe `
  -m models `
  -t "Hello from the engine layer." `
  -o examples\engine_test.wav
```

### Streaming CLI

Custom voice / speaker embedding example:

```powershell
build-vs2022-x64\apps\streaming_cli\Release\qwen3_streaming_cli.exe `
  -m models `
  --model-identifier qwen3-tts-0.6b-f16 `
  --speaker-embedding reference\alfie_0.6b_f16.json `
  -t "Hello. Welcome to Alfie's Bodega. How can I help you today?" `
  -o examples\streaming_test.wav
```

VoiceDesign example:

```powershell
build-vs2022-x64\apps\streaming_cli\Release\qwen3_streaming_cli.exe `
  -m models `
  --voice-design `
  --model-name qwen3-tts-1.7b-voicedesign-f16 `
  --voice-design-instruct "A calm, deep male narrator with a restrained delivery." `
  -t "I was not expecting visitors this late." `
  -o examples\voice_design.wav
```

If you want callback-style diagnostics without caring about the streamed samples, add:

```powershell
--simulate-stream-callback --dump-streaming-overlap
```

## Streaming Hint Track

The engine and wrapper now support a lightweight streaming hint track alongside
normal PCM chunk callbacks.

The Qwen-side hint track is intentionally limited to information the engine can
observe directly during synthesis:

- exact streamed sample ranges
- codec-frame provenance for emitted chunks
- model/runtime conditioning metadata
- cheap PCM-derived evidence such as RMS, peak energy, and zero-crossing rate

The engine does not attempt to emit:

- word ownership
- phoneme counts
- viseme ownership
- predicted word timestamps
- final lipsync timing

That linguistic layer belongs in the downstream consumer.

Wrapper-facing types live in:

- `apps/streaming_cli/include/offgrid_tts/Qwen3StreamingTts.h`

Main additions:

- `TtsStreamHintHeader`
- `TtsStreamHintChunk`
- `TtsHintEnergyClass`

`TtsStreamOptions::hint_header_callback` fires before the first streamed audio
chunk. Per-chunk hint metadata is attached to `TtsStreamChunk` via:

- `has_hint`
- `hint`

Current hint chunk fields:

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

`audio_sample_end` and `audio_end_sec` use exclusive-end semantics.

Experimental V2 note:

- `text_progress` is a soft monotonic whole-transcript model-path hint in `[0, 1]`
- it is not a forced alignment result
- it is derived from full text-token projection similarity against per-frame talker hidden states
- `text_progress_confidence` is intentionally conservative
- downstream consumers should blend it with occupancy evidence rather than treating it as authority

## Current Streaming Defaults

The default wrapper/engine settings now match the most reliable callback-oriented
profile we found for Offgrid-style consumers:

- `first_tail_window_frames=3`
- `ramp_tail_window_frames=6`
- `ramp_tail_window_count=0`
- `steady_tail_window_frames=8`
- `context_frames=3`
- `early_context_frames=2`
- `early_context_window_count=2`
- `adaptive_steady_windows=off`
- `adaptive_min_tail_window_frames=6`
- `adaptive_low_watermark_ms=220`
- `adaptive_high_watermark_ms=520`
- `paced_audio_delivery=on`
- `delivery_chunk_ms=40`
- `delivery_start_buffer_ms=40`
- `delivery_target_lead_ms=300`
- `steady_split_decode_frames=4`

For Offgrid-style callback consumers that maintain their own playback buffer, `--tts-profile offgrid-callback` now resolves to these same defaults explicitly.

## VoiceDesign Notes

- VoiceDesign models require instruction text.
- Speaker embeddings are rejected for VoiceDesign models.
- The wrapper auto-detects VoiceDesign model metadata and enforces the correct input rules.
- Lowering temperature too aggressively can destabilize VoiceDesign output quality and termination behavior.

For repeated VoiceDesign use with a fixed persona, the engine now supports:

- instruction-token caching
- one-time voice-profile warmup

These help hot-start latency for repeated requests that reuse the same stable profile key. They do not materially change steady vocoder window cadence by themselves.

Important:

- reuse a stable `instruction_cache_key` only when you intentionally want the same instruction text reused
- if your app appends per-line emotional or delivery modifiers, keep instruction-token caching keyed to the exact instruction text and use the stable profile key only for `warm_voice_profile`

Example:

```powershell
build-vs2022-x64\apps\streaming_cli\Release\qwen3_streaming_cli.exe `
  -m models `
  --voice-design `
  --model-name qwen3-tts-1.7b-voicedesign-f16 `
  --cache-instruction-tokens `
  --instruction-cache-key npc_lana_profile `
  --warm-voice-profile `
  --warm-voice-profile-key npc_lana_profile `
  --warmup-text "Hello." `
  --voice-design-instruct "A 30 year old woman with a rich feminine voice." `
  -t "I was not expecting visitors this late." `
  -o examples\voice_design_cached.wav
```

## Performance Notes

- Streaming prewarm is enabled by default and is excluded from the reported hot-path synthesis timing.
- First useful PCM can arrive much earlier than fully safe playback start.
- The standalone live player can hide burstiness that an external streaming client cannot.
- For integration work, callback-mode cadence is the more useful benchmark than WAV completion time.

Recent callback-mode comparison on `qwen3-tts-1.7b-voicedesign-f16`:

- new defaults:
  - first paced chunk: about `314 ms`
  - first playback submit: about `702 ms`
  - second window gap: about `387 ms`
- older control settings:
  - first paced chunk: about `312 ms`
  - first playback submit: about `867 ms`
  - second window gap: about `555 ms`

So the current defaults preserve early first audio while improving downstream playback readiness and early cadence.

Recent same-process VoiceDesign cache/warmup test on the branch that introduced persona reuse:

- baseline hot run:
  - first PCM ready: about `290 ms`
  - first playback submit: about `659 ms`
  - second window gap: about `369 ms`
- cached + warmed hot run:
  - first PCM ready: about `278 ms`
  - first playback submit: about `692 ms`
  - second window gap: about `414 ms`

Interpretation:

- retained: instruction-token caching and one-time voice-profile warmup
- not retained as a cadence fix: expectation that these would smooth steady decode delivery

They are useful for repeated fixed-profile VoiceDesign requests, but vocoder/decode cadence still dominates burstiness.

## More Detail

- Architecture overview: [docs/architecture.md](C:/git/qwen3-tts-cpp-streaming/docs/architecture.md)
- Current baseline notes: [docs/baseline_results.md](C:/git/qwen3-tts-cpp-streaming/docs/baseline_results.md)
