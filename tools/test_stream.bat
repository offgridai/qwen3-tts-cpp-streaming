@echo off
set "ROOT=%~dp0.."
pushd "%ROOT%"
call copy build-vs2022-x64\bin\Release\ggml*.* build-vs2022-x64\apps\streaming_cli\Release /y
build-vs2022-x64\apps\streaming_cli\Release\qwen3_streaming_cli.exe -m models --model-identifier qwen3-tts-0.6b-f16 --speaker-embedding reference\alfie_0.6b_f16.json --live-preroll-ms 250 --first-tail-window-frames 3 --ramp-tail-window-frames 5 --ramp-tail-window-count 2 --steady-tail-window-frames 8 --context-frames 3 --early-context-frames 2 --early-context-window-count 2 --final-context-frames 4 --adaptive-steady-windows --adaptive-min-tail-window-frames 6 --adaptive-low-watermark-ms 220 --adaptive-high-watermark-ms 520 --delivery-chunk-ms 80 --delivery-start-buffer-ms 80 --delivery-target-lead-ms 240 -t "Hello. Welcome to Alfie's Bodega. How can I help you today?" -o examples\streaming_test.wav
popd
