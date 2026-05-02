# qwen3-tts-cpp-streaming

C++ streaming voice clone implementation for Qwen3 TTS

Credit to predict-woo/qwen3-tts.cpp and Danmoreng/qwen3-tts.cpp for their earlier work on C++ qwen3-tts

## Overview

### Goals:
- ≤250ms first audio
- Sub-realtime ongoing stream

### Status:
- Local Qwen3-TTS C++ voice-clone streaming
- Using: Qwen3-TTS-12Hz-1.7B-Base
- First PCM observed: ~175ms on RTX 5090
- Streaming throughput: ~1.2–1.3x realtime
- Output: mono 24kHz PCM16

### Current Experiments

- **Tail-context decoding (no prefix re-decode)**. Eliminated O(N²) behavior by decoding only new frames with minimal left context.
- **Immediate first-frame scheduling** Queued the first decode directly from the first-frame callback (~40 ms), removing idle gaps.
- **One-frame first window** Allowed earliest possible audio emission, cutting TTFA significantly.
- **Fixed-size steady windows** Stable 12–16 frame decode batches for predictable performance and throughput.
- **Async decode pipeline** Decoupled generation from vocoder decode to keep GPU fully utilized.
- **Playback worker isolation** Moved audio output to a separate thread to eliminate blocking from synthesis timing.
- **Streaming prewarm** Warmed model + decoder path ahead of real input to avoid first-run latency spikes.
- **Final tail flush with extended context** Prevented cutoff artifacts without affecting streaming latency.
- **Minimal buffering / immediate playback** Started playback on first PCM chunk instead of waiting for larger buffers.
- **Tuned sampling defaults (top-k, etc.)** Balanced quality vs. stability without increasing latency.
- **Removed Python / IPC overhead** Native C++ execution avoided serialization and process latency.
- **Strictly bounded decode shapes** Kept predictable frame sizes (1 / 13 / 16) enabling consistent performance.
- **Separated latency measurement stages** Identified real bottlenecks (generation vs decode vs playback) to guide fixes.

## Quick Start Guide
Python environment for scripts & training:
py -3.10 -m venv .venv
.venv\Scripts\activate
python -m pip install --upgrade pip
pip install torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cu121
python -c "import torch; print(torch.cuda.is_available(), torch.version.cuda)"
pip install huggingface_hub gguf safetensors tqdm

Acquire Qwen3-tts model:
python scripts\setup_1.7b_model.py

Build underlying qwen3-tts-cpp:
cd third_party\qwen3-tts-cpp
powershell -ExecutionPolicy Bypass -File .\build.ps1 ^
  -UseNinja ^
  -EnableCuda ^
  -EnableCudaGraphs ^
  -Configuration Release

Build qwen3-tts-cpp-streaming:
(top of repo)
cmake -S . -B build
cmake --build build --config Release

Record speaker reference wav:
place in reference\ref.wav
(if necessary) ffmpeg -i ref.wav -ac 1 -ar 24000 -sample_fmt s16 ref.wav

Train the clone:
third_party\qwen3-tts-cpp\build\qwen3-tts-cli.exe ^
  -m models ^
  -r reference\ref.wav ^
  --dump-speaker-embedding reference\ref_speaker.json ^
  -t "This is a test string."

Stream command:
build\Release\qwen3_streaming_cli.exe ^
  -m models ^
  --speaker-embedding reference\ref_speaker.json ^
  -t "Hello. Welcome to Alfie's Bodega. How can I help you today?" ^
  --instruct "happy" ^
  -o examples\alfie.wav