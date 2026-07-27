# Qwen3-TTS 0.6B current performance baseline

This is the accepted local baseline for `main` at revision `58661ee` on 2026-07-24. Results are hardware- and passage-specific measurements, not portable model claims. The original pre-optimization baseline is retained in `performance-baseline-0.6b.md`.

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

`offgrid-callback` starts with five frames, uses two five-frame ramp windows, then adaptive seven- to eight-frame windows with two frames of left context. The benchmark evaluates a downstream 350 ms callback buffer and permits up to 520 ms of lead after startup.

The distributable CUDA build contains native GTX 16-series (`sm_75`), RTX 4090
(`sm_89`), and RTX 5090 (`sm_120a`) kernels. All figures below remain RTX 5090
measurements; they must not be treated as GTX 1660 or RTX 4090 performance
claims until the acceptance suite has run on that hardware.

## Top-level results

| Dimension | Current baseline | Notes |
|---|---:|---|
| Cold model/runtime initialization | 1.286 s mean, 1.280-1.294 s | Tokenizer, transformer, vocoder, and short runtime prime. |
| Extract embedding from 20.3 s reference | 249 ms mean, 241-260 ms | Model already loaded; 55x faster than the original frontend. |
| Extract Priestley embedding from 48.2 s reference | 610 ms | Produces 1,024 finite values. |
| Cold clone readiness, 20.3 s reference | approximately 1.54 s | Initialization plus extraction. |
| Cold first 350 ms with stored embedding | approximately 1.66-1.67 s | Initialization plus resident callback startup. |
| Resident first 350 ms callback buffer | 373-381 ms | Configured preroll is 350 ms; compute determines actual readiness. |
| Maximum callback/decode gap | 430-441 ms | Short and long seeded probes. |
| Minimum simulated playback headroom | 21-55 ms | Measured after playback starts. |
| Simulated playback underruns | 0 | Short and long seeded probes. |
| Callback streaming speed | 1.20-1.25x real time | RTF 0.797-0.835. |
| Batch synthesis speed | approximately 1.41x real time | Priestley evaluation passage. |

Steady seven-frame windows produce about 560 ms of new PCM in roughly 420-441 ms. The larger post-start lead is intentional: it does not increase the benchmark's initial 350 ms downstream buffer target, but it absorbs bursty vocoder completion.

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

Seeded synthesis is byte-deterministic. Semantic CB0 and residual-codebook sampling use separate seeded random streams. CB0 top-p remains 1.0 because lower values prevented natural EOS for some seeds. A dynamic ceiling of five audio frames per content token, with a 64-frame minimum, prevents runaway generation when EOS is not sampled.

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
