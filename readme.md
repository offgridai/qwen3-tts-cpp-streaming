# qwen3-tts-cpp-streaming

Native Qwen3-TTS inference in C++17 with GGML and optional CUDA acceleration. The project supports batch synthesis, incremental PCM streaming, voice cloning, CustomVoice, VoiceDesign, and a reusable C++ integration library.

## Getting started

### 1. Install prerequisites

- CMake 3.20 or newer
- A C++17 compiler
- Visual Studio 2022 with the Desktop C++ workload on Windows
- Ninja and the CUDA Toolkit for the recommended Windows GPU build
- Python 3 for model preparation and benchmark utilities

CUDA 11.8 or newer is required for the default portable build. CUDA 12.8 or
newer is recommended so it also contains native RTX 5090 kernels.

Run the following commands from a Visual Studio Developer PowerShell in the repository root.

### 2. Prepare models

Place converted GGUF files in `models/`, or prepare the standard model set with:

```powershell
py -3 tools\setup_pipeline_models.py --models-dir models --coreml off
```

The minimum 0.6B F16 streaming set is:

```text
models/qwen3-tts-0.6b-f16.gguf
models/qwen3-tts-tokenizer-f16.gguf
```

### 3. Build

```powershell
cmake -S . -B build-ninja-cuda -G Ninja `
  -DQWEN3_TTS_COREML=OFF `
  -DQWEN3_TTS_EMBED_GGML=ON `
  -DQWEN3_TTS_CUDA=ON `
  -DQWEN3_TTS_CUDA_ARCHITECTURES=portable `
  -DGGML_CUDA=ON `
  -DGGML_CUDA_GRAPHS=ON

cmake --build build-ninja-cuda --target `
  tts_engine_cli qwen3_tts_streaming qwen3_streaming_cli
```

For a Visual Studio generator build, use `tools\build.bat`.

### 4. Synthesize streaming speech

This example uses the committed Priestley 0.6B voice, plays audio as it is generated, and writes the complete WAV:

```powershell
build-ninja-cuda\apps\streaming_cli\qwen3_streaming_cli.exe `
  -m models `
  --model-identifier qwen3-tts-0.6b-f16 `
  --speaker-embedding reference\priestley_0.6b_f16.json `
  --tts-profile offgrid-callback `
  --seed 42 `
  -t "Hello from Qwen3 TTS." `
  -o examples\getting_started.wav
```

Use `--no-play-streaming --simulate-stream-callback` for a silent callback-path run. On Windows, streaming CLI diagnostics go to stdout, so `2>&1` is unnecessary when piping to `Tee-Object`.

## Current RTX 5090 performance

These are local F16 measurements from 2026-07-27 with a stored Priestley clone,
seed 42, the `offgrid-callback` profile, and a simulated 350 ms playback buffer.
They are workload-specific rather than hardware-portable claims.

| Model | Cold model start | Resident first 350 ms | Streaming speed | Maximum production gap |
|---|---:|---:|---:|---:|
| 0.6B Base | 1.30-1.33 s | 283-302 ms | 2.61-2.72x realtime (RTF 0.368-0.383) | 239-262 ms |
| 1.7B Base | 1.93-1.98 s | 319-342 ms | 2.48-2.53x realtime (RTF 0.395-0.404) | 251-295 ms |

The current 28.8 s Priestley reference takes 348-355 ms to encode with the
already-loaded 1.7B model, making cold clone readiness about 2.28-2.33 s. A
longer 1.7B evaluation produced 2.78x realtime (RTF 0.360), showing the expected
amortization of startup work. Neither streaming case produced a simulated
playback underrun. See the linked performance records for exact methodology.

### Persistent batch synthesis

The streaming CLI can load the model once and synthesize the Cartesian product
of transcript files, voice-clone JSONs, and seeds. Transcript and voice lists
are tab-separated `<id><TAB><path>` files; IDs may contain letters, digits,
underscores, and hyphens. Relative paths are resolved from the list file.

```powershell
build-ninja-cuda\apps\streaming_cli\qwen3_streaming_cli.exe `
  -m models `
  --model-identifier qwen3-tts-0.6b-f16 `
  --batch-transcript-list batch\transcripts.tsv `
  --batch-voice-list batch\voices.tsv `
  --batch-seeds 41,42,43 `
  --batch-output-dir batch\wav `
  --no-play-streaming --simulate-stream-callback --quiet-all
```

Outputs are named `<transcript-id>__<voice-id>__seed_<seed>.wav`. A
`batch_results.tsv` index is written alongside them. The model remains loaded
for the entire batch, while each voice embedding is loaded once per voice.
This default path processes one request at a time and preserves normal
streaming behavior.

For offline throughput, opt into physical vocoder batching:

```powershell
build-ninja-cuda\apps\streaming_cli\qwen3_streaming_cli.exe `
  -m models `
  --model-identifier qwen3-tts-0.6b-f16 `
  --batch-job-list batch\jobs.tsv `
  --batch-output-dir batch\wav `
  --vocoder-batch-size 2 `
  --quiet-all
```

Autoregressive code generation remains sequential. Equal-frame-count results
are grouped automatically and decoded in one physical vocoder graph; unmatched
lengths fall back to smaller groups. The option defaults to `1`, so ordinary
streaming latency and output are unchanged. On an RTX 5090, a two-item,
45-frame decoder probes took 516-553 ms versus 684-687 ms serial (1.24-1.33x
vocoder throughput); short nine-frame probes took 175-182 ms versus 386-390 ms
(2.14-2.21x).
Batched floating-point scheduling is not bit-identical to serial decoding; the
45-frame probe measured waveform RMSE 0.0025 and maximum sample delta 0.122.

For exact per-utterance assignments, use `--batch-job-list <tsv>` instead of
the three Cartesian inputs. Each non-comment row has five tab-separated fields:

```text
job-id  transcript-path  voice-id  voice-json-path  seed
```

Paths may be relative to the job-list file. Each job produces `<job-id>.wav`;
ordering jobs by voice avoids reloading the same speaker embedding.

Batch controls are mutually exclusive: use either the three Cartesian list
options or `--batch-job-list`. `--vocoder-batch-size` affects only offline batch
jobs, requires equal codec-frame counts within a physical group, and does not
improve streaming time to first audio.

## Build options and outputs

`QWEN3_TTS_BUILD_STREAMING_WRAPPER=OFF` omits the integration library and its CLI. `QWEN3_TTS_BUILD_STREAMING_CLI=OFF` builds the library without the harness.

Primary Ninja outputs:

```text
build-ninja-cuda/engine/tts_engine_cli.exe
build-ninja-cuda/apps/streaming_cli/qwen3_tts_streaming.lib
build-ninja-cuda/apps/streaming_cli/qwen3_streaming_cli.exe
build-ninja-cuda/engine/tts_engine_quantize.exe
```

Generated `ggml*.dll` files must be beside the executable or available on `PATH`. The normal build places copies beside the CLIs.

### CUDA GPU compatibility

`portable` is the default CUDA architecture policy. With CUDA 12.8 or newer it
builds native kernels for GTX 16-series Turing (`sm_75`), RTX 4090-class Ada
(`sm_89`), and RTX 5090-class Blackwell (`sm_120a`) GPUs, plus Ada PTX. With
CUDA 11.8 through 12.7 it omits only the Blackwell target.

For a smaller GTX 16-series build, use the `turing` policy. It targets `sm_75`
and enables GGML's MMQ kernels because these cards do not have tensor cores:

```powershell
cmake -S . -B build-ninja-turing -G Ninja `
  -DQWEN3_TTS_COREML=OFF `
  -DQWEN3_TTS_EMBED_GGML=ON `
  -DQWEN3_TTS_CUDA=ON `
  -DQWEN3_TTS_CUDA_ARCHITECTURES=turing `
  -DGGML_CUDA=ON `
  -DGGML_CUDA_GRAPHS=ON
cmake --build build-ninja-turing --target qwen3_streaming_cli
```

GTX 1660 cards generally have 6 GiB VRAM. Prefer a quantized 1.7B model and
set `QWEN3_TTS_LOW_MEM=1` if normal loading exhausts VRAM. As a driver-specific
fallback only, rebuild with `-DGGML_CUDA_NO_VMM=ON` if allocation through CUDA
virtual memory fails.

Use a native-only build to reduce compilation time while iterating on one
machine:

```powershell
cmake -S . -B build-ninja-native -G Ninja `
  -DQWEN3_TTS_CUDA=ON `
  -DQWEN3_TTS_CUDA_ARCHITECTURES=native
```

An explicit CMake architecture list is also accepted. Release artifacts meant
for all supported GPU generations should be built with CUDA 12.8 or newer and
`portable`; do not redistribute a `native` build from an RTX 5090 as an RTX
4090- or GTX 1660-compatible binary. The Turing and RTX 4090 targets are
compile-validated, but current performance measurements were collected on an
RTX 5090; physical acceptance runs remain outstanding.

### Quantized 1.7B models

The bundled quantizer can derive Q4_K and Q5_K models from any converted F16
GGUF. After building `tts_engine_quantize`, generate both variants of the 1.7B
Base model with:

```powershell
py -3 tools\quantize_models.py models\qwen3-tts-1.7b-base-f16.gguf
```

The outputs are `models\qwen3-tts-1.7b-base-q4_k.gguf` and
`models\qwen3-tts-1.7b-base-q5_k.gguf`. Pass a CustomVoice or VoiceDesign F16
GGUF instead to create the corresponding variants. Q4_K uses less memory;
Q5_K retains more weight precision. The current Base conversion is about 1.59
GiB in Q4_K and 1.77 GiB in Q5_K. Model binaries remain ignored by Git, so this
command is the reproducible source of these local artifacts.

## Streaming integration library

The CMake target `qwen3_tts::streaming` exposes `offgrid_tts/Qwen3StreamingTts.h`:

```cpp
#include <offgrid_tts/Qwen3StreamingTts.h>
#include <stdexcept>

Qwen3StreamingTts tts;
if (!tts.load("models", "qwen3-tts-0.6b-f16")) {
    throw std::runtime_error(tts.last_error());
}
if (!tts.load_speaker_embedding("reference/priestley_0.6b_f16.json")) {
    throw std::runtime_error(tts.last_error());
}

TtsStreamOptions options;
options.output_wav.clear(); // Callback-only; do not write a WAV.
options.play_streaming = false;

bool ok = tts.synthesize_streaming(
    "Hello from the integration library.",
    options,
    [](const TtsStreamChunk& chunk) {
        consume_pcm(chunk.samples.data(), chunk.samples.size(), chunk.sample_rate); // Application-provided sink.
        return true; // Return false to cancel generation.
    });
```

The wrapper:

- loads the selected model immediately;
- is movable but not copyable;
- supports callback-only or callback-plus-WAV operation;
- exposes language IDs, named speakers, speaker embeddings, VoiceDesign instructions, sampling, and streaming controls;
- reports failures through `last_error()` and does not own application playback buffering.
- exposes deferred codec generation and equal-length physical vocoder decode
  for applications that explicitly prefer offline throughput over minimum latency.

## Streaming profiles

| Profile | Model | Decode policy |
|---|---|---|
| `realtime` | 0.6B F16 | 3-frame first window; raw decode callbacks |
| `memory-saver` | 0.6B Q5_K | Lower-memory quantized model |
| `ultra-low` | 0.6B Q4_K | Smallest supported quantized model |
| `offgrid-callback` | Caller-selected | 6-frame first window, two 6-frame ramps, fixed 10-frame steady windows, 4-frame context, 240 ms callback chunks, 520 ms target lead |

The quality-oriented default spends some of the resident vocoder's performance
headroom on larger, less frequent decode windows and twice the previous left
context. Five 1.7B tuning seeds reached the first 350 ms in 309-318 ms with no
simulated underruns. This is intended to reduce audible joins and synthetic
window-to-window variation; listening remains the decisive quality test.

The engine owns optional Windows `waveOut` playback. Applications consuming PCM callbacks should implement their own device integration and buffering policy.

## Engine CLI

Use the lower-level engine CLI for batch or direct engine testing:

```powershell
build-ninja-cuda\engine\tts_engine_cli.exe `
  -m models `
  --model-name qwen3-tts-0.6b-f16.gguf `
  --speaker-embedding reference\lana_0.6b_f16.json `
  -t "This line uses the stored voice." `
  -o examples\engine.wav
```

Incremental generation is the default. Add `--batch` for one full vocoder decode.

## Voices and VoiceDesign

The `reference/` directory contains 0.6B and 1.7B F16 embeddings for Lana and Priestley, together with their source WAV files. Embeddings are model-family-specific.

Extract an embedding from a clean mono 24 kHz WAV:

```powershell
py -3 tools\wav_to_speaker_embedding.py `
  --input-wav reference\voice_ref.wav `
  --output-json reference\voice_0.6b_f16.json `
  --model-name qwen3-tts-0.6b-f16.gguf
```

VoiceDesign requires a VoiceDesign model and a non-empty instruction; it does not accept a speaker embedding:

```powershell
build-ninja-cuda\apps\streaming_cli\qwen3_streaming_cli.exe `
  -m models `
  --voice-design `
  --model-identifier qwen3-tts-1.7b-voicedesign-f16 `
  --voice-design-instruct "A calm, deep male narrator." `
  -t "I was not expecting visitors this late." `
  -o examples\voice_design.wav
```

## Sampling and reliability

Listening-selected defaults are temperature 0.75, top-k 16, residual top-p 0.9, repetition penalty 1.02, and semantic CB0 top-p 1.0. A text-relative safety ceiling prevents runaway generation when EOS is not sampled. Use `--seed` for repeatable sampling; the acceptance suite checks strict byte determinism.

Useful validation commands:

```powershell
py -3 tools\streaming_callback_benchmark.py `
  --input-json reference\lana_0.6b_f16.json `
  --seed 42 `
  --text "Use a reasonably long paragraph for this consistency test."

py -3 tools\streaming_regression_benchmark.py `
  --input-json reference\lana_0.6b_f16.json `
  --seeds 40,41,42,43,44,45
```

The regression command accepts natural EOS and the bounded text-relative safety limit. It fails on process errors, timeouts, or the absolute token limit.

## Tests

The default CTest suite is model-free and checks wrapper ownership, moves, initial state, and error reporting:

```powershell
cmake --build build-ninja-cuda --target qwen3_wrapper_contract_test
ctest --test-dir build-ninja-cuda -L fast --output-on-failure
```

The acceptance runner uses the real 0.6B model and stored voice. Quick mode checks wrapper streaming contracts, callback/WAV equivalence, cancellation, deterministic output, initialization and streaming budgets, PCM integrity, artifact scores, and one reliability case:

```powershell
cmake --build build-ninja-cuda --target `
  qwen3_wrapper_contract_test qwen3_wrapper_model_test qwen3_streaming_cli

py -3 tools\acceptance.py
```

Run `py -3 tools\acceptance.py --full` for the six-case, six-seed reliability corpus. Model-backed checks are not registered with CTest by default; configure with `-DQWEN3_TTS_ENABLE_MODEL_TESTS=ON` when that behavior is desirable for a dedicated machine.

Run the physical vocoder equivalence/performance probe directly with:

```powershell
build-ninja-cuda\tests\qwen3_decoder_batch_test.exe `
  models\qwen3-tts-tokenizer-f16.gguf 45
```

## Runtime controls

- `QWEN3_TTS_LOW_MEM=1`: lazily load and unload large components.
- `QWEN3_TTS_PRIME_RUNTIME=full`: prime the steady vocoder shape; `0` disables priming.
- `QWEN3_TTS_GGML_DEBUG=1`: enable GGML debug logging.
- `GGML_CUDA_GRAPH_STATS=1`: print opt-in CUDA graph capture and replay counters when each CUDA backend is released.
- `QWEN3_TTS_USE_COREML=1`: enable CoreML prediction on supported macOS builds.
- `QWEN3_TTS_PROFILE_DECODER=1`: print detailed vocoder profiling.
- `QWEN3_TTS_DEBUG_DUMP_DIR=<path>`: write transformer parity traces.

## Repository map

```text
engine/                 Core C++ library, engine CLI, quantizer
apps/streaming_cli/     Reusable streaming wrapper and CLI harness
models/                 Local GGUF models; ignored by Git
reference/              Reusable voices and source audio
tools/                  Model preparation and validation utilities
docs/                   Architecture and measured performance records
```

Further reading:

- [Architecture](docs/architecture.md)
- [Current 0.6B baseline](docs/performance-baseline-0.6b-current.md)
- [Optimization results](docs/optimization-results-0.6b.md)
- [Current 1.7B baseline and GPU-resident vocoder evaluation](docs/cuda-graph-evaluation.md)
- [Historical baseline](docs/performance-baseline-0.6b.md)

Use each CLI's `--help` output as the authoritative option reference.
