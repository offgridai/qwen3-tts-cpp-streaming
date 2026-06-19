#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from detect_synthetic_spans import detect_file
from speaker_embedding_smoke_test import STREAMING_PRESETS, resolve_engine_cli


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL_NAME = "qwen3-tts-0.6b-f16.gguf"
DEFAULT_TEXT = (
    "I was not expecting visitors this late. What a pleasure it is to meet you here. "
    "I'm so sorry to hear about your loss. Its heartbreaking."
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render batch and silent streaming A/B outputs from one speaker embedding, then score them."
    )
    parser.add_argument("--input-json", type=Path, required=True, help="Speaker embedding JSON file.")
    parser.add_argument("--output-dir", type=Path, required=True, help="Directory for WAVs and report JSON.")
    parser.add_argument("--text", default=DEFAULT_TEXT, help="Text used for the A/B run.")
    parser.add_argument(
        "--models-dir",
        type=Path,
        default=PROJECT_ROOT / "models",
        help="Models directory. Default: ./models",
    )
    parser.add_argument("--exe", type=Path, help="Optional explicit path to tts_engine_cli(.exe).")
    parser.add_argument(
        "--model-name",
        default=DEFAULT_MODEL_NAME,
        help=f"Base model name. Default: {DEFAULT_MODEL_NAME}",
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
    parser.add_argument(
        "--presets",
        nargs="+",
        choices=tuple(STREAMING_PRESETS.keys()),
        default=["default", "fidelity", "fidelity-plus"],
        help="Streaming presets to compare against batch.",
    )
    return parser.parse_args()


def build_base_command(args: argparse.Namespace, cli_path: Path, input_json: Path, output_wav: Path) -> list[str]:
    return [
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


def summarize_detector(path: Path) -> dict[str, object]:
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
        "file": str(path),
        "duration_sec": report["duration_sec"],
        "worst_halfsec": max((row["mean_score"] for row in halfsec), default=0.0),
        "worst_region": max((row["mean_score"] for row in regions), default=0.0),
        "mean_top3_halfsec": (
            sum(row["mean_score"] for row in halfsec[:3]) / min(3, len(halfsec))
            if halfsec
            else 0.0
        ),
        "top_regions": regions[:3],
    }


def main() -> int:
    args = parse_args()
    input_json = args.input_json.resolve()
    if not input_json.exists():
        raise SystemExit(f"Speaker embedding JSON not found: {input_json}")

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    cli_path = resolve_engine_cli(args.exe)

    runs: list[dict[str, object]] = []

    batch_wav = output_dir / "batch.wav"
    batch_cmd = build_base_command(args, cli_path, input_json, batch_wav) + ["--batch"]
    print("Running:", subprocess.list2cmdline(batch_cmd))
    if subprocess.run(batch_cmd, cwd=PROJECT_ROOT).returncode != 0:
        return 1
    runs.append({"case": "batch", "mode": "batch", "preset": None, **summarize_detector(batch_wav)})

    for preset in args.presets:
        wav_path = output_dir / f"stream_{preset}.wav"
        cmd = build_base_command(args, cli_path, input_json, wav_path)
        cmd.extend(["--streaming-generate", "--no-play-streaming"])
        cmd.extend(STREAMING_PRESETS[preset])
        print("Running:", subprocess.list2cmdline(cmd))
        if subprocess.run(cmd, cwd=PROJECT_ROOT).returncode != 0:
            return 1
        runs.append({"case": f"stream_{preset}", "mode": "stream", "preset": preset, **summarize_detector(wav_path)})

    report = {
        "input_json": str(input_json),
        "model_name": args.model_name,
        "text": args.text,
        "settings": {
            "temperature": args.temperature,
            "top_k": args.top_k,
            "top_p": args.top_p,
            "repetition_penalty": args.repetition_penalty,
        },
        "runs": runs,
    }
    report_path = output_dir / "report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print("\nSummary")
    for row in runs:
        print(
            f"  {row['case']}: duration={row['duration_sec']}s "
            f"worst_halfsec={row['worst_halfsec']:.3f} "
            f"worst_region={row['worst_region']:.3f} "
            f"mean_top3_halfsec={row['mean_top3_halfsec']:.3f}"
        )
    print(f"\nReport: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
