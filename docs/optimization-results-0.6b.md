# Qwen3-TTS 0.6B optimization results

These results explain the experiments that moved the original 2026-07-24 RTX 5090 baseline to the accepted state in `performance-baseline-0.6b-current.md`. The original baseline remains in `performance-baseline-0.6b.md`.

## Accepted results

| Dimension | Baseline | Optimized | Change |
|---|---:|---:|---:|
| Full 20.3 s reference embedding extraction | 13.78 s | 249 ms mean (241-260) | 55x faster |
| Cold model/runtime initialization | 1.69 s | 1.286 s mean (1.280-1.294) | 24% faster |
| Estimated cold clone readiness | 15.44 s | 1.54 s | 90% faster |
| Estimated cold first 350 ms | 2.06 s | 1.67 s | 19% faster |
| Resident first 350 ms | 372 ms | 381 ms mean (374-384) | 2% slower |
| Maximum decode-window gap | 483 ms mean | 443 ms mean (429-453) | 8% lower |
| Sustained speed | 1.36x real time | 1.30x mean | 4% slower |

The clone speedup replaces the speaker frontend's per-frame O(N²) DFT with a radix-2 FFT. Compared with the prior native extraction, the optimized embedding had cosine similarity 0.999999996, maximum absolute difference 0.000089, and RMSE 0.000028.

Initialization now primes the transformer and first vocoder path but skips the 392 ms steady-window vocoder pass. Set `QWEN3_TTS_PRIME_RUNTIME=full` to restore full eager priming or `0` to disable priming. The default trades approximately 9 ms of resident first-buffer latency for a substantially faster cold path.

The final `offgrid-callback` profile uses a 5-frame first window, two 5-frame ramp windows, 2-frame context, and adaptive 7-8-frame steady windows. The benchmark models a 350 ms downstream buffer and permits 520 ms of lead afterward. Short and long seeded probes reported zero simulated underruns, 21-54 ms minimum headroom, and 1.21-1.25x real-time throughput.

## Rejected low-preroll cadence pass

An experimental policy started with four frames, used no context for one bootstrap join, then switched to one-frame context and adaptive four-frame minimum windows. It improved measured cadence, but listening found audible stutters and a slightly more synthetic voice. The automated artifact score did not detect that regression, so the policy was rejected.

| Metric | Restored profile | Rejected 4-frame profile |
|---|---:|---:|
| First PCM block ready | 374-384 ms | 323-350 ms; 329-330 ms typical |
| Playback start with 350 ms default preroll | 374-384 ms | 569-616 ms; about 570 ms typical |
| Maximum paced delivery gap | about 443 ms | 316-322 ms |
| Simulated playback underruns | not measured | 0 in 4 serial runs |
| Minimum simulated headroom after start | not measured | 31-56 ms |
| Sustained speed | 1.30x | 1.12-1.14x |
| Worst local artifact score | 1.714 | 1.492 |
| Output duration versus batch | equal | 23 ms shorter once per request |

The rejected profile demonstrated that callback-buffer continuity is not sufficient to establish perceptual smoothness: its queue never starved, but frequent vocoder joins remained audible. Human listening therefore remains a required acceptance gate for window-policy changes.

## Reliability and fidelity

The evaluation path now supports explicit seeds, reports EOS versus safety/token-limit termination, and separates the semantic CB0 RNG from residual-codebook sampling. Repeated seed 42 synthesis produced byte-identical WAV files.

On the six-seed passage that reproduced the baseline defect:

| Metric | Seeded pre-change | Optimized |
|---|---:|---:|
| Natural EOS | 2/6 | 5/6 |
| Unbounded/token-limit completion | 4/6 | 0/6 |
| Text-relative safety completion | not available | 1/6 at 13.98 s |
| Mean worst-half-second artifact score | 0.825 | 0.796 |
| Mean worst-region artifact score | 1.680 | 1.618 |
| Mean throughput | 1.378x | 1.387x before adaptive cadence policy |

CB0 defaults to top-p 1.0 because tests showed that values from 0.90 through 0.99 could prevent EOS for some seeds. `--top-p` controls residual acoustic codebooks; `--cb0-top-p` is an explicit experimental override.

Human A/B listening selected temperature 0.75, top-k 16, residual top-p 0.9, and repetition penalty 1.02 over the former 0.9/75/1.0/1.05 defaults. The former settings produced a noticeably more synthetic result for the same seed and clone embedding.

A dynamic safety ceiling defaults to five audio frames per content token, with a minimum of 64 frames and the caller's `max_audio_tokens` as the absolute upper bound. It prevents catastrophic 20-327 second continuations but is not equivalent to natural EOS. In a one-seed six-case corpus, four cases reached EOS and two used the safety ceiling. Termination therefore remains the principal model-quality limitation.

The spectral scores are heuristic. ASR WER, speaker-similarity evaluation across multiple voices, and human listening scores remain outstanding.

## Rejected experiments

- Preserving EOS outside top-p did not change failing seeded paths.
- Relaxing top-p only after a long-generation threshold did not recover divergent paths.
- Synchronous streaming decode left startup unchanged and reduced speed to 1.10x real time.
- Disabling all runtime priming improved cold initialization but raised first-buffer latency to approximately 418 ms.
- Six-frame adaptive windows reduced maximum gaps to roughly 395-423 ms but reduced speed to 1.23-1.24x real time.
- Decoder graph construction, allocation, upload, download, and reset were negligible; GPU compute dominated each decode, so multi-shape graph caching was not implemented.
- Priming an exact five-frame decoder shape left startup at 376-386 ms and increased initialization work.
- A 10-40 ms context crossfade did not improve the worst local artifact score and cannot repair PCM already emitted to a live callback.
- Zero context on every steady window started in 324-331 ms and ran at 1.18x, but shortened a 9.74 s utterance to 9.07 s.
- Five-frame steady windows ran at 1.23-1.24x, but startup headroom fell to 2-9 ms and the worst local artifact score rose to 2.154.

## Reproduction

Use `tools/streaming_regression_benchmark.py` for seeded corpus runs and `tools/streaming_callback_benchmark.py` for first-buffer/cadence measurements. Generated reports and WAV files belong in ignored temporary or `*_runs` directories.
