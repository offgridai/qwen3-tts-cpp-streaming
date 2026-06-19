@echo off
set "ROOT=%~dp0.."
pushd "%ROOT%"
build-ninja-cuda\apps\streaming_cli\qwen3_streaming_cli.exe ^
  -m models ^
  --model-name qwen3-tts-0.6b-f16 ^
  --speaker-embedding reference\lana_0.6b_f16.json ^
  --tts-profile offgrid-callback ^
  --linecoach-proxy-playback ^
  --callback-preroll-ms 350 ^
  --callback-buffer-floor-ms 250 ^
  --callback-coalesce-ms 40 ^
  --callback-max-burst-ms 500 ^
  -t "I was not expecting visitors this late. What a pleasure it is to meet you here. Did you know I offer sandwiches for sale? Tell me about your favorite childhood memory. Preferably of sandwiches. Thanks!" ^
  -o examples\lana_linecoach_proxy.wav
popd
