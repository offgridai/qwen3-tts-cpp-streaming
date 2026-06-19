#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STREAMING_BUILD = [
    PROJECT_ROOT / "build-ninja-cuda" / "apps" / "streaming_cli" / "qwen3_streaming_cli.exe",
    PROJECT_ROOT / "build-ninja-verify" / "apps" / "streaming_cli" / "qwen3_streaming_cli.exe",
    PROJECT_ROOT / "build-vs2022-x64" / "apps" / "streaming_cli" / "Release" / "qwen3_streaming_cli.exe",
    PROJECT_ROOT / "build-vs2022-x64" / "apps" / "streaming_cli" / "qwen3_streaming_cli.exe",
]

DELIVER_RE = re.compile(
    r"^\[deliver\]\s+chunk_index=(?P<chunk>\d+)\s+samples=(?P<samples>\d+)\s+audio_ms=(?P<audio_ms>[0-9.]+)\s+wall_ms_since_request=(?P<wall_ms>\d+)",
    re.MULTILINE,
)
INT_METRIC_RE = re.compile(r"^\s*(?P<label>[^:]+):\s+(?P<value>-?\d+)(?:\s*ms)?\s*$", re.MULTILINE)
FLOAT_METRIC_RE = re.compile(r"^\s*(?P<label>[^:]+):\s+(?P<value>[0-9.]+)x realtime", re.MULTILINE)
RTF_RE = re.compile(r"RTF=(?P<value>[0-9.]+)")
CADENCE_RE = {
    "second_window_gap_ms": re.compile(r"second window gap ms:\s+(?P<value>-?\d+)", re.IGNORECASE),
    "max_window_gap_ms": re.compile(r"max window gap ms:\s+(?P<value>-?\d+)", re.IGNORECASE),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark callback-buffer startup and sustained streaming speed using the silent callback simulation path."
    )
    parser.add_argument("--text", required=True, help="Transcript text to synthesize.")
    parser.add_argument(
        "--input-json",
        type=Path,
        required=True,
        help="Speaker embedding JSON used for the run.",
    )
    parser.add_argument(
        "--output-json",
        type=Path,
        help="Optional JSON file for the parsed metrics.",
    )
    parser.add_argument(
        "--exe",
        type=Path,
        help="Optional explicit path to qwen3_streaming_cli.exe.",
    )
    parser.add_argument(
        "--models-dir",
        type=Path,
        default=PROJECT_ROOT / "models",
        help="Models directory. Default: ./models",
    )
    parser.add_argument(
        "--model-name",
        default="qwen3-tts-0.6b-f16",
        help="Streaming model identifier. Default: qwen3-tts-0.6b-f16",
    )
    parser.add_argument(
        "--tts-profile",
        default="offgrid-callback",
        help="Streaming profile alias. Default: offgrid-callback",
    )
    parser.add_argument(
        "--target-buffer-ms",
        type=float,
        default=350.0,
        help="Target downstream callback buffer size. Default: 350 ms",
    )
    parser.add_argument(
        "--extra-args",
        nargs="*",
        default=[],
        help="Optional additional args passed through to qwen3_streaming_cli.",
    )
    return parser.parse_args()


def resolve_streaming_cli(explicit: Path | None) -> Path:
    if explicit:
        if explicit.exists():
            return explicit.resolve()
        raise SystemExit(f"Streaming CLI not found: {explicit}")
    for candidate in DEFAULT_STREAMING_BUILD:
        if candidate.exists():
            return candidate.resolve()
    raise SystemExit("Could not find qwen3_streaming_cli.exe. Pass --exe or build the streaming CLI first.")


def parse_metrics(log_text: str, target_buffer_ms: float) -> dict[str, object]:
    deliveries: list[dict[str, float | int]] = []
    delivered_ms = 0.0
    first_full_buffer_ms: float | None = None
    for match in DELIVER_RE.finditer(log_text):
        audio_ms = float(match.group("audio_ms"))
        wall_ms = int(match.group("wall_ms"))
        delivered_ms += audio_ms
        deliveries.append(
            {
                "chunk_index": int(match.group("chunk")),
                "samples": int(match.group("samples")),
                "audio_ms": audio_ms,
                "wall_ms_since_request": wall_ms,
                "cumulative_audio_ms": round(delivered_ms, 1),
            }
        )
        if first_full_buffer_ms is None and delivered_ms >= target_buffer_ms:
            first_full_buffer_ms = float(wall_ms)

    metrics: dict[str, object] = {
        "target_buffer_ms": target_buffer_ms,
        "first_full_buffer_ms": first_full_buffer_ms,
        "delivered_chunks": len(deliveries),
        "deliveries": deliveries,
    }

    for match in INT_METRIC_RE.finditer(log_text):
        label = match.group("label").strip().lower().replace(" ", "_")
        metrics[label] = int(match.group("value"))

    xrt_match = FLOAT_METRIC_RE.search(log_text)
    if xrt_match:
        metrics["throughput_x_realtime"] = float(xrt_match.group("value"))
    rtf_match = RTF_RE.search(log_text)
    if rtf_match:
        metrics["rtf"] = float(rtf_match.group("value"))
    for key, pattern in CADENCE_RE.items():
        match = pattern.search(log_text)
        if match:
            metrics[key] = int(match.group("value"))
    return metrics


def main() -> int:
    args = parse_args()
    cli_path = resolve_streaming_cli(args.exe)
    input_json = args.input_json.resolve()
    if not input_json.exists():
        raise SystemExit(f"Speaker embedding JSON not found: {input_json}")

    output_wav = PROJECT_ROOT / "examples" / "callback_benchmark_probe.wav"
    cmd = [
        str(cli_path),
        "-m",
        str(args.models_dir.resolve()),
        "--model-identifier",
        args.model_name,
        "--speaker-embedding",
        str(input_json),
        "--tts-profile",
        args.tts_profile,
        "--simulate-stream-callback",
        "--no-play-streaming",
        "--dump-streaming-overlap",
        "-t",
        args.text,
        "-o",
        str(output_wav),
    ]
    cmd.extend(args.extra_args)

    completed = subprocess.run(
        cmd,
        cwd=PROJECT_ROOT,
        text=True,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        print(completed.stdout)
        print(completed.stderr)
        return completed.returncode

    log_text = completed.stdout + completed.stderr
    metrics = parse_metrics(log_text, args.target_buffer_ms)
    metrics["command"] = subprocess.list2cmdline(cmd)

    print(f"Throughput: {metrics.get('throughput_x_realtime', 'n/a')}x realtime")
    print(f"RTF: {metrics.get('rtf', 'n/a')}")
    print(f"First paced chunk: {metrics.get('first_paced_chunk')} ms")
    print(f"First full {args.target_buffer_ms:.0f} ms buffer: {metrics.get('first_full_buffer_ms')} ms")
    print(f"Second window gap: {metrics.get('second_window_gap_ms')} ms")
    print(f"Max window gap: {metrics.get('max_window_gap_ms')} ms")
    print(f"Max paced gap: {metrics.get('max_paced_gap')} ms")

    if args.output_json:
        output_json = args.output_json.resolve()
        output_json.parent.mkdir(parents=True, exist_ok=True)
        output_json.write_text(json.dumps(metrics, indent=2), encoding="utf-8")
        print(f"Metrics JSON: {output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
