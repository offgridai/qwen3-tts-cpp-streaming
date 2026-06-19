#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL_NAME = "qwen3-tts-1.7b-voicedesign-f16"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a single WAV from multiple English lines using the 1.7B VoiceDesign model."
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
        help="Optional explicit path to qwen3_streaming_cli(.exe).",
    )
    parser.add_argument(
        "--model-name",
        default=DEFAULT_MODEL_NAME,
        help=f"VoiceDesign model name. Default: {DEFAULT_MODEL_NAME}",
    )
    parser.add_argument("--temperature", type=float, default=0.9)
    parser.add_argument("--top-k", type=int, default=75)
    parser.add_argument("--top-p", type=float, default=1.0)
    parser.add_argument("--repetition-penalty", type=float, default=1.02)
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Pass --quiet to the underlying CLI.",
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


def resolve_streaming_cli(explicit: Path | None) -> Path:
    if explicit:
        if explicit.exists():
            return explicit.resolve()
        raise SystemExit(f"Streaming CLI not found: {explicit}")

    candidates = [
        PROJECT_ROOT / "build-vs2022-x64" / "apps" / "streaming_cli" / "Release" / "qwen3_streaming_cli.exe",
        PROJECT_ROOT / "build-vs2022-x64" / "apps" / "streaming_cli" / "qwen3_streaming_cli.exe",
        PROJECT_ROOT / "build-ninja-cuda" / "apps" / "streaming_cli" / "qwen3_streaming_cli.exe",
        PROJECT_ROOT / "build-ninja-verify" / "apps" / "streaming_cli" / "qwen3_streaming_cli.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()
    raise SystemExit(
        "Could not find qwen3_streaming_cli.exe. Pass --exe or build the streaming CLI first."
    )


def main() -> int:
    args = parse_args()
    lines = load_lines(args)
    output_path = args.output.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    text = " ".join(lines)
    cli_path = resolve_streaming_cli(args.exe)

    cmd = [
        str(cli_path),
        "-m",
        str(args.models_dir.resolve()),
        "--voice-design",
        "--model-name",
        args.model_name,
        "--voice-design-instruct",
        args.instruct,
        "--temperature",
        str(args.temperature),
        "--top-k",
        str(args.top_k),
        "--top-p",
        str(args.top_p),
        "--repetition-penalty",
        str(args.repetition_penalty),
        "-t",
        text,
        "-o",
        str(output_path),
    ]
    if args.quiet:
        cmd.append("--quiet")

    print("Running:", subprocess.list2cmdline(cmd))
    completed = subprocess.run(cmd, cwd=PROJECT_ROOT)
    if completed.returncode != 0:
        return completed.returncode

    print(f"Wrote VoiceDesign WAV: {output_path}")
    print(f"Input lines: {len(lines)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
