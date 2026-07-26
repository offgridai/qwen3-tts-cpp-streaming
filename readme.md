# qwen3-tts-cpp-streaming

Native Qwen3-TTS inference in C++17 with GGML and optional CUDA acceleration. The project supports batch synthesis, incremental PCM streaming, voice cloning, CustomVoice, VoiceDesign, and a reusable C++ integration library.

## Getting started

### 1. Install prerequisites

- CMake 3.20 or newer
- A C++17 compiler
- Visual Studio 2022 with the Desktop C++ workload on Windows
- Ninja and the CUDA Toolkit for the recommended Windows GPU build
- Python 3 for model preparation and benchmark utilities

CUDA 11.8 or newer is required for RTX 4090 builds. CUDA 12.8 or newer is
recommended for distributable builds that also contain native RTX 5090 kernels.

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
builds native kernels for both RTX 4090-class Ada GPUs (`sm_89`) and RTX
5090-class Blackwell GPUs (`sm_120a`), plus Ada PTX. With CUDA 11.8 through
12.7 it builds the Ada target only.

Use a native-only build to reduce compilation time while iterating on one
machine:

```powershell
cmake -S . -B build-ninja-native -G Ninja `
  -DQWEN3_TTS_CUDA=ON `
  -DQWEN3_TTS_CUDA_ARCHITECTURES=native
```

An explicit CMake architecture list is also accepted. Release artifacts meant
for both GPU generations should be built with CUDA 12.8 or newer and
`portable`; do not redistribute a `native` build from an RTX 5090 as an RTX
4090-compatible binary. The RTX 4090 target is compile-validated, but current
performance measurements were collected on an RTX 5090; a physical RTX 4090
acceptance run remains outstanding.

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

## Streaming profiles

| Profile | Model | Decode policy |
|---|---|---|
| `realtime` | 0.6B F16 | 3-frame first window; raw decode callbacks |
| `memory-saver` | 0.6B Q5_K | Lower-memory quantized model |
| `ultra-low` | 0.6B Q4_K | Smallest supported quantized model |
| `offgrid-callback` | Caller-selected | 5-frame first window, two 5-frame ramps, adaptive 7-8-frame steady windows, 240 ms callback chunks, 520 ms target lead |

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

Listening-selected defaults are temperature 0.75, top-k 16, residual top-p 0.9, repetition penalty 1.02, and semantic CB0 top-p 1.0. A text-relative safety ceiling prevents runaway generation when EOS is not sampled. Use `--seed` for deterministic output.

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

## Runtime controls

- `QWEN3_TTS_LOW_MEM=1`: lazily load and unload large components.
- `QWEN3_TTS_PRIME_RUNTIME=full`: prime the steady vocoder shape; `0` disables priming.
- `QWEN3_TTS_GGML_DEBUG=1`: enable GGML debug logging.
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
- [Historical baseline](docs/performance-baseline-0.6b.md)

Use each CLI's `--help` output as the authoritative option reference.
