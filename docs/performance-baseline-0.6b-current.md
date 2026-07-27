# Qwen3-TTS 0.6B current performance baseline

This is the accepted local baseline for `main` at revision `350ff5b` on 2026-07-27. Results are hardware- and passage-specific measurements, not portable model claims. The original pre-optimization baseline is retained in `performance-baseline-0.6b.md`.

`performance-baseline-0.6b-current.json` contains the same acceptance-critical values in machine-readable form.

## Test context

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 5090, 32 GB |
| Driver | 595.95 |
| OS | Windows 10.0.26200 |
| Backend | GGML CUDA |
| Model | `qwen3-tts-0.6b-f16` |
| Primary clone | `reference/lana_0.6b_f16.json` |
| Additional clone | `reference/priestley_0.6b_f16.json` |
| Sampling | temperature 0.75, top-k 16, residual top-p 0.9, repetition penalty 1.02, CB0 top-p 1.0 |
| Streaming profile | `offgrid-callback` |

`offgrid-callback` starts with six frames, uses two six-frame ramp windows, then
fixed ten-frame windows with four frames of left context. Early windows use
two context frames and the final window uses eight. The benchmark evaluates a
downstream 350 ms callback buffer and permits up to 520 ms of lead after startup.

The distributable CUDA build contains native GTX 16-series (`sm_75`), RTX 4090
(`sm_89`), and RTX 5090 (`sm_120a`) kernels. All figures below remain RTX 5090
measurements; they must not be treated as GTX 1660 or RTX 4090 performance
claims until the acceptance suite has run on that hardware.

## Top-level results

| Dimension | Current baseline | Notes |
|---|---:|---|
| Cold model/runtime initialization | 1.310 s mean, 1.295-1.328 s | Tokenizer, transformer, resident vocoder, and short runtime prime. |
| Extract embedding from current 28.8 s Priestley reference | 347 ms mean, 346-349 ms | Model already loaded; produces 1,024 finite values. |
| Cold clone readiness, current reference | approximately 1.66-1.68 s | Measured model initialization plus extraction. |
| Cold first 350 ms with stored embedding | approximately 1.58-1.63 s | Initialization plus resident callback startup. |
| Resident first 350 ms callback buffer | 283-302 ms | Remains below the 350 ms perceptual ceiling. |
| Maximum callback/decode gap | 239-262 ms | Larger, less frequent quality-oriented windows. |
| Minimum simulated playback headroom | 257-313 ms | Measured after playback starts. |
| Simulated playback underruns | 0/3 | Three standard-passage runs. |
| Callback streaming speed | 2.61-2.72x real time | RTF 0.368-0.383. |

The principal gain comes from keeping vocoder weights resident on CUDA instead
of copying them through the scheduler for every decode graph. The decoder uses
eager CUDA execution and the legacy scratch allocator; transformer CUDA graphs
and VMM remain enabled. Steady callback chunks are produced faster than
playback consumes them, increasing headroom without increasing the configured
350 ms downstream buffer target.

The smoother default uses that headroom for six-frame startup/ramp windows,
ten-frame steady windows, and four-frame overlap context. Compared with the
short-window policy, it reduces vocoder join frequency and gives each join more
acoustic history. Automated spectral heuristics were neutral, so the expected
quality benefit must be confirmed by listening.

## Fidelity and reliability

The six-seed passage that reproduced the original termination defect now records:

| Metric | Current baseline |
|---|---:|
| Natural EOS | 5/6 |
| Text-relative safety completion | 1/6 |
| Unbounded or absolute token-limit completion | 0/6 |
| Mean worst-half-second spectral score | 0.796 |
| Mean worst-region spectral score | 1.618 |

Lower spectral scores are better, but these detectors are uncalibrated heuristics. They did not reliably predict every audible streaming seam during optimization. Human A/B listening is therefore a required acceptance check.

Semantic CB0 and residual-codebook sampling use separate seeded random streams.
The acceptance suite enforces fixed-seed byte determinism, although intermittent
byte-level divergence was observed during repeated 2026-07-27 wrapper probes
and remains an open reliability item. CB0 top-p remains 1.0 because lower values
prevented natural EOS for some seeds. A dynamic ceiling of five audio frames per
content token, with a 64-frame minimum, prevents runaway generation when EOS is
not sampled.

The optimized FFT speaker frontend reproduced the prior embedding with cosine similarity 0.999999996, maximum absolute difference 0.000089, and RMSE 0.000028.

For the accepted Priestley listening passage:

| Metric | Batch | Streaming |
|---|---:|---:|
| Duration | 10.217 s | 10.217 s |
| Worst-half-second spectral score | 0.475 | 0.512 |
| Worst-region spectral score | 1.832 | 1.973 |
| Human assessment | sounds good | sounds good |

Objective ASR WER/CER, multi-voice speaker similarity, listening MOS, p95/p99 cadence, and long-duration soak testing remain outstanding.

## Reproduction

Build the measured targets:

```powershell
cmake --build build-ninja-cuda --target `
  tts_engine_cli qwen3_streaming_cli tts_engine_quantize
```

Measure callback startup, cadence, lead, underruns, and real-time factor:

```powershell
py -3 tools\streaming_callback_benchmark.py `
  --input-json reference\lana_0.6b_f16.json `
  --seed 42 `
  --text "Use a reasonably long paragraph for this consistency test." `
  --output-json callback_baseline.json
```

Run the deterministic termination/performance corpus:

```powershell
py -3 tools\streaming_regression_benchmark.py `
  --input-json reference\lana_0.6b_f16.json `
  --seeds 40,41,42,43,44,45 `
  --output-dir regression_runs
```

Render a fixed-seed batch/streaming listening pair:

```powershell
py -3 tools\streaming_quality_ab.py `
  --input-json reference\priestley_0.6b_f16.json `
  --output-dir priestley_ab `
  --seed 42 `
  --offgrid-callback-profile `
  --presets default
```

Generated reports and WAVs should remain in ignored temporary or run directories.
