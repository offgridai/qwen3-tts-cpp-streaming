# qwen3-tts-cpp-streaming

Native Qwen3-TTS inference in C++17 with GGML, CUDA acceleration, batch synthesis, incremental vocoder decode, callback-paced PCM, voice cloning, CustomVoice, and VoiceDesign support.

## Repository layout

```text
engine/                 Core library, C/C++ APIs, engine CLI, quantizer
apps/streaming_cli/     Callback-oriented integration harness
models/                 Local GGUF models (ignored by Git)
reference/              Reusable speaker embeddings and reference audio
tools/                  Model conversion and operational quality utilities
docs/architecture.md    Runtime architecture and streaming semantics
docs/performance-baseline-0.6b.md
                        Historical pre-optimization benchmark
docs/performance-baseline-0.6b-current.md
                        Accepted current-main benchmark and reproduction
docs/performance-baseline-0.6b-current.json
                        Machine-readable current-main scores
```

`engine/ggml/` is built as part of the workspace. The streaming CLI links directly to `tts_engine`; it does not launch the engine CLI as a subprocess.

## Requirements

- CMake 3.20+
- A C++17 compiler
- Visual Studio 2022 with the Desktop C++ workload on Windows
- Optional CUDA Toolkit for GPU inference
- Python only for model conversion and workflow tools

## Models

Place GGUF artifacts in `models/`. The common set is:

```text
qwen3-tts-0.6b-f16.gguf
qwen3-tts-0.6b-q5_k.gguf
qwen3-tts-0.6b-q4_k.gguf
qwen3-tts-1.7b-base-f16.gguf
qwen3-tts-1.7b-customvoice-f16.gguf
qwen3-tts-1.7b-voicedesign-f16.gguf
qwen3-tts-tokenizer-f16.gguf
```

`tools/setup_pipeline_models.py` prepares the standard model set. The conversion scripts remain available for manual workflows.

## Build

From a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build-ninja-cuda -G Ninja `
  -DQWEN3_TTS_COREML=OFF `
  -DQWEN3_TTS_EMBED_GGML=ON `
  -DQWEN3_TTS_CUDA=ON `
  -DGGML_CUDA=ON `
  -DGGML_CUDA_GRAPHS=ON

cmake --build build-ninja-cuda --target `
  tts_engine_cli qwen3_streaming_cli tts_engine_quantize
```

For a Visual Studio generator build, run `tools\build.bat`. The engine-only build helper is `engine\build.ps1`.

Primary Ninja outputs:

```text
build-ninja-cuda\engine\tts_engine_cli.exe
build-ninja-cuda\apps\streaming_cli\qwen3_streaming_cli.exe
build-ninja-cuda\engine\tts_engine_quantize.exe
```

On Windows, keep `build-ninja-cuda\bin` and `build-ninja-cuda\engine` on `PATH`, or place the generated `ggml*.dll` files beside the executable.

## Engine CLI

Base synthesis:

```powershell
build-ninja-cuda\engine\tts_engine_cli.exe `
  -m models `
  --model-name qwen3-tts-0.6b-f16.gguf `
  -t "Hello from the native engine." `
  -o examples\engine.wav
```

Voice cloning from a reusable embedding:

```powershell
build-ninja-cuda\engine\tts_engine_cli.exe `
  -m models `
  --model-name qwen3-tts-0.6b-f16.gguf `
  --speaker-embedding reference\lana_0.6b_f16.json `
  -t "This line uses the stored voice." `
  -o examples\clone.wav
```

Committed 0.6B clone assets include `lana_0.6b_f16.json`/`lana_ref.wav` and `priestley_0.6b_f16.json`/`priestley_ref.wav` under `reference/`.

Extract a new embedding from a clean mono 24 kHz WAV:

```powershell
py -3 tools\wav_to_speaker_embedding.py `
  --input-wav reference\voice_ref.wav `
  --output-json reference\voice_0.6b_f16.json `
  --model-name qwen3-tts-0.6b-f16.gguf
```

Add `--batch` for a single full vocoder decode. Streaming generation is the default. Listening-selected sampling defaults are temperature 0.75, top-k 16, residual top-p 0.9, repetition penalty 1.02, and semantic CB0 top-p 1.0. Sampling supports deterministic `--seed` and explicit overrides. A text-relative safety ceiling prevents short prompts from running to the absolute token limit; tune it with `--max-frames-per-text-token` and `--min-dynamic-tokens`.

## Streaming CLI

```powershell
build-ninja-cuda\apps\streaming_cli\qwen3_streaming_cli.exe `
  -m models `
  --model-name qwen3-tts-0.6b-f16 `
  --speaker-embedding reference\lana_0.6b_f16.json `
  --tts-profile offgrid-callback `
  --no-play-streaming `
  --simulate-stream-callback `
  -t "Hello from the callback path." `
  -o examples\stream.wav
```

Profiles:

| Profile | Model selection | First window | PCM delivery |
|---|---|---:|---|
| `realtime` | 0.6B F16 | 3 frames | raw decode windows |
| `memory-saver` | 0.6B Q5_K | 3 frames | raw decode windows |
| `ultra-low` | 0.6B Q4_K | 3 frames | raw decode windows |
| `offgrid-callback` | caller-selected | 5 frames | two 5-frame ramp windows, then adaptive 7-8-frame windows with 2-frame context; 350 ms callback preroll, 520 ms steady lead |

The default streaming policy uses 3/6/8-frame first/ramp/steady windows, 2 frames of left context, 1 early-context frame for the first two windows, 3 final-context frames, asynchronous decode, adaptive windows off, and paced delivery off.

On Windows, `qwen3_streaming_cli.exe` writes diagnostics to stdout. Pipe it directly to `Tee-Object`; `2>&1` is unnecessary.

## VoiceDesign and reusable voices

VoiceDesign requires a VoiceDesign model and non-empty instruction. It rejects speaker embeddings.

```powershell
build-ninja-cuda\apps\streaming_cli\qwen3_streaming_cli.exe `
  -m models `
  --voice-design `
  --model-name qwen3-tts-1.7b-voicedesign-f16 `
  --voice-design-instruct "A calm, deep male narrator." `
  -t "I was not expecting visitors this late." `
  -o examples\voice_design.wav
```

For a fixed reusable persona:

1. Generate clean reference audio with `tools/voicedesign_to_wav.py`.
2. Extract a model-family-specific embedding with `tools/wav_to_speaker_embedding.py`.
3. Validate it with `tools/speaker_embedding_smoke_test.py`.

Instruction token caching is available through `--cache-instruction-tokens` and `--instruction-cache-key`. A cache key must represent the exact instruction text; reusing a key with different text intentionally reuses the earlier tokens.

## Streaming hint track

The C++ API and wrapper can emit a header followed by per-audio-chunk hints. Hints contain exact sample ranges, contributing codec-frame ranges, RMS/peak/zero-crossing evidence, activity spans, and an experimental monotonic text-progress estimate.

The hint track is not word, phoneme, viseme, or forced alignment. Sample end positions are exclusive. Downstream timing systems should treat activity and text progress as soft evidence.

See [docs/architecture.md](docs/architecture.md) for the execution flow and concurrency model.

See [docs/performance-baseline-0.6b.md](docs/performance-baseline-0.6b.md) for the initial 0.6B CUDA performance baseline, fidelity limitations, and optimization roadmap.

See [docs/performance-baseline-0.6b-current.md](docs/performance-baseline-0.6b-current.md) for the authoritative current-main performance, fidelity, cadence, and reproduction baseline.

See [docs/optimization-results-0.6b.md](docs/optimization-results-0.6b.md) for measured results from the first optimization pass.

## Retained tools

| Tool | Purpose |
|---|---|
| `setup_pipeline_models.py` | Prepare the standard GGUF model set |
| `convert_tts_to_gguf.py` | Convert a Qwen3-TTS model |
| `convert_tokenizer_to_gguf.py` | Convert tokenizer/vocoder assets |
| `convert_code_predictor_to_coreml.py` | Export the optional CoreML predictor |
| `voicedesign_to_wav.py` | Produce segmented or candidate-selected reference audio |
| `wav_to_speaker_embedding.py` | Extract a reusable speaker embedding |
| `speaker_embedding_smoke_test.py` | Validate an embedding through synthesis |
| `streaming_quality_ab.py` | Compare batch and streaming output |
| `streaming_callback_benchmark.py` | Measure callback startup and cadence |
| `streaming_regression_benchmark.py` | Run a deterministic reliability/performance corpus |
| `detect_synthetic_spans.py` | Heuristic artifact scoring for WAV files |

Generated WAVs, reports, caches, models, and temporary run directories are ignored by Git.

## Runtime controls

- `QWEN3_TTS_LOW_MEM=1`: lazily load and unload large components to reduce residency.
- `QWEN3_TTS_PRIME_RUNTIME=full`: also prime the steady vocoder shape; `0` disables all runtime priming.
- `QWEN3_TTS_GGML_DEBUG=1`: include GGML debug logging.
- `QWEN3_TTS_USE_COREML=1`: enable the CoreML code predictor on supported macOS builds.

Use each CLI's `--help` output as the authoritative option reference.
