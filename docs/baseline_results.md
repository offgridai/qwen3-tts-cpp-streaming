# Baseline Results

Known-good streaming configuration:

- top-k: 75
- temperature: 0.9
- top-p: 1.0
- first_tail_window_frames: 1
- steady_tail_window_frames: 12
- context_frames: 4
- final_context_frames: 4
- async streaming decode: enabled
- streaming prewarm: enabled
- live playback worker: enabled

Observed:
- first window queued: ~39ms after immediate scheduling fix
- first PCM ready: ~175ms in best run
- output format: mono 24kHz PCM16