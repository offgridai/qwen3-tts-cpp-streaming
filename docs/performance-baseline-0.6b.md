# Qwen3-TTS 0.6B performance baseline

This is the historical pre-optimization baseline for the native streaming path. Results are local measurements, not portable model claims. See `performance-baseline-0.6b-current.md` for the accepted baseline on current `main`.

## Test context

| Item | Value |
|---|---|
| Date | 2026-07-24 |
| Source revision | `2e6a8fc76c4017b6e07767260d690c95b7edf869` |
| GPU | NVIDIA GeForce RTX 5090, 32 GB |
| Driver | 595.95 |
| OS | Windows 10.0.26200 |
| Backend | GGML CUDA |
| Model | `qwen3-tts-0.6b-f16` |
| Clone | `reference/lana_0.6b_f16.json` |
| Source reference | `reference/lana_ref.wav`, approximately 20.3 seconds |
| Streaming profile | `offgrid-callback`, 350 ms target lead |

Startup and cadence figures use three or five independent cold processes as noted below. The callback benchmark synthesized a four-sentence passage producing approximately 9.5-11.5 seconds of audio in successful runs.

## Baseline results

| Dimension | Baseline | Sample | Interpretation |
|---|---:|---:|---|
| Establish clone from reference WAV, model already loaded | 13.78 s mean | 3 | Dominated by speaker-embedding extraction; lazy encoder load was only 38 ms mean. |
| Establish clone from a cold process | approximately 15.44 s | derived | Model initialization plus clone establishment; excludes subsequent synthesis. |
| Initialize streaming model/runtime with cached clone | 1.69 s mean | 3 | Includes tokenizer, transformer, vocoder, and runtime prime. |
| Full cold process through a five-frame output | 2.24 s mean | 3 | Includes initialization, a minimal generation, decode, and process overhead. |
| First complete 350 ms callback buffer, resident engine | 372 ms mean, 361-386 ms | 5 | Request-clock latency; model initialization is excluded. |
| First complete 350 ms callback buffer, cold estimate | approximately 2.06 s | derived | Model initialization plus resident-engine buffer latency. |
| Second decode-window gap | 443 ms mean, 431-459 ms | 5 | Decode arrives in bursts rather than uniform callback-sized increments. |
| Maximum decode-window gap | 483 ms mean, 494 ms observed maximum | 5 | Requires downstream buffering despite faster-than-real-time synthesis. |
| Sustained synthesis speed | 1.36x real time, 1.35-1.37x | 5 | Mean real-time factor was approximately 0.734. |
| Normal completion at default sampling | 5/5 | 5 | Temperature 0.9, top-k 75, top-p 1.0. |

## Fidelity and reliability finding

Streaming transport completed normally and produced valid PCM in all five default callback trials. Throughput was tightly grouped, but this does not measure pronunciation accuracy, speaker identity, or perceptual quality.

Generation termination is the principal baseline defect. With the quality-sweep settings (temperature 0.75, top-k 16, top-p 0.9), two of six small-sample streaming attempts did not terminate normally:

- one reached a 256-frame bound and produced 20.46 seconds for the short passage;
- one reached the 4096-frame bound and produced 327.66 seconds;
- successful outputs for the same text ranged from 8.54 to 13.10 seconds.

The spectral artifact detector marked short suspect regions, but it is heuristic and has no calibrated error-rate interpretation. No ASR word-error rate, speaker-similarity score, listening MOS, or preference score was collected. Consequently, this baseline records fidelity as **unquantified**, with observed repetition/EOS failures, rather than assigning an unsupported fidelity percentage.

## Optimization roadmap

Optimizations should be evaluated against a fixed corpus and fixed random seeds. Every experiment must report quality and termination alongside latency; a faster configuration that degrades either is a regression.

### 1. Fidelity, errata, and termination

1. Add explicit seed control and a benchmark corpus covering short prompts, long prompts, punctuation, numbers, names, and silence-prone text.
2. Instrument EOS rank/probability, sampled-token history, repetition loops, text-progress stalls, and generated-duration ratio.
3. Audit EOS handling under top-k/top-p, especially whether nucleus filtering removes EOS indefinitely.
4. Add loop detection and a bounded recovery policy: relax sampling or admit EOS after demonstrated text completion, then apply a hard safety ceiling.
5. Add objective evaluation: ASR WER/CER, speaker-embedding cosine similarity, duration ratio, silence/repetition detection, and the existing spectral artifact score.
6. Run blinded listening checks on the best objective candidates.

Acceptance target: zero unbounded generations; at least 99.5% natural termination across 1,000 seeded cases; less than 1% repetition/truncation failures; no statistically significant WER, speaker-similarity, or listening-quality regression from batch output.

### 2. Clone establishment

1. Break the 13.74-second extraction stage into resampling, feature extraction, encoder execution, and pooling timings.
2. Test whether a selected clean 3-8 second reference segment preserves speaker similarity. Avoid processing a full 20-second recording when it adds no value.
3. Cache normalized audio/features by content hash and always persist the final embedding.
4. Reuse the encoder context and move or fuse any remaining CPU-heavy frontend operations.
5. Investigate batched extraction only after the single-clone path is profiled.

Acceptance target: no work for a cached clone; less than 3 seconds to establish a new clone from a selected 5-8 second clip, with speaker similarity no worse than the full-reference baseline.

### 3. Streaming model initialization

1. Make a persistent resident engine the normal service architecture so initialization is paid once.
2. Profile file I/O, tensor upload, graph construction, and the approximately 795 ms runtime prime separately.
3. Parallelize independent tokenizer, transformer, and vocoder preparation where GPU-memory ordering allows it.
4. Cache reusable graph plans and evaluate memory mapping or asynchronous weight upload.
5. Decide whether eager prime, lazy prime, or background prime gives the best cold-request tradeoff.

Acceptance target: less than 1.0 second cold initialization and less than 10 ms cached-clone request setup; resident requests should not reload model state.

### 4. First 350 ms of streamed audio

1. Trace text tokenization, transformer prefill, first codec tokens, first vocoder window, PCM copy, and pacing independently.
2. Compare three- and five-frame first windows for latency, boundary quality, and total throughput.
3. Start the first vocoder work as soon as its minimum safe context exists and overlap later token generation with decode.
4. Avoid copies between vocoder output, pacing storage, and the client callback.
5. Precompute clone-conditioned state that is invariant across requests.

Acceptance target: a full 350 ms buffer in at most 300 ms on a resident engine, with no measurable boundary-quality regression; cold first-buffer latency below 1.3 seconds after initialization work is complete.

### 5. Ongoing streaming consistency

1. Report p50/p95/p99 decode gaps, callback gaps, buffer lead, and actual underruns rather than only means and maxima.
2. Enable a lead-aware adaptive window controller: decode earlier or increase a window before the buffer reaches its danger threshold.
3. Separate generation, vocoder, and delivery scheduling; keep the callback thread free of GPU waits and allocations.
4. Evaluate separate CUDA streams and overlap only where profiling shows genuine concurrency.
5. Add 10- and 30-minute soak runs with randomized text lengths and deliberate client backpressure.

Acceptance target: zero underruns in a 30-minute soak with a 350 ms target buffer, p99 callback gap below the available audio lead, and no long-tail gap above 350 ms after startup.

### 6. Streaming speed versus real time

1. Attribute time separately to talker generation, residual codebooks, vocoder, synchronization, and PCM handling.
2. Capture and reuse stable CUDA graphs; remove avoidable host synchronization and allocation in the token loop.
3. Improve KV-cache locality and fuse sampling operations where profiling justifies it.
4. Overlap vocoder windows with token generation and tune steady-state window size for throughput.
5. Evaluate F16, Q5_K, and Q4_K as separate quality/performance points rather than assuming quantization is free.

Acceptance target: at least 2.0x real time on the baseline RTX 5090 for F16 while meeting all fidelity and cadence gates. Quantized profiles require their own quality baselines.

## Recommended execution order

1. Build the deterministic evaluation harness and fix EOS/repetition failures.
2. Profile and shorten clone extraction.
3. Reduce resident-engine first-buffer latency.
4. Add buffer-aware cadence control and soak testing.
5. Optimize steady-state kernels and overlap.
6. Rework cold initialization after the resident path is stable.

Record each accepted experiment as a new dated result alongside this file; do not overwrite this baseline.
