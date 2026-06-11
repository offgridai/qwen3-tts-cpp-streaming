# Baseline Results

## Current Streaming Defaults

Current low-latency / less-bursty defaults:

- `first_tail_window_frames=3`
- `ramp_tail_window_frames=5`
- `ramp_tail_window_count=2`
- `steady_tail_window_frames=8`
- `context_frames=3`
- `early_context_frames=2`
- `early_context_window_count=2`
- `final_context_frames=4`
- `adaptive_steady_windows=on`
- `adaptive_min_tail_window_frames=6`
- `adaptive_low_watermark_ms=220`
- `adaptive_high_watermark_ms=520`
- `paced_audio_delivery=on`
- `delivery_chunk_ms=80`
- `delivery_start_buffer_ms=80`
- `delivery_target_lead_ms=240`
- `async_streaming_decode=on`
- `prewarm_streaming=on`

These defaults are tuned for callback-driven consumers such as Offgrid-style clients, where earlier usable audio and steadier delivery matter more than peak batch efficiency.

## What Changed

Earlier streaming settings optimized well for the standalone player, but still produced larger decode bursts for external consumers.

The current defaults keep:

- the same `3`-frame first window
- smaller early ramp windows
- reduced early left-context
- adaptive steady-state windows
- a paced emitter with a non-zero lead target

The qwen-side paced callback path also no longer waits on an unnecessarily large startup buffer when it is feeding an external callback instead of qwen's own paced live player.

## Verified Comparison

Representative callback-mode VoiceDesign run on `qwen3-tts-1.7b-voicedesign-f16`:

### New defaults

- first paced chunk: `314 ms`
- first playback submit: `702 ms`
- second window gap: `387 ms`
- max window gap: `532 ms`
- throughput: `1.03x realtime`

### Old-style control

- first paced chunk: `312 ms`
- first playback submit: `867 ms`
- second window gap: `555 ms`
- max window gap: `555 ms`
- throughput: `1.07x realtime`

## Interpretation

The retained improvements are:

- startup remains essentially unchanged for first useful PCM
- downstream consumers can begin playback materially sooner
- the first major inter-window gap is smaller
- paced delivery becomes friendlier to clients that manage their own playback buffer

The tradeoff is a small reduction in peak throughput, but the tested path remained faster than realtime.

## Practical Guidance

- For interactive runtime speech, judge success from:
  - first usable chunk time
  - first safe playback time
  - second-window gap
  - max window gap
- The standalone player can hide burstiness that a game/runtime client cannot.
- If you are integrating this engine into another app, test in callback mode rather than relying only on WAV or direct-player behavior.
