# qwen3-tts-cpp-streaming

C++ streaming voice clone implementation for Qwen3 TTS
Credit to predict-woo/qwen3-tts.cpp and Danmoreng/qwen3-tts.cpp for their earlier work on C++ qwen3-tts

Goals:
- ≤250ms first audio
- Sub-realtime ongoing stream

Status:
- Local Qwen3-TTS C++ voice-clone streaming
- Using: Qwen3-TTS-12Hz-1.7B-Base
- First PCM observed: ~175–280ms on RTX 5090
- Streaming throughput: ~1.2–1.3x realtime
- Output: mono 24kHz PCM16

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