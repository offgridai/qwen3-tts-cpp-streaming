# Bridge Adapter

This is a deliberate intermediate migration layer.

The top-level `qwen3_streaming_cli` executable invokes the known-good vendored engine at:

```text
third_party/qwen3-tts-cpp/build/qwen3-tts-cli.exe
```

It forwards the stable low-latency defaults:

- streaming generate enabled
- async streaming decode enabled
- live playback enabled
- prewarm enabled
- first tail window = 1 frame
- steady tail window = 12 frames
- context = 4 frames
- final context = 4 frames
- temperature = 0.9
- top-k = 75
- top-p = 1.0

This is not the final library boundary. It lets the new repo run the proven implementation while the internal streaming API is extracted from the vendored engine in later commits.
