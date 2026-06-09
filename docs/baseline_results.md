# Baseline Results

## Streaming Configuration

Known-good streaming configuration:

- top-k: `75`
- temperature: `0.9`
- top-p: `1.0`
- first_tail_window_frames: `1`
- steady_tail_window_frames: `12`
- context_frames: `4`
- final_context_frames: `4`
- async streaming decode: enabled
- streaming prewarm: enabled
- live playback worker: enabled
- output format: mono `24 kHz` PCM16

## Latency Baseline

Observed low-latency startup behavior:

- first window queued: about `36-47 ms`
- first tail decode: about `126-136 ms`
- first PCM ready: about `169-175 ms`

This is the most important "feels responsive" metric for streaming playback.

## Timing Interpretation

The reported `Total` timing now excludes streaming prewarm.

Earlier measurements overstated end-to-end wall time because:

- `Code generation` excluded prewarm
- `Vocoder decode` excluded prewarm
- `Total` still included prewarm

That accounting bug is fixed. Realtime judgments should now be based on the corrected `Total` and `Throughput` lines.

## Recent Verified Results

### `qwen3-tts-0.6b-f16`

Representative streaming runs:

- first PCM ready: `169-173 ms`
- throughput: about `1.13x-1.22x realtime`
- `50 ms` live preroll starts playback on the first chunk
- `150 ms` live preroll typically delays first submit until the second chunk

### `qwen3-tts-1.7b-voicedesign-f16`

Representative streaming run:

- `n_instruct=21`
- first PCM ready: `173 ms`
- streaming decode total: `2411 ms`
- total hot-path time: `2729 ms`
- audio duration: `3.18 s`
- throughput: `1.16x realtime`

## Practical Guidance

- For low-latency interactive runtime speech, the `0.6B` path remains the best default.
- VoiceDesign is now viable for direct streaming use, but it is heavier and better suited to persona creation or lower-frequency use.
- A strong production pattern is:
  1. use VoiceDesign once to create a target voice
  2. switch to a faster clone/custom path for repeated lines
