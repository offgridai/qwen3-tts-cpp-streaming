@echo off

build-ninja-cuda\apps\streaming_cli\qwen3_streaming_cli.exe ^
  -m models ^
  --voice-design ^
  --model-name qwen3-tts-1.7b-voicedesign-f16 ^
  --temperature 0.9 ^
  --top-k 75 ^
  --repetition-penalty 1.02 ^
  --first-tail-window-frames 3 ^
  --ramp-tail-window-frames 6 ^
  --ramp-tail-window-count 0 ^
  --steady-tail-window-frames 10 ^
  --context-frames 3 ^
  --early-context-frames 2 ^
  --early-context-window-count 2 ^
  --final-context-frames 4 ^
  --steady-split-decode-frames 5 ^
  --delivery-target-lead-ms 450 ^
  --linecoach-proxy-playback ^
  --callback-preroll-ms 350 ^
  --callback-buffer-floor-ms 250 ^
  --callback-coalesce-ms 40 ^
  --callback-max-burst-ms 500 ^
  --voice-design-instruct "A 30 year old woman with a rich feminine voice." ^
  -t "I was not expecting visitors this late. What a pleasure it is to meet you here." ^
  -o examples\voice_design_quality_stable.wav