# Baseline Results

## Current Streaming Defaults

Current balanced standalone defaults:

- `first_tail_window_frames=3`
- `ramp_tail_window_frames=6`
- `ramp_tail_window_count=0`
- `steady_tail_window_frames=8`
- `context_frames=3`
- `early_context_frames=2`
- `early_context_window_count=2`
- `final_context_frames=4`
- `adaptive_steady_windows=off`
- `adaptive_min_tail_window_frames=6`
- `adaptive_low_watermark_ms=220`
- `adaptive_high_watermark_ms=520`
- `paced_audio_delivery=on`
- `delivery_chunk_ms=40`
- `delivery_start_buffer_ms=40`
- `delivery_target_lead_ms=300`
- `steady_split_decode_frames=4`
- `async_streaming_decode=on`
- `prewarm_streaming=on`

These defaults remain the best fit for the built-in standalone live player.

## Offgrid Callback Profile

For callback-driven consumers that maintain their own playback queue, use `--tts-profile offgrid-callback`:

- `first_tail_window_frames=3`
- `ramp_tail_window_frames=6`
- `ramp_tail_window_count=0`
- `steady_tail_window_frames=8`
- `context_frames=3`
- `early_context_frames=2`
- `early_context_window_count=2`
- `final_context_frames=4`
- `adaptive_steady_windows=off`
- `paced_audio_delivery=on`
- `delivery_chunk_ms=40`
- `delivery_start_buffer_ms=40`
- `delivery_target_lead_ms=300`
- `steady_split_decode_frames=4`

## What Changed

Earlier callback delivery could dump multiple chunks at the same timestamp and then leave longer gaps. The retained qwen-side pacing fix removes that zero-gap burst pattern.

The retained callback-specific profile keeps:

- the same `3`-frame first window
- no ramp windows after the first tail
- reduced early left-context
- a paced emitter with a larger lead target
- steady-window decode splitting for smaller callback arrivals

The qwen-side paced callback path also no longer uses the old zero-gap catch-up behavior that was especially unfriendly to downstream consumers.

## Verified Comparison

Representative callback-mode VoiceDesign run on `qwen3-tts-1.7b-voicedesign-f16`:

### Balanced standalone defaults

- first paced chunk: `314 ms`
- first playback submit: `702 ms`
- second window gap: `387 ms`
- max window gap: `532 ms`
- throughput: `1.03x realtime`

### Callback-oriented profile

- first paced chunk: `292 ms`
- first playback submit: `609 ms`
- second window gap: `317 ms`
- max window gap: `382 ms`
- queued audio at playback start: `536.9 ms`
- note: this profile is intended for external buffered consumers, not the standalone player

## Interpretation

The retained improvements are:

- startup remains essentially unchanged for first useful PCM
- callback consumers receive smaller, less bursty arrivals
- the first major inter-window gap is smaller
- the callback profile should be paired with a real client-side playback buffer

The tradeoff is that the callback-oriented profile is not suitable for qwen's own standalone live player. It shifts work toward steadier external delivery rather than maximizing standalone playback smoothness.

## Practical Guidance

- For interactive runtime speech, judge success from:
  - first usable chunk time
  - first safe playback time
  - second-window gap
  - max window gap
- The standalone player can hide burstiness that a game/runtime client cannot.
- If you are integrating this engine into another app, test in callback mode rather than relying only on WAV or direct-player behavior.

## VoiceDesign Persona Reuse

Separate from streaming-window tuning, the engine now supports two retained VoiceDesign reuse features:

- cached instruction tokens keyed by a stable profile key
- one-time voice-profile warmup keyed by that same profile key

Representative same-process test results on `qwen3-tts-1.7b-voicedesign-f16`:

- baseline hot run:
  - first PCM ready: `290 ms`
  - first playback submit: `659 ms`
  - second window gap: `369 ms`
  - max window gap: `560 ms`
- cached+warmed hot run:
  - first PCM ready: `278 ms`
  - first playback submit: `692 ms`
  - second window gap: `414 ms`
  - max window gap: `507 ms`

Interpretation:

- keep the features for repeated fixed-profile VoiceDesign conversations
- do not treat them as a burstiness fix
- steady-state delivery is still dominated by vocoder/decode timing, not instruction tokenization
- if the app varies instruction text per line, use stable profile warmup separately from exact-instruction token caching
