@echo off
set "ROOT=%~dp0.."
pushd "%ROOT%"
cmake -S . -B build-vs2022-x64 -G "Visual Studio 17 2022" -A x64 -DQWEN3_TTS_COREML=OFF -DQWEN3_TTS_EMBED_GGML=ON -DQWEN3_TTS_CUDA=ON -DGGML_CUDA=ON -DGGML_CUDA_GRAPHS=ON
cmake --build build-vs2022-x64 --config Release --target qwen3_streaming_cli
popd
