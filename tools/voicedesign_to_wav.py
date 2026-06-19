#!/usr/bin/env python3
from __future__ import annotations

import argparse
from array import array
import contextlib
import json
import re
import shutil
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from detect_synthetic_spans import detect_file


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL_NAME = "qwen3-tts-1.7b-voicedesign-f16"
DEFAULT_BASE_MODEL_NAME = "qwen3-tts-1.7b-base-f16.gguf"
DEFAULT_BASE_MODEL_NAME_06B = "qwen3-tts-0.6b-f16.gguf"
DEFAULT_STREAMING_BUILD = [
    PROJECT_ROOT / "build-vs2022-x64" / "apps" / "streaming_cli" / "Release" / "qwen3_streaming_cli.exe",
    PROJECT_ROOT / "build-vs2022-x64" / "apps" / "streaming_cli" / "qwen3_streaming_cli.exe",
    PROJECT_ROOT / "build-ninja-cuda" / "apps" / "streaming_cli" / "qwen3_streaming_cli.exe",
    PROJECT_ROOT / "build-ninja-verify" / "apps" / "streaming_cli" / "qwen3_streaming_cli.exe",
]
DEFAULT_ENGINE_BUILD = [
    PROJECT_ROOT / "build-vs2022-x64" / "engine" / "Release" / "tts_engine_cli.exe",
    PROJECT_ROOT / "build-vs2022-x64" / "engine" / "tts_engine_cli.exe",
    PROJECT_ROOT / "build-ninja-cuda" / "engine" / "tts_engine_cli.exe",
    PROJECT_ROOT / "build-ninja-verify" / "engine" / "tts_engine_cli.exe",
]
SENTENCE_RE = re.compile(r"(?<=[.!?])\s+")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a VoiceDesign WAV from English text, with optional high-fidelity per-sentence rendering."
    )
    parser.add_argument(
        "--line",
        action="append",
        dest="lines",
        default=[],
        help="English line to include in the generated WAV. Pass multiple times.",
    )
    parser.add_argument(
        "--lines-file",
        type=Path,
        help="Optional text file containing one line per utterance.",
    )
    parser.add_argument(
        "--instruct",
        required=True,
        help="VoiceDesign instruction prompt describing the character voice.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="Output WAV path.",
    )
    parser.add_argument(
        "--models-dir",
        type=Path,
        default=PROJECT_ROOT / "models",
        help="Models directory. Default: ./models",
    )
    parser.add_argument(
        "--exe",
        type=Path,
        help="Optional explicit path to the render CLI. Honors --render-path.",
    )
    parser.add_argument(
        "--model-name",
        default=DEFAULT_MODEL_NAME,
        help=f"VoiceDesign model name. Default: {DEFAULT_MODEL_NAME}",
    )
    parser.add_argument(
        "--base-model-name",
        default=DEFAULT_BASE_MODEL_NAME,
        help=f"Base model name used for anchor-clone rendering. Default: {DEFAULT_BASE_MODEL_NAME}",
    )
    parser.add_argument(
        "--base-model-name-0_6b",
        default=DEFAULT_BASE_MODEL_NAME_06B,
        help=f"0.6B base model name used for optional speaker embedding export. Default: {DEFAULT_BASE_MODEL_NAME_06B}",
    )
    parser.add_argument(
        "--output-speaker-json-1_7b",
        type=Path,
        help="Optional path to export a reusable 1.7B speaker embedding JSON from the anchor reference.",
    )
    parser.add_argument(
        "--output-speaker-json-0_6b",
        type=Path,
        help="Optional path to export a reusable 0.6B speaker embedding JSON from the anchor reference.",
    )
    parser.add_argument(
        "--consistency-mode",
        choices=("voicedesign", "anchor-clone"),
        default="voicedesign",
        help="voicedesign renders every segment with VoiceDesign. anchor-clone makes one VoiceDesign anchor, extracts a speaker embedding, then renders the final composite with the base model.",
    )
    parser.add_argument(
        "--anchor-line-index",
        type=int,
        default=1,
        help="1-based line index used as the VoiceDesign anchor in anchor-clone mode.",
    )
    parser.add_argument(
        "--render-path",
        choices=("stream", "batch"),
        help="Render using the streaming CLI or the engine batch CLI.",
    )
    parser.add_argument(
        "--segmentation",
        choices=("joined", "per-line", "per-sentence"),
        help="How to split the text before rendering.",
    )
    parser.add_argument(
        "--fidelity-preset",
        choices=("default", "high"),
        default="default",
        help="Default preserves the legacy behavior. High uses a lower-variance render path.",
    )
    parser.add_argument("--temperature", type=float, help="Sampling temperature. 0 = greedy.")
    parser.add_argument("--top-k", type=int, help="Top-k sampling. 0 disables top-k.")
    parser.add_argument("--top-p", type=float, help="Top-p sampling.")
    parser.add_argument("--repetition-penalty", type=float, help="Repetition penalty.")
    parser.add_argument("--max-tokens", type=int, help="Optional max token cap passed to the CLI.")
    parser.add_argument(
        "--takes-per-segment",
        type=int,
        default=1,
        help="Generate multiple candidates per segment and keep the cleanest by detector score.",
    )
    parser.add_argument(
        "--segment-gap-ms",
        type=float,
        help="Silence inserted between rendered segments before concatenation.",
    )
    parser.add_argument(
        "--fade-ms",
        type=float,
        help="Linear fade-in/out applied to each rendered segment before concatenation.",
    )
    parser.add_argument(
        "--trim-silence",
        action="store_true",
        help="Trim leading and trailing low-amplitude silence from each rendered segment before stitching.",
    )
    parser.add_argument(
        "--trim-threshold",
        type=float,
        default=0.01,
        help="Normalized absolute amplitude threshold for silence trimming. Default: 0.01",
    )
    parser.add_argument(
        "--trim-pad-ms",
        type=float,
        default=35.0,
        help="Padding to preserve around detected speech when trimming, in milliseconds. Default: 35",
    )
    parser.add_argument(
        "--manifest-out",
        type=Path,
        help="Optional JSON manifest describing segments and render settings.",
    )
    parser.add_argument(
        "--keep-intermediates",
        action="store_true",
        help="Keep intermediate per-segment WAV files.",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Pass quiet mode to the underlying CLI when supported.",
    )
    return parser.parse_args()


def load_lines(args: argparse.Namespace) -> list[str]:
    lines = [line.strip() for line in args.lines if line.strip()]
    if args.lines_file:
        file_lines = [
            line.strip()
            for line in args.lines_file.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        lines.extend(file_lines)
    if not lines:
        raise SystemExit("No input lines provided. Use --line and/or --lines-file.")
    return lines


def split_sentences(line: str) -> list[str]:
    chunks = [chunk.strip() for chunk in SENTENCE_RE.split(line.strip()) if chunk.strip()]
    return chunks or [line.strip()]


def resolve_segments(lines: list[str], segmentation: str) -> list[str]:
    if segmentation == "joined":
        return [" ".join(lines)]
    if segmentation == "per-line":
        return lines
    if segmentation == "per-sentence":
        segments: list[str] = []
        for line in lines:
            segments.extend(split_sentences(line))
        return segments
    raise ValueError(f"Unsupported segmentation: {segmentation}")


def resolve_segment_entries(lines: list[str], segmentation: str) -> list[dict[str, object]]:
    entries: list[dict[str, object]] = []
    if segmentation == "joined":
        entries.append({"text": " ".join(lines), "line_index": 1, "line_count": 1, "is_line_final": True})
        return entries
    if segmentation == "per-line":
        for line_index, line in enumerate(lines, start=1):
            sentence_parts = split_sentences(line)
            for sentence_index, sentence in enumerate(sentence_parts, start=1):
                entries.append(
                    {
                        "text": sentence,
                        "line_index": line_index,
                        "line_count": len(sentence_parts),
                        "sentence_index": sentence_index,
                        "is_line_final": sentence_index == len(sentence_parts),
                    }
                )
        return entries
    if segmentation == "per-sentence":
        for line_index, line in enumerate(lines, start=1):
            sentence_parts = split_sentences(line)
            for sentence_index, sentence in enumerate(sentence_parts, start=1):
                entries.append(
                    {
                        "text": sentence,
                        "line_index": line_index,
                        "line_count": len(sentence_parts),
                        "sentence_index": sentence_index,
                        "is_line_final": sentence_index == len(sentence_parts),
                    }
                )
        return entries
    raise ValueError(f"Unsupported segmentation: {segmentation}")


def resolve_streaming_cli(explicit: Path | None) -> Path:
    if explicit:
        if explicit.exists():
            return explicit.resolve()
        raise SystemExit(f"Streaming CLI not found: {explicit}")
    for candidate in DEFAULT_STREAMING_BUILD:
        if candidate.exists():
            return candidate.resolve()
    raise SystemExit(
        "Could not find qwen3_streaming_cli.exe. Pass --exe or build the streaming CLI first."
    )


def resolve_engine_cli(explicit: Path | None) -> Path:
    if explicit:
        if explicit.exists():
            return explicit.resolve()
        raise SystemExit(f"Engine CLI not found: {explicit}")
    for candidate in DEFAULT_ENGINE_BUILD:
        if candidate.exists():
            return candidate.resolve()
    raise SystemExit(
        "Could not find tts_engine_cli.exe. Pass --exe or build the engine CLI first."
    )


def build_clone_batch_command(
    cli_path: Path,
    models_dir: Path,
    model_name: str,
    speaker_embedding: Path,
    text: str,
    output_path: Path,
    config: dict[str, object],
) -> list[str]:
    cmd = [
        str(cli_path),
        "-m",
        str(models_dir.resolve()),
        "--model-name",
        model_name,
        "--speaker-embedding",
        str(speaker_embedding),
        "--temperature",
        str(config["temperature"]),
        "--top-k",
        str(config["top_k"]),
        "--top-p",
        str(config["top_p"]),
        "--repetition-penalty",
        str(config["repetition_penalty"]),
        "--batch",
        "-t",
        text,
        "-o",
        str(output_path),
    ]
    if config["max_tokens"] is not None:
        cmd.extend(["--max-tokens", str(config["max_tokens"])])
    return cmd


def resolve_render_config(args: argparse.Namespace) -> dict[str, object]:
    if args.fidelity_preset == "high":
        render_path = args.render_path or "batch"
        segmentation = args.segmentation or ("per-line" if args.consistency_mode == "anchor-clone" else "per-sentence")
        if args.consistency_mode == "anchor-clone":
            temperature = 0.75 if args.temperature is None else args.temperature
            top_k = 16 if args.top_k is None else args.top_k
            top_p = 0.9 if args.top_p is None else args.top_p
        else:
            temperature = 0.0 if args.temperature is None else args.temperature
            top_k = 0 if args.top_k is None else args.top_k
            top_p = 1.0 if args.top_p is None else args.top_p
        repetition_penalty = 1.02 if args.repetition_penalty is None else args.repetition_penalty
        max_tokens = 320 if args.max_tokens is None else args.max_tokens
        if args.consistency_mode == "anchor-clone":
            segment_gap_ms = 750.0 if args.segment_gap_ms is None else args.segment_gap_ms
            fade_ms = 10.0 if args.fade_ms is None else args.fade_ms
        else:
            segment_gap_ms = 90.0 if args.segment_gap_ms is None else args.segment_gap_ms
            fade_ms = 12.0 if args.fade_ms is None else args.fade_ms
    else:
        render_path = args.render_path or "stream"
        segmentation = args.segmentation or "joined"
        temperature = 0.9 if args.temperature is None else args.temperature
        top_k = 75 if args.top_k is None else args.top_k
        top_p = 1.0 if args.top_p is None else args.top_p
        repetition_penalty = 1.02 if args.repetition_penalty is None else args.repetition_penalty
        max_tokens = args.max_tokens
        segment_gap_ms = 0.0 if args.segment_gap_ms is None else args.segment_gap_ms
        fade_ms = 0.0 if args.fade_ms is None else args.fade_ms

    return {
        "render_path": render_path,
        "segmentation": segmentation,
        "temperature": temperature,
        "top_k": top_k,
        "top_p": top_p,
        "repetition_penalty": repetition_penalty,
        "max_tokens": max_tokens,
        "segment_gap_ms": segment_gap_ms,
        "fade_ms": fade_ms,
    }


def build_stream_command(
    cli_path: Path,
    models_dir: Path,
    model_name: str,
    instruct: str,
    text: str,
    output_path: Path,
    config: dict[str, object],
    quiet: bool,
) -> list[str]:
    cmd = [
        str(cli_path),
        "-m",
        str(models_dir.resolve()),
        "--voice-design",
        "--model-name",
        model_name,
        "--voice-design-instruct",
        instruct,
        "--temperature",
        str(config["temperature"]),
        "--top-k",
        str(config["top_k"]),
        "--top-p",
        str(config["top_p"]),
        "--repetition-penalty",
        str(config["repetition_penalty"]),
        "-t",
        text,
        "-o",
        str(output_path),
    ]
    if config["max_tokens"] is not None:
        cmd.extend(["--max-tokens", str(config["max_tokens"])])
    if quiet:
        cmd.append("--quiet")
    return cmd


def build_batch_command(
    cli_path: Path,
    models_dir: Path,
    model_name: str,
    instruct: str,
    text: str,
    output_path: Path,
    config: dict[str, object],
) -> list[str]:
    cmd = [
        str(cli_path),
        "-m",
        str(models_dir.resolve()),
        "--model-name",
        model_name,
        "--instruction",
        instruct,
        "--temperature",
        str(config["temperature"]),
        "--top-k",
        str(config["top_k"]),
        "--top-p",
        str(config["top_p"]),
        "--repetition-penalty",
        str(config["repetition_penalty"]),
        "--batch",
        "-t",
        text,
        "-o",
        str(output_path),
    ]
    if config["max_tokens"] is not None:
        cmd.extend(["--max-tokens", str(config["max_tokens"])])
    return cmd


def run_segment(
    cli_path: Path,
    models_dir: Path,
    model_name: str,
    instruct: str,
    text: str,
    output_path: Path,
    config: dict[str, object],
    quiet: bool,
) -> None:
    if config["render_path"] == "stream":
        cmd = build_stream_command(
            cli_path=cli_path,
            models_dir=models_dir,
            model_name=model_name,
            instruct=instruct,
            text=text,
            output_path=output_path,
            config=config,
            quiet=quiet,
        )
    else:
        cmd = build_batch_command(
            cli_path=cli_path,
            models_dir=models_dir,
            model_name=model_name,
            instruct=instruct,
            text=text,
            output_path=output_path,
            config=config,
        )
    print("Running:", subprocess.list2cmdline(cmd))
    completed = subprocess.run(cmd, cwd=PROJECT_ROOT)
    if completed.returncode != 0:
        raise SystemExit(completed.returncode)


def run_clone_segment(
    cli_path: Path,
    models_dir: Path,
    model_name: str,
    speaker_embedding: Path,
    text: str,
    output_path: Path,
    config: dict[str, object],
) -> None:
    cmd = build_clone_batch_command(
        cli_path=cli_path,
        models_dir=models_dir,
        model_name=model_name,
        speaker_embedding=speaker_embedding,
        text=text,
        output_path=output_path,
        config=config,
    )
    print("Running:", subprocess.list2cmdline(cmd))
    completed = subprocess.run(cmd, cwd=PROJECT_ROOT)
    if completed.returncode != 0:
        raise SystemExit(completed.returncode)


def score_wav_for_selection(path: Path) -> dict[str, float]:
    report = detect_file(
        path=path,
        frame_ms=40.0,
        hop_ms=20.0,
        dft_n=256,
        decim=4,
        min_rms=0.008,
        region_gap_sec=0.14,
        halfsec_window_frames=25,
        top_n=5,
    )
    halfsec_scores = [row["mean_score"] for row in report["top_halfsec_centers"]]
    region_scores = [row["mean_score"] for row in report["top_suspect_regions"]]
    return {
        "worst_halfsec": max(halfsec_scores) if halfsec_scores else 0.0,
        "worst_region": max(region_scores) if region_scores else 0.0,
        "mean_top3_halfsec": (sum(halfsec_scores[:3]) / min(3, len(halfsec_scores))) if halfsec_scores else 0.0,
    }


def better_candidate(lhs: dict[str, float], rhs: dict[str, float]) -> bool:
    lhs_key = (lhs["worst_halfsec"], lhs["worst_region"], lhs["mean_top3_halfsec"])
    rhs_key = (rhs["worst_halfsec"], rhs["worst_region"], rhs["mean_top3_halfsec"])
    return lhs_key < rhs_key


def apply_edge_fades(samples: array, channels: int, fade_frames: int) -> None:
    if fade_frames <= 0:
        return
    total_frames = len(samples) // channels
    fade_frames = min(fade_frames, total_frames // 2)
    if fade_frames <= 0:
        return
    for frame_idx in range(fade_frames):
        head_gain = frame_idx / fade_frames
        tail_gain = (fade_frames - frame_idx) / fade_frames
        for channel in range(channels):
            head_index = frame_idx * channels + channel
            tail_index = (total_frames - fade_frames + frame_idx) * channels + channel
            samples[head_index] = int(samples[head_index] * head_gain)
            samples[tail_index] = int(samples[tail_index] * tail_gain)


def trim_silence_samples(samples: array, channels: int, threshold: float, pad_frames: int) -> array:
    if not samples:
        return samples
    total_frames = len(samples) // channels
    start_frame = 0
    end_frame = total_frames

    for frame_idx in range(total_frames):
        peak = 0.0
        for channel in range(channels):
            sample = abs(samples[frame_idx * channels + channel]) / 32768.0
            if sample > peak:
                peak = sample
        if peak >= threshold:
            start_frame = max(0, frame_idx - pad_frames)
            break
    else:
        return samples

    for frame_idx in range(total_frames - 1, -1, -1):
        peak = 0.0
        for channel in range(channels):
            sample = abs(samples[frame_idx * channels + channel]) / 32768.0
            if sample > peak:
                peak = sample
        if peak >= threshold:
            end_frame = min(total_frames, frame_idx + 1 + pad_frames)
            break

    if start_frame == 0 and end_frame == total_frames:
        return samples

    trimmed = array("h")
    trimmed.extend(samples[start_frame * channels : end_frame * channels])
    return trimmed


def concat_wavs(
    parts: list[Path],
    output_path: Path,
    gap_ms: float,
    fade_ms: float,
    trim_silence: bool,
    trim_threshold: float,
    trim_pad_ms: float,
    gap_after_ms: list[float] | None = None,
) -> list[dict[str, float | str]]:
    if not parts:
        raise ValueError("No WAV parts to concatenate.")

    metadata: list[dict[str, float | str]] = []
    sample_cursor = 0
    with contextlib.ExitStack() as stack:
        readers = [stack.enter_context(wave.open(str(path), "rb")) for path in parts]
        first = readers[0]
        params = (
            first.getnchannels(),
            first.getsampwidth(),
            first.getframerate(),
            first.getcomptype(),
            first.getcompname(),
        )
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(output_path), "wb") as out:
            out.setnchannels(params[0])
            out.setsampwidth(params[1])
            out.setframerate(params[2])
            out.setcomptype(params[3], params[4])
            fade_frames = max(0, int(params[2] * fade_ms / 1000.0))
            trim_pad_frames = max(0, int(params[2] * trim_pad_ms / 1000.0))

            for idx, (path, reader) in enumerate(zip(parts, readers)):
                current = (
                    reader.getnchannels(),
                    reader.getsampwidth(),
                    reader.getframerate(),
                    reader.getcomptype(),
                    reader.getcompname(),
                )
                if current != params:
                    raise SystemExit(
                        f"Intermediate WAV parameters do not match for concatenation: {path}"
                    )
                nframes = reader.getnframes()
                frames = reader.readframes(nframes)
                pcm = array("h")
                pcm.frombytes(frames)
                original_frames = len(pcm) // params[0]
                if trim_silence:
                    pcm = trim_silence_samples(
                        pcm,
                        channels=params[0],
                        threshold=trim_threshold,
                        pad_frames=trim_pad_frames,
                    )
                apply_edge_fades(pcm, params[0], fade_frames)
                out.writeframes(pcm.tobytes())
                written_frames = len(pcm) // params[0]
                start_sec = sample_cursor / params[2]
                end_sec = (sample_cursor + written_frames) / params[2]
                metadata.append(
                    {
                        "file": str(path),
                        "start_sec": round(start_sec, 3),
                        "end_sec": round(end_sec, 3),
                        "duration_sec": round(end_sec - start_sec, 3),
                        "source_duration_sec": round(original_frames / params[2], 3),
                    }
                )
                sample_cursor += written_frames
                next_gap_ms = gap_ms if gap_after_ms is None else gap_after_ms[idx]
                gap_frames = max(0, int(params[2] * next_gap_ms / 1000.0))
                silence = bytes(gap_frames * params[0] * params[1])
                if idx != len(parts) - 1 and gap_frames > 0:
                    out.writeframes(silence)
                    sample_cursor += gap_frames
    return metadata


def cleanup(paths: list[Path]) -> None:
    for path in paths:
        if path.exists():
            path.unlink()


def reset_temp_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def resolve_anchor_text(lines: list[str], anchor_line_index: int) -> str:
    if anchor_line_index < 1 or anchor_line_index > len(lines):
        raise SystemExit(
            f"--anchor-line-index must be between 1 and {len(lines)} for the provided input lines."
        )
    return lines[anchor_line_index - 1]


def extract_speaker_embedding(
    engine_cli: Path,
    models_dir: Path,
    base_model_name: str,
    input_wav: Path,
    output_json: Path,
    scratch_output: Path,
) -> None:
    cmd = [
        str(engine_cli),
        "-m",
        str(models_dir.resolve()),
        "--model-name",
        base_model_name,
        "-t",
        "Embedding extraction pass.",
        "--reference",
        str(input_wav),
        "--dump-speaker-embedding",
        str(output_json),
        "-o",
        str(scratch_output),
    ]
    print("Running:", subprocess.list2cmdline(cmd))
    completed = subprocess.run(cmd, cwd=PROJECT_ROOT)
    if completed.returncode != 0:
        raise SystemExit(completed.returncode)


def export_requested_embeddings(
    args: argparse.Namespace,
    engine_cli: Path,
    anchor_wav: Path,
    temp_dir: Path,
    anchor_json_1_7b: Path,
) -> None:
    if args.output_speaker_json_1_7b:
        out_1_7b = args.output_speaker_json_1_7b.resolve()
        out_1_7b.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(anchor_json_1_7b, out_1_7b)
        print(f"Wrote speaker embedding JSON: {out_1_7b}")

    if args.output_speaker_json_0_6b:
        out_0_6b = args.output_speaker_json_0_6b.resolve()
        out_0_6b.parent.mkdir(parents=True, exist_ok=True)
        scratch_0_6b = temp_dir / "anchor_embedding_0_6b_scratch.wav"
        extract_speaker_embedding(
            engine_cli=engine_cli,
            models_dir=args.models_dir,
            base_model_name=args.base_model_name_0_6b,
            input_wav=anchor_wav,
            output_json=out_0_6b,
            scratch_output=scratch_0_6b,
        )
        print(f"Wrote speaker embedding JSON: {out_0_6b}")


def render_anchor_clone(
    args: argparse.Namespace,
    lines: list[str],
    segment_entries: list[dict[str, object]],
    config: dict[str, object],
    output_path: Path,
    manifest_out: Path,
) -> int:
    engine_cli = resolve_engine_cli(args.exe)
    anchor_text = resolve_anchor_text(lines, args.anchor_line_index)
    temp_dir = output_path.parent / f"{output_path.stem}_segments"
    reset_temp_dir(temp_dir)

    anchor_wav = temp_dir / "anchor_voicedesign.wav"
    anchor_json = temp_dir / "anchor_embedding.json"
    anchor_scratch_wav = temp_dir / "anchor_embedding_scratch.wav"

    print(f"[anchor] line {args.anchor_line_index}/{len(lines)}")
    run_segment(
        cli_path=engine_cli,
        models_dir=args.models_dir,
        model_name=args.model_name,
        instruct=args.instruct,
        text=anchor_text,
        output_path=anchor_wav,
        config=config,
        quiet=args.quiet,
    )
    extract_speaker_embedding(
        engine_cli=engine_cli,
        models_dir=args.models_dir,
        base_model_name=args.base_model_name,
        input_wav=anchor_wav,
        output_json=anchor_json,
        scratch_output=anchor_scratch_wav,
    )
    export_requested_embeddings(
        args=args,
        engine_cli=engine_cli,
        anchor_wav=anchor_wav,
        temp_dir=temp_dir,
        anchor_json_1_7b=anchor_json,
    )

    temp_paths: list[Path] = []
    gap_after_ms: list[float] = []
    for idx, entry in enumerate(segment_entries, start=1):
        segment = str(entry["text"])
        print(f"[segment {idx}/{len(segment_entries)} line {entry['line_index']}] {segment}")
        seg_path = temp_dir / f"{idx:03d}.wav"
        run_clone_segment(
            cli_path=engine_cli,
            models_dir=args.models_dir,
            model_name=args.base_model_name,
            speaker_embedding=anchor_json,
            text=segment,
            output_path=seg_path,
            config=config,
        )
        temp_paths.append(seg_path)
        gap_after_ms.append(float(config["segment_gap_ms"]) if bool(entry["is_line_final"]) else 0.0)

    timeline = concat_wavs(
        temp_paths,
        output_path,
        gap_ms=float(config["segment_gap_ms"]),
        fade_ms=float(config["fade_ms"]),
        trim_silence=args.trim_silence,
        trim_threshold=args.trim_threshold,
        trim_pad_ms=args.trim_pad_ms,
        gap_after_ms=gap_after_ms,
    )
    manifest = {
        "output_wav": str(output_path),
        "fidelity_preset": args.fidelity_preset,
        "consistency_mode": args.consistency_mode,
        "render_path": "anchor-clone-base-batch",
        "segmentation": config["segmentation"],
        "segment_count": len(segment_entries),
        "anchor": {
            "line_index": args.anchor_line_index,
            "text": anchor_text,
            "voicedesign_wav": str(anchor_wav),
            "speaker_embedding_json": str(anchor_json),
        },
        "settings": {
            "temperature": config["temperature"],
            "top_k": config["top_k"],
            "top_p": config["top_p"],
            "repetition_penalty": config["repetition_penalty"],
            "max_tokens": config["max_tokens"],
            "segment_gap_ms": config["segment_gap_ms"],
            "fade_ms": config["fade_ms"],
            "trim_silence": args.trim_silence,
            "trim_threshold": args.trim_threshold,
            "trim_pad_ms": args.trim_pad_ms,
        },
        "segments": [
            {
                "text": str(entry["text"]),
                "line_index": int(entry["line_index"]),
                "is_line_final": bool(entry["is_line_final"]),
                "file": timing["file"],
                "start_sec": timing["start_sec"],
                "end_sec": timing["end_sec"],
                "duration_sec": timing["duration_sec"],
                "source_duration_sec": timing.get("source_duration_sec"),
            }
            for entry, timing in zip(segment_entries, timeline)
        ],
    }
    manifest_out.parent.mkdir(parents=True, exist_ok=True)
    manifest_out.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    if not args.keep_intermediates:
        shutil.rmtree(temp_dir)

    print(f"Wrote VoiceDesign WAV: {output_path}")
    print(f"Segments rendered: {len(segment_entries)}")
    print(f"Manifest: {manifest_out}")
    return 0


def main() -> int:
    args = parse_args()
    lines = load_lines(args)
    config = resolve_render_config(args)
    segments = resolve_segments(lines, str(config["segmentation"]))
    output_path = args.output.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    if str(config["render_path"]) == "stream":
        cli_path = resolve_streaming_cli(args.exe)
    else:
        cli_path = resolve_engine_cli(args.exe)

    manifest_out = args.manifest_out.resolve() if args.manifest_out else output_path.with_suffix(".segments.json")

    if args.consistency_mode == "anchor-clone":
        clone_segmentation = args.segmentation or "per-line"
        clone_segment_entries = resolve_segment_entries(lines, clone_segmentation)
        clone_config = dict(config)
        clone_config["segmentation"] = clone_segmentation
        return render_anchor_clone(
            args=args,
            lines=lines,
            segment_entries=clone_segment_entries,
            config=clone_config,
            output_path=output_path,
            manifest_out=manifest_out,
        )

    if len(segments) == 1:
        run_segment(
            cli_path=cli_path,
            models_dir=args.models_dir,
            model_name=args.model_name,
            instruct=args.instruct,
            text=segments[0],
            output_path=output_path,
            config=config,
            quiet=args.quiet,
        )
        manifest = {
            "output_wav": str(output_path),
            "fidelity_preset": args.fidelity_preset,
            "render_path": config["render_path"],
            "segmentation": config["segmentation"],
            "segment_count": 1,
            "settings": {
                "temperature": config["temperature"],
                "top_k": config["top_k"],
                "top_p": config["top_p"],
                "repetition_penalty": config["repetition_penalty"],
                "max_tokens": config["max_tokens"],
                "segment_gap_ms": config["segment_gap_ms"],
                "fade_ms": config["fade_ms"],
                "trim_silence": args.trim_silence,
                "trim_threshold": args.trim_threshold,
                "trim_pad_ms": args.trim_pad_ms,
            },
            "segments": [{"text": segments[0], "start_sec": 0.0, "end_sec": None}],
        }
    else:
        temp_dir = output_path.parent / f"{output_path.stem}_segments"
        reset_temp_dir(temp_dir)
        temp_paths: list[Path] = []
        candidate_metadata: list[dict[str, object]] = []
        for idx, segment in enumerate(segments, start=1):
            print(f"[segment {idx}/{len(segments)}] {segment}")
            seg_path = temp_dir / f"{idx:03d}.wav"
            best_score: dict[str, float] | None = None
            best_candidate_path: Path | None = None
            all_candidates: list[dict[str, object]] = []
            for take_idx in range(1, args.takes_per_segment + 1):
                print(f"  [take {take_idx}/{args.takes_per_segment}] rendering")
                candidate_path = seg_path if args.takes_per_segment == 1 else temp_dir / f"{idx:03d}_take{take_idx:02d}.wav"
                run_segment(
                    cli_path=cli_path,
                    models_dir=args.models_dir,
                    model_name=args.model_name,
                    instruct=args.instruct,
                    text=segment,
                    output_path=candidate_path,
                    config=config,
                    quiet=args.quiet,
                )
                score = score_wav_for_selection(candidate_path)
                all_candidates.append(
                    {
                        "take": take_idx,
                        "file": str(candidate_path),
                        "selection_score": score,
                    }
                )
                print(
                    "  "
                    f"[take {take_idx}/{args.takes_per_segment}] "
                    f"score worst_halfsec={score['worst_halfsec']:.3f} "
                    f"worst_region={score['worst_region']:.3f} "
                    f"mean_top3={score['mean_top3_halfsec']:.3f}"
                )
                if best_score is None or better_candidate(score, best_score):
                    best_score = score
                    best_candidate_path = candidate_path
            if best_candidate_path is None or best_score is None:
                raise SystemExit(f"Failed to select a candidate for segment {idx}.")
            if best_candidate_path != seg_path:
                best_candidate_path.replace(seg_path)
            print(
                "  "
                f"[selected] worst_halfsec={best_score['worst_halfsec']:.3f} "
                f"worst_region={best_score['worst_region']:.3f} "
                f"mean_top3={best_score['mean_top3_halfsec']:.3f}"
            )
            temp_paths.append(seg_path)
            candidate_metadata.append(
                {
                    "segment_index": idx,
                    "text": segment,
                    "selected_file": str(seg_path),
                    "selected_score": best_score,
                    "candidates": all_candidates,
                }
            )
            for candidate in all_candidates:
                candidate_file = Path(str(candidate["file"]))
                if candidate_file != seg_path and candidate_file.exists():
                    candidate_file.unlink()
        timeline = concat_wavs(
            temp_paths,
            output_path,
            gap_ms=float(config["segment_gap_ms"]),
            fade_ms=float(config["fade_ms"]),
            trim_silence=args.trim_silence,
            trim_threshold=args.trim_threshold,
            trim_pad_ms=args.trim_pad_ms,
        )
        manifest_segments = []
        for segment_text, timing in zip(segments, timeline):
            manifest_segments.append(
                {
                    "text": segment_text,
                    "file": timing["file"],
                    "start_sec": timing["start_sec"],
                    "end_sec": timing["end_sec"],
                    "duration_sec": timing["duration_sec"],
                }
            )
        manifest = {
            "output_wav": str(output_path),
            "fidelity_preset": args.fidelity_preset,
            "render_path": config["render_path"],
            "segmentation": config["segmentation"],
            "segment_count": len(segments),
            "settings": {
                "temperature": config["temperature"],
                "top_k": config["top_k"],
                "top_p": config["top_p"],
                "repetition_penalty": config["repetition_penalty"],
                "max_tokens": config["max_tokens"],
                "takes_per_segment": args.takes_per_segment,
                "segment_gap_ms": config["segment_gap_ms"],
                "fade_ms": config["fade_ms"],
                "trim_silence": args.trim_silence,
                "trim_threshold": args.trim_threshold,
                "trim_pad_ms": args.trim_pad_ms,
            },
            "segment_selection": candidate_metadata,
            "segments": manifest_segments,
        }
        if not args.keep_intermediates:
            shutil.rmtree(temp_dir)

    manifest_out.parent.mkdir(parents=True, exist_ok=True)
    manifest_out.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    print(f"Wrote VoiceDesign WAV: {output_path}")
    print(f"Segments rendered: {len(segments)}")
    print(f"Manifest: {manifest_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
