#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL_NAME = "qwen3-tts-1.7b-base-f16.gguf"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract a reusable speaker embedding JSON from a WAV using the 1.7B base model."
    )
    parser.add_argument("--input-wav", type=Path, required=True, help="Reference WAV file.")
    parser.add_argument("--output-json", type=Path, required=True, help="Output speaker embedding JSON.")
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
        "--scratch-output",
        type=Path,
        default=PROJECT_ROOT / "examples" / "_embedding_extract_scratch.wav",
        help="Throwaway WAV path required by tts_engine_cli.",
    )
    parser.add_argument(
        "--scratch-text",
        default="Embedding extraction pass.",
        help="Throwaway text required by tts_engine_cli.",
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
    input_wav = args.input_wav.resolve()
    if not input_wav.exists():
        raise SystemExit(f"Reference WAV not found: {input_wav}")

    output_json = args.output_json.resolve()
    output_json.parent.mkdir(parents=True, exist_ok=True)

    scratch_output = args.scratch_output.resolve()
    scratch_output.parent.mkdir(parents=True, exist_ok=True)

    cli_path = resolve_engine_cli(args.exe)
    cmd = [
        str(cli_path),
        "-m",
        str(args.models_dir.resolve()),
        "--model-name",
        args.model_name,
        "-t",
        args.scratch_text,
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
        return completed.returncode

    print(f"Wrote speaker embedding JSON: {output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
