#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from detect_synthetic_spans import detect_file


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL_NAME = "qwen3-tts-1.7b-base-f16.gguf"
DEFAULT_TEST_LINE = "I was not expecting visitors this late. What a pleasure it is to meet you here."

STREAMING_PRESETS: dict[str, list[str]] = {
    "default": [],
    "fidelity": [
        "--first-tail-window-frames", "3",
        "--ramp-tail-window-frames", "8",
        "--ramp-tail-window-count", "1",
        "--steady-tail-window-frames", "12",
        "--context-frames", "4",
        "--early-context-frames", "3",
        "--early-context-window-count", "2",
        "--final-context-frames", "6",
        "--no-paced-audio-delivery",
        "--steady-split-decode-frames", "0",
    ],
    "fidelity-plus": [
        "--first-tail-window-frames", "4",
        "--ramp-tail-window-frames", "10",
        "--ramp-tail-window-count", "1",
        "--steady-tail-window-frames", "14",
        "--context-frames", "5",
        "--early-context-frames", "4",
        "--early-context-window-count", "2",
        "--final-context-frames", "8",
        "--no-paced-audio-delivery",
        "--steady-split-decode-frames", "0",
    ],
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Synthesize a verification line from a speaker embedding JSON via batch or silent streaming decode."
    )
    parser.add_argument("--input-json", type=Path, required=True, help="Speaker embedding JSON file.")
    parser.add_argument("--output-wav", type=Path, required=True, help="Output verification WAV.")
    parser.add_argument(
        "--text",
        default=DEFAULT_TEST_LINE,
        help="Test line used to verify the preserved voice.",
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
        help="Optional explicit path to tts_engine_cli(.exe).",
    )
    parser.add_argument(
        "--model-name",
        default=DEFAULT_MODEL_NAME,
        help=f"Base model name. Default: {DEFAULT_MODEL_NAME}",
    )
    parser.add_argument(
        "--render-mode",
        choices=("batch", "stream"),
        default="batch",
        help="batch uses normal synthesis. stream uses the silent incremental streaming decode path.",
    )
    parser.add_argument(
        "--streaming-preset",
        choices=tuple(STREAMING_PRESETS.keys()),
        default="fidelity",
        help="Streaming overlap/window preset. Ignored in batch mode.",
    )
    parser.add_argument("--temperature", type=float, default=0.75, help="Sampling temperature. Default: 0.75")
    parser.add_argument("--top-k", type=int, default=16, help="Top-k sampling. Default: 16")
    parser.add_argument("--top-p", type=float, default=0.9, help="Top-p sampling. Default: 0.9")
    parser.add_argument(
        "--repetition-penalty",
        type=float,
        default=1.02,
        help="Repetition penalty. Default: 1.02",
    )
    parser.add_argument("--max-tokens", type=int, help="Optional max token cap.")
    parser.add_argument(
        "--analyze",
        action="store_true",
        help="Run the synthetic-span detector on the generated WAV and print a short summary.",
    )
    return parser.parse_args()


def resolve_engine_cli(explicit: Path | None) -> Path:
    if explicit:
        if explicit.exists():
            return explicit.resolve()
        raise SystemExit(f"Engine CLI not found: {explicit}")

    candidates = [
        PROJECT_ROOT / "build-vs2022-x64" / "engine" / "Release" / "tts_engine_cli.exe",
        PROJECT_ROOT / "build-vs2022-x64" / "engine" / "tts_engine_cli.exe",
        PROJECT_ROOT / "build-ninja-cuda" / "engine" / "tts_engine_cli.exe",
        PROJECT_ROOT / "build-ninja-verify" / "engine" / "tts_engine_cli.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()
    raise SystemExit("Could not find tts_engine_cli.exe. Pass --exe or build the engine CLI first.")


def build_command(args: argparse.Namespace, cli_path: Path, input_json: Path, output_wav: Path) -> list[str]:
    cmd = [
        str(cli_path),
        "-m",
        str(args.models_dir.resolve()),
        "--model-name",
        args.model_name,
        "--speaker-embedding",
        str(input_json),
        "--temperature",
        str(args.temperature),
        "--top-k",
        str(args.top_k),
        "--top-p",
        str(args.top_p),
        "--repetition-penalty",
        str(args.repetition_penalty),
        "-t",
        args.text,
        "-o",
        str(output_wav),
    ]
    if args.max_tokens is not None:
        cmd.extend(["--max-tokens", str(args.max_tokens)])
    if args.render_mode == "batch":
        cmd.append("--batch")
    else:
        cmd.extend(["--streaming-generate", "--no-play-streaming"])
        cmd.extend(STREAMING_PRESETS[args.streaming_preset])
    return cmd


def analyze_wav(path: Path) -> dict[str, object]:
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
    halfsec = report["top_halfsec_centers"]
    regions = report["top_suspect_regions"]
    return {
        "duration_sec": report["duration_sec"],
        "worst_halfsec": max((row["mean_score"] for row in halfsec), default=0.0),
        "worst_region": max((row["mean_score"] for row in regions), default=0.0),
        "top_halfsec_centers": halfsec[:3],
        "top_regions": regions[:3],
    }


def main() -> int:
    args = parse_args()
    input_json = args.input_json.resolve()
    if not input_json.exists():
        raise SystemExit(f"Speaker embedding JSON not found: {input_json}")

    output_wav = args.output_wav.resolve()
    output_wav.parent.mkdir(parents=True, exist_ok=True)

    cli_path = resolve_engine_cli(args.exe)
    cmd = build_command(args, cli_path, input_json, output_wav)

    print("Running:", subprocess.list2cmdline(cmd))
    completed = subprocess.run(cmd, cwd=PROJECT_ROOT)
    if completed.returncode != 0:
        return completed.returncode

    print(f"Wrote verification WAV: {output_wav}")
    if args.analyze:
        summary = analyze_wav(output_wav)
        print("Detector summary:")
        print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
