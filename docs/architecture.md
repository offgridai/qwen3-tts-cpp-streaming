# qwen3-tts-cpp-streaming Architecture

## Purpose

`qwen3-tts-cpp-streaming` is a low-latency native C++ streaming voice-clone system built on top of the open-source `qwen3-tts-cpp` project.

The goal is real-time streaming TTS:

- First audio in ~175–325 ms
- Continuous PCM streaming
- Fully local execution (no cloud, no Python at runtime)

---

## Key Clarification

The `qwen3-tts-cpp` repository is **not a vendored dependency**.

It is:

- an **upstream open-source project**
- actively improved independently
- the core engine powering this system

This repo:

```text
qwen3-tts-cpp-streaming
```

is a **layer on top of that project**, adding:

- streaming orchestration
- stable defaults
- contributor-facing API
- integration surface (CLI, future Unreal/service)

Think of it as:

```text
qwen3-tts-cpp       → engine (upstream OSS)
qwen3-tts-cpp-streaming → streaming system + product layer
```

---

## High-Level Architecture

```text
CLI / API (this repo)
        ↓
Streaming layer (this repo)
        ↓
qwen3-tts-cpp engine (upstream OSS)
        ↓
Qwen3 model + vocoder
        ↓
streamed PCM audio
```

---

## Responsibilities Split

### qwen3-tts-cpp (Upstream)

Owns:

- model loading
- tokenizer
- speaker embedding handling
- transformer inference
- vocoder decode
- CUDA execution

### This Repository

Owns:

- streaming strategy
- latency optimizations
- default configuration
- CLI / integration surface
- future API abstraction
- service / Unreal integration

---

## Why This Separation Matters

Do NOT:

- rewrite core inference here
- fork unnecessarily
- duplicate engine logic

DO:

- improve streaming behavior
- upstream fixes where appropriate
- keep integration layer clean

---

## Streaming Strategy

The system uses:

```text
Incremental Tail-Context Streaming
```

Key idea:

```text
decode only new frames
+ small left context
→ emit stable audio
```

Avoids:

- robotic artifacts (independent windows)
- O(N²) decode (prefix method)

---

## Core Optimizations

- Tail-context decode (no prefix recompute)
- 1-frame first window
- Immediate first-frame scheduling
- Async decode pipeline
- Playback thread isolation
- Streaming prewarm
- Minimal buffering

---

## Performance

Typical:

```text
First PCM:     ~175–325 ms
RTF:           ~0.8–1.2
```

This is sufficient for real-time dialogue.

---

## Current Implementation

Today:

```text
CLI → bridge → qwen3-tts-cli.exe
```

This is transitional.

Next step:

```text
CLI → direct C++ API → engine
```

---

## Future API Shape

```cpp
class Qwen3StreamingTts {
public:
    bool load(...);
    bool synthesize_streaming(...);
};
```

With callback-based PCM delivery.

---

## Build Flow

```bat
cd third_party\qwen3-tts-cpp
build.ps1 -UseNinja -EnableCuda -Configuration Release

cd ..
cmake -S . -B build
cmake --build build
```

---

## Runtime Requirements

- NVIDIA GPU
- CUDA driver
- model files (not checked in)
- speaker embedding JSON

---

## Contributor Guidelines

- Preserve baseline behavior
- Measure before optimizing
- Keep playback async
- Do not reintroduce Python
- Do not add HTTP layers
- Keep system local-first

---

## Roadmap

1. Remove subprocess bridge
2. Expose PCM callback API
3. Add cancellation
4. Add service integration
5. Improve packaging

---

## Summary

This repo is:

```text
a streaming + integration layer
on top of an upstream TTS engine
```