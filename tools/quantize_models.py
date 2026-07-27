#!/usr/bin/env python3
"""Create compact Qwen3-TTS GGUF variants with the bundled quantizer."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL = ROOT / "models" / "qwen3-tts-1.7b-base-f16.gguf"
QUANT_TYPES = ("q4_k", "q5_k")


def find_quantizer(explicit: Path | None) -> Path:
    candidates = [explicit] if explicit else [
        ROOT / "build-ninja-cuda" / "engine" / "tts_engine_quantize.exe",
        ROOT / "build-ninja-native" / "engine" / "tts_engine_quantize.exe",
        ROOT / "build-ninja-turing" / "engine" / "tts_engine_quantize.exe",
        ROOT / "build-vs2022-x64" / "engine" / "Release" / "tts_engine_quantize.exe",
        ROOT / "build" / "engine" / "tts_engine_quantize",
    ]
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate
    raise FileNotFoundError(
        "tts_engine_quantize was not found; build that CMake target or pass --quantizer"
    )


def output_path(source: Path, quant_type: str, output_dir: Path | None) -> Path:
    stem = source.stem
    if stem.lower().endswith("-f16"):
        stem = stem[:-4]
    return (output_dir or source.parent) / f"{stem}-{quant_type}.gguf"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate Q4_K/Q5_K GGUF files from an F16 Qwen3-TTS model."
    )
    parser.add_argument(
        "models",
        nargs="*",
        type=Path,
        default=[DEFAULT_MODEL],
        help=f"input F16 GGUF files (default: {DEFAULT_MODEL.relative_to(ROOT)})",
    )
    parser.add_argument(
        "--types",
        nargs="+",
        choices=QUANT_TYPES,
        default=list(QUANT_TYPES),
        help="quantization variants to create (default: q4_k q5_k)",
    )
    parser.add_argument("--output-dir", type=Path, help="output directory (default: beside input)")
    parser.add_argument("--quantizer", type=Path, help="path to tts_engine_quantize")
    parser.add_argument("--force", action="store_true", help="replace existing outputs")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        quantizer = find_quantizer(args.quantizer)
    except FileNotFoundError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)

    for source in args.models:
        source = source.resolve()
        if not source.is_file():
            print(f"error: model not found: {source}", file=sys.stderr)
            return 2
        for quant_type in args.types:
            destination = output_path(source, quant_type, args.output_dir)
            if destination.exists() and not args.force:
                print(f"skip: {destination} already exists")
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            print(f"create: {destination} ({quant_type})", flush=True)
            result = subprocess.run(
                [str(quantizer), str(source), str(destination), quant_type],
                check=False,
            )
            if result.returncode:
                destination.unlink(missing_ok=True)
                return result.returncode

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
