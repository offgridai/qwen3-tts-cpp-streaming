# CUDA graph evaluation

## Current 1.7B F16 baseline

Measured on 2026-07-27 at revision `350ff5b` on an RTX 5090 with the Base F16
model, Priestley voice, seed 42, `offgrid-callback`, and a simulated 350 ms
playback buffer:

| Dimension | Current result |
|---|---:|
| Cold model/runtime start with stored clone | 1.932-1.978 s |
| Encode current 28.8 s reference after model load | 348-355 ms |
| Cold new-clone readiness | approximately 2.28-2.33 s |
| Resident first 350 ms | 319-342 ms |
| Cold first 350 ms with stored clone | approximately 2.25-2.32 s |
| Standard-passage streaming speed | 2.48-2.53x realtime (RTF 0.395-0.404) |
| Standard-passage maximum production gap | 251-295 ms |
| Standard-passage minimum playback headroom | 301-310 ms |
| Simulated playback underruns | 0/3 |

The standard passage was `Use a reasonably long paragraph for this consistency
test.` Five longer tuning runs with the smoother window policy measured RTF
0.345-0.356, first-buffer readiness of 309-318 ms, and no simulated underruns.

## Optimization history

Measured on an RTX 5090 with the 1.7B Base F16 model, the Priestley embedding,
seed 42, and the `offgrid-callback` streaming profile. Each result is the mean
of four alternating runs of the same passage.

| Runtime policy | First playable 350 ms | RTF | Maximum callback gap |
|---|---:|---:|---:|
| CUDA graphs enabled | 387 ms | 0.794 | 426 ms |
| CUDA graphs disabled | 438 ms | 0.813 | 451 ms |

The existing GGML CUDA graph path therefore improves first-buffer latency by
about 51 ms (12%), sustained synthesis time by about 2.4%, and worst callback
gap by about 25 ms for this workload.

Set `GGML_CUDA_GRAPH_STATS=1` to print graph counters at backend shutdown. The
representative enabled run recorded:

```text
keys=18 active=17 compute=1352 compatible=1352 changes=39
warmups=35 resets=19 captures=35 launches=1313 replays=1278
```

CUDA graphs were already launched for 97% of eligible evaluations, and 95% of
all evaluations were direct graph replays. Build-time enablement is therefore
working in the Qwen path rather than merely compiling unused support.

An eager-recapture experiment replaced the post-change warmup reset with an
immediate executable-graph update. It raised launches from 1,313 to 1,333, but
four paired runs changed mean RTF only from 0.792 to 0.791, first-buffer time
from 382 ms to 376 ms, and maximum callback gap from 427 ms to 427 ms. This was
too small to justify the additional scheduling risk, so the behavior was not
adopted.

Further performance work should profile model kernels, vocoder work, host
synchronization, and allocation rather than assuming missing CUDA graph reuse
is the primary bottleneck.

## Follow-up: GPU-resident vocoder

That profiling identified the main bottleneck: decoder weights were allocated
on CPU and copied through the scheduler on every vocoder graph. Loading them
once into the decoder's live CUDA backend was substantially more valuable than
additional graph capture.

The resident decoder deliberately uses eager CUDA execution and the legacy
scratch allocator. Dynamic decoder graphs were not capture-safe, and GGML's
VMM scratch pool requires strict LIFO release that this streaming workload does
not guarantee. Transformer CUDA graphs and VMM remain enabled.

Measured on the same RTX 5090, 1.7B Base F16, Priestley voice, seed 42, and
passage as above (two final runs):

| Metric | Previous | Resident vocoder |
|---|---:|---:|
| First playable 350 ms | 387 ms | 294-306 ms |
| RTF | 0.794 | 0.360 |
| Throughput | 1.26x realtime | 2.78x realtime |
| Maximum production gap | 426 ms | 316-323 ms |
| Minimum playback headroom | not recorded | 207-222 ms |
| Simulated underruns | 0 | 0 |

This is a 55% RTF reduction and brings the native GGML path comfortably past
the reported `faster-qwen3-tts` result of approximately 0.8 RTF. It is not a
same-GPU comparison: the external report used a GTX 1660 Super, while these
measurements use an RTX 5090.

The quick acceptance suite also passed callback/WAV parity, cancellation,
cadence, fidelity heuristics, and reliability. The portable CUDA build now
contains native GTX 16-series (`sm_75`), RTX 4090 (`sm_89`), and RTX 5090
(`sm_120a`) code. Only the RTX 5090 has been performance-tested locally.
