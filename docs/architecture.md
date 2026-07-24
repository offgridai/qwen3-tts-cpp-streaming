# Architecture

## Components

```text
qwen3_streaming_cli
        |
        v
Qwen3StreamingTts wrapper
        |
        v
Qwen3TTS engine
  | tokenizer
  | optional instruction or speaker conditioning
  | autoregressive talker + residual code predictor
  | incremental or batch vocoder
  v
24 kHz mono PCM + optional hint metadata
```

The wrapper links directly to the engine library. `Qwen3StreamingTts::load()` validates and records the model directory; the selected model is loaded on the first synthesis request and reused until the model name changes.

## Model loading

The selected TTS GGUF supplies the text tokenizer, talker transformer, residual code predictor, model-family metadata, and—where supported—the speaker encoder. `qwen3-tts-tokenizer-f16.gguf` supplies the audio decoder/vocoder.

The speaker encoder is lazy. The vocoder is also lazy when `QWEN3_TTS_LOW_MEM=1`. By default, model loading primes a short transformer and first vocoder path once. Set `QWEN3_TTS_PRIME_RUNTIME=full` to prime the steady vocoder shape too, or `0` to disable priming.

Model families:

- `base`: general synthesis and speaker-embedding voice cloning.
- `custom_voice`: named-speaker or embedding-conditioned synthesis.
- `voice_design`: instruction-conditioned synthesis; speaker embeddings are invalid.

## Generation

Text is encoded as a Qwen assistant message. Instructions are encoded separately as a user message. Exact instruction token sequences may be cached by key for repeated requests.

For every acoustic frame:

1. The talker samples semantic codebook 0, applying repetition penalty, temperature, top-k, and its separate CB0 top-p control. CB0 top-p defaults to 1.0 to protect EOS reliability.
2. EOS terminates generation; thinking tokens are filtered from emitted audio frames.
3. The residual predictor autoregressively samples codebooks 1–15 with temperature, top-k, and the regular acoustic top-p control. It uses an independent seeded RNG so acoustic settings do not change the semantic random stream.
4. The combined codec embedding and trailing text state advance the talker.
5. The streaming callback receives the complete generated-code prefix.

The listening-selected defaults are temperature 0.75, top-k 16, residual top-p 0.9, repetition penalty 1.02, and semantic CB0 top-p 1.0. Generation progress callbacks report completed acoustic frames against the effective limit. By default that limit is the smaller of `max_audio_tokens` and five frames per content token, with a 64-frame minimum. Results distinguish natural EOS, the text-relative safety limit, and the absolute token limit.

## Voice cloning

Base models accept either a reference WAV or a stored 1,024-value speaker embedding. The speaker encoder expects 24 kHz mono audio and is loaded lazily. The frontend uses a radix-2 FFT for log-mel extraction; a 20.3-second reference takes about 249 ms after model initialization on the baseline RTX 5090.

Stored embeddings avoid repeat extraction and are model-family-specific. The repository includes `lana` and `priestley` 0.6B F16 references and embeddings under `reference/`.

## Streaming decode

The first decode is queued when the first-window frame count is available. Later windows use ramp or steady sizes. Each job includes left-context frames; after vocoder decode, samples produced by that context are dropped and only new PCM is appended.

With asynchronous decode enabled, one worker processes decode jobs while the generation thread continues producing codes. Shared text-progress state is mutex-protected. The final job uses the configured final context and marks the final callback.

Default engine/wrapper values:

```text
first/ramp/steady windows: 3 / 6 / 8 frames
ramp count:                0
context/early/final:       2 / 1 / 3 frames
early-context windows:     2
async decode:              on
adaptive windows:          off
paced delivery:            off
delivery chunk/start:      40 / 40 ms
delivery target lead:      300 ms
```

`offgrid-callback` uses a 5-frame first window, two 5-frame ramp windows, 2-frame left context, and adaptive 7-8-frame steady windows. Callback delivery uses 240 ms chunks, starts at a 350 ms preroll, then permits up to 520 ms of steady lead to cover the larger decode windows.

## Paced delivery

Vocoder windows are relatively coarse. When pacing is enabled, decoded PCM enters a second buffer and a delivery thread emits smaller callback chunks. Codec provenance is retained as ranges because a paced chunk may overlap more than one vocoder window.

The complete PCM result is still accumulated and written to WAV by both CLIs. Callback delivery does not discard the final output.

## Hint semantics

The header reports sample rate, model family, conditioning presence, text-token count, and whether experimental text progress is available.

Each chunk reports:

- codec-frame and absolute audio-sample ranges;
- start/end seconds with exclusive end semantics;
- RMS, peak, zero-crossing rate, and coarse energy class;
- experimental activity spans and speech occupancy;
- experimental monotonic text-progress and token-index estimates;
- paced/final flags.

Sample and frame provenance are authoritative. Activity classification is PCM-derived. Text progress compares each talker hidden state against projected text-token embeddings within a constrained forward search window. It is a heuristic cursor, not linguistic alignment.

## Public surfaces

- `engine/src/qwen3_tts.h`: primary C++ API.
- `engine/src/qwen3_tts_c.h`: compact C ABI for batch-style synthesis and embedding workflows.
- `apps/streaming_cli/include/offgrid_tts/Qwen3StreamingTts.h`: integration wrapper with callback and hint types.
- `engine/src/main.cpp`: engine CLI.
- `apps/streaming_cli/src/cli/main.cpp`: streaming/profile harness.

The Windows streaming CLI routes diagnostic output to stdout so PowerShell does not render normal engine logs as errors. The engine CLI retains conventional stderr diagnostics.
