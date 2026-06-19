#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL_NAME = "qwen3-tts-1.7b-base-f16.gguf"
DEFAULT_TEST_LINE = "I was not expecting visitors this late. What a pleasure it is to meet you here."


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Synthesize a test line from a speaker embedding JSON using the 1.7B base model."
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


def main() -> int:
    args = parse_args()
    input_json = args.input_json.resolve()
    if not input_json.exists():
        raise SystemExit(f"Speaker embedding JSON not found: {input_json}")

    output_wav = args.output_wav.resolve()
    output_wav.parent.mkdir(parents=True, exist_ok=True)

    cli_path = resolve_engine_cli(args.exe)
    cmd = [
        str(cli_path),
        "-m",
        str(args.models_dir.resolve()),
        "--model-name",
        args.model_name,
        "--speaker-embedding",
        str(input_json),
        "-t",
        args.text,
        "-o",
        str(output_wav),
    ]

    print("Running:", subprocess.list2cmdline(cmd))
    completed = subprocess.run(cmd, cwd=PROJECT_ROOT)
    if completed.returncode != 0:
        return completed.returncode

    print(f"Wrote verification WAV: {output_wav}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
