# Architecture

## Components

```text
qwen3_streaming_cli or host application
        |
        v
qwen3_tts_streaming integration library
        |
        v
Qwen3TTS engine
  | tokenizer
  | optional instruction or speaker conditioning
  | autoregressive talker + residual code predictor
  | incremental or batch vocoder
  v
24 kHz mono PCM
```

The wrapper links directly to the engine. `Qwen3StreamingTts::load()` loads the selected model immediately and reuses it until another model is requested. It is movable, not copyable, owns its implementation with `std::unique_ptr`, and reports errors through `last_error()`.

## Model loading

The selected TTS GGUF supplies the text tokenizer, talker transformer, residual code predictor, model-family metadata, and, where supported, the speaker encoder. `qwen3-tts-tokenizer-f16.gguf` supplies the audio decoder/vocoder.

The speaker encoder is lazy. The vocoder is also lazy when `QWEN3_TTS_LOW_MEM=1`. By default, model loading primes a short transformer and first-vocoder path once. Set `QWEN3_TTS_PRIME_RUNTIME=full` to prime the steady vocoder shape too, or `0` to disable priming.

Model families:

- `base`: general synthesis and speaker-embedding voice cloning.
- `custom_voice`: named-speaker or embedding-conditioned synthesis.
- `voice_design`: instruction-conditioned synthesis; speaker embeddings are invalid.

## Generation

Text is encoded as a Qwen assistant message. Instructions are encoded separately as a user message. Exact instruction token sequences may be cached by key for repeated requests.

For every acoustic frame:

1. The talker samples semantic codebook 0 using repetition penalty, temperature, top-k, and its separate CB0 top-p control. CB0 top-p defaults to 1.0 to protect EOS reliability.
2. EOS terminates generation; thinking tokens are filtered from emitted audio frames.
3. The residual predictor samples codebooks 1-15 using an independent seeded RNG.
4. The combined codec embedding and trailing text state advance the talker.
5. Streaming generation queues newly available codec frames for vocoder decode.

The listening-selected defaults are temperature 0.75, top-k 16, residual top-p 0.9, repetition penalty 1.02, and semantic CB0 top-p 1.0. The effective generation limit is the smaller of `max_audio_tokens` and five frames per content token, with a 64-frame minimum. Results distinguish EOS, the text-relative limit, and the absolute token limit.

## Voice cloning

Base models accept either a reference WAV or a stored 1,024-value speaker embedding. The speaker encoder expects 24 kHz mono audio and is loaded lazily. Stored embeddings avoid repeat extraction and are model-family-specific. The repository includes Lana and Priestley 0.6B and 1.7B F16 embeddings and their reference audio under `reference/`.

## Streaming decode

The first decode is queued when the first-window frame count is available. Later jobs use ramp or steady windows. Each job includes left-context frames; samples attributable to that context are dropped after decode, leaving only new PCM.

With asynchronous decode enabled, one worker processes vocoder jobs while generation continues producing codes. The final job uses the configured final context and marks the final PCM callback.

Default engine and wrapper values:

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

`offgrid-callback` uses a 5-frame first window, two 5-frame ramp windows, 2-frame left context, adaptive 7-8-frame steady windows, 240 ms callback chunks, and a 520 ms target lead.

## Delivery and playback

When callback pacing is enabled, decoded PCM enters a delivery buffer and a worker emits smaller chunks. Returning `false` from the wrapper callback requests cancellation. The full PCM result remains available internally; the CLI writes it to WAV, while integration callers can leave `output_wav` empty for callback-only operation.

Windows live playback is owned by the engine. The device is retained by each `Qwen3TTS` instance and reused between requests. The CLI no longer contains a second application-specific player or buffering policy.

## Public surfaces

- `engine/src/qwen3_tts.h`: primary C++ engine API.
- `apps/streaming_cli/include/offgrid_tts/Qwen3StreamingTts.h`: reusable streaming integration API.
- `engine/src/main.cpp`: engine CLI.
- `apps/streaming_cli/src/cli/main.cpp`: streaming/profile harness.

The integration path is controlled by `QWEN3_TTS_BUILD_STREAMING_WRAPPER`; its CLI can be omitted independently with `QWEN3_TTS_BUILD_STREAMING_CLI=OFF`. The Windows streaming CLI routes diagnostics to stdout so PowerShell does not render normal engine logs as errors.
