# Voice Quality Experiments

Branch: `voice-quality`

These experiments were run to separate:

- VoiceDesign generation instability
- streaming incremental decode / overlap-trim artifacts
- prompt-length effects

Artifacts are in:

- `tools/voice_quality_runs/`
- `tools/voice_quality_runs/detector_report.json`

## Setup

Instruction:

```text
A 30 year old woman with a rich feminine voice.
```

Joined text:

```text
I was not expecting visitors this late. What a pleasure it is to meet you here. Did you know I offer sandwiches for sale? Tell me about your favorite childhood memory.
```

Short text:

```text
I was not expecting visitors this late. What a pleasure it is to meet you here.
```

Detector:

- `tools/detect_synthetic_spans.py`
- heuristic only
- based on spectral flatness, centroid, high-band ratio, and frame instability

## Initial Matrix

| Case | Path | Sampling | Text | Duration (s) | Worst Region Score | Mean Top-3 Score |
|---|---|---|---|---:|---:|---:|
| `batch_greedy_joined` | engine `--batch` | greedy | joined | 25.577 | 1.397 | 1.177 |
| `stream_greedy_joined` | streaming CLI | greedy | joined | 25.577 | 1.407 | 1.225 |
| `batch_default_joined` | engine `--batch` | `temp=0.9 top-k=75` | joined | 11.417 | 1.974 | 1.517 |
| `stream_default_joined` | streaming CLI | `temp=0.9 top-k=75` | joined | 11.657 | 1.696 | 1.616 |
| `batch_cool_joined` | engine `--batch` | `temp=0.6 top-k=40 top-p=0.95 rep=1.04` | joined | 11.577 | 2.008 | 1.539 |
| `stream_cool_joined` | streaming CLI | `temp=0.6 top-k=40 top-p=0.95 rep=1.04` | joined | 13.257 | 1.884 | 1.549 |
| `batch_default_short` | engine `--batch` | `temp=0.9 top-k=75` | short | 5.257 | 1.560 | 1.448 |
| `stream_default_short` | streaming CLI | `temp=0.9 top-k=75` | short | 5.337 | 1.453 | 1.420 |

## Findings

### 1. Streaming is not the primary cause

The strongest result is the greedy A/B:

- `batch_greedy_joined.wav`
- `stream_greedy_joined.wav`

They have:

- the same duration
- nearly the same top suspect regions
- nearly the same detector scores

That means the synthetic stretches are already present before the streaming
wrapper can meaningfully damage them. The incremental decode path may still
color some edges, but it is not the main source of the problem.

### 2. VoiceDesign generation quality is the dominant variable

The sampled runs are substantially worse than the greedy runs:

- default sampled joined and cool sampled joined both score worse than greedy
- this holds in both batch and streaming paths

That points to VoiceDesign token generation quality as the first-order problem.

### 3. Shorter prompts help somewhat

Comparing:

- `batch_default_joined.wav` vs `batch_default_short.wav`
- `stream_default_joined.wav` vs `stream_default_short.wav`

The short-text runs still show suspect regions, but the overall exposure is
lower and the artifact spans are less spread out.

This suggests longer joined utterances allow persona drift or acoustic-token
instability to accumulate.

### 4. Cooler sampled settings did not fix the issue

This sweep used:

- `temperature=0.6`
- `top-k=40`
- `top-p=0.95`
- `repetition_penalty=1.04`

It did not materially improve the detector score relative to the default
sampled settings, and in some cases it was slightly worse.

## Current Best Read

The evidence on this branch supports:

1. the root problem is mostly in VoiceDesign generation quality
2. streaming decode is a secondary concern, not the main cause
3. long joined prompts increase exposure

## Implication For The Goal

The eventual goal is still valid: get streaming fidelity high.

But the order of operations should be:

1. improve VoiceDesign generation stability first
2. then verify the streaming path preserves that quality

If the source acoustic tokens are already unstable, no streaming overlap or
pacing fix will fully solve the synthetic stretches.

## Follow-Up Improvements

The branch now includes a stronger reference-render path in:

- `tools/voicedesign_to_wav.py`

New capabilities added after the initial matrix:

- batch VoiceDesign rendering from the engine CLI
- per-sentence segmentation
- configurable inter-segment silence and edge fades
- best-of-N candidate selection per segment using `tools/detect_synthetic_spans.py`

## Best Current Recipe

For the short two-sentence test line, the best detector result so far came from:

- `temperature=0.35`
- `top-k=16`
- `top-p=0.95`
- `takes-per-segment=6`
- `segment-gap-ms=120`
- `fade-ms=20`

Artifact:

- `tools/voice_quality_runs/hf_bestof_temp035_plus.wav`

Detector summary:

| Case | Duration (s) | Worst Half-Second Score | Notable Read |
|---|---:|---:|---|
| `stream_default_short` | 5.337 | 0.687 | old streaming helper baseline |
| `hf_gapfade` | 5.004 | 0.594 | batch + per-sentence + gap/fade |
| `hf_bestof_temp02` | 5.244 | 0.527 | best-of-4 with mild sampling |
| `hf_bestof_temp035` | 5.724 | 0.502 | best-of-4 with more diversity |
| `hf_bestof_temp035_plus` | 4.954 | 0.472 | best-of-6 + softer stitching; current best |

## Updated Read

The improvements above reinforce the earlier conclusion:

1. the biggest fidelity gains come from generation-side controls
2. short segmented renders are better than long joined renders
3. objective post-selection of sampled candidates helps more than forcing pure greedy output
4. stitching quality matters enough that pause/fade handling should not be treated as a cosmetic detail
