#!/usr/bin/env python3
"""Run a seeded 0.6B streaming corpus and aggregate reliability/performance metrics."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import time
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXE = ROOT / "build-ninja-cuda" / "apps" / "streaming_cli" / "qwen3_streaming_cli.exe"
DEFAULT_CORPUS = [
    {"id": "short", "text": "Hello. It is good to hear from you."},
    {"id": "punctuation", "text": "Wait—really? Yes: exactly; that is the answer."},
    {"id": "numbers", "text": "Order 17 ships on July 24 at 3:45 in the afternoon."},
    {"id": "names", "text": "Amelia spoke with Javier and Siobhan in Reykjavík."},
    {"id": "long", "text": "I was not expecting visitors this late. What a pleasure it is to meet you here. Did you know I offer sandwiches for sale? Tell me about your favorite childhood memory."},
    {"id": "pauses", "text": "Well... I suppose we could try. But first, let us take a moment and think."},
]

TERMINATION_RE = re.compile(r"Generation termination:\s+(\w+)\s+\((\d+) audio tokens\)")
THROUGHPUT_RE = re.compile(r"([0-9.]+)x realtime")
WINDOW_GAP_RE = re.compile(r"max window gap ms:\s+(-?\d+)", re.IGNORECASE)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-json", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "tools" / "streaming_regression_runs")
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--models-dir", type=Path, default=ROOT / "models")
    parser.add_argument("--model-name", default="qwen3-tts-0.6b-f16")
    parser.add_argument("--corpus-json", type=Path)
    parser.add_argument("--case", action="append", help="Run only the named built-in/corpus case; repeatable.")
    parser.add_argument("--seeds", default="0,1,2,3,4", help="Comma-separated integer seeds.")
    parser.add_argument("--temperature", type=float, default=0.75)
    parser.add_argument("--top-k", type=int, default=16)
    parser.add_argument("--top-p", type=float, default=0.9)
    parser.add_argument("--repetition-penalty", type=float, default=1.02)
    parser.add_argument("--max-tokens", type=int, default=256)
    parser.add_argument("--timeout-sec", type=float, default=90.0)
    return parser.parse_args()


def load_corpus(args: argparse.Namespace) -> list[dict[str, str]]:
    corpus = DEFAULT_CORPUS
    if args.corpus_json:
        corpus = json.loads(args.corpus_json.read_text(encoding="utf-8"))
    if args.case:
        selected = set(args.case)
        corpus = [row for row in corpus if row["id"] in selected]
        missing = selected - {row["id"] for row in corpus}
        if missing:
            raise SystemExit(f"Unknown corpus case(s): {', '.join(sorted(missing))}")
    return corpus


def wav_duration(path: Path) -> float | None:
    if not path.exists():
        return None
    with wave.open(str(path), "rb") as wav:
        return wav.getnframes() / wav.getframerate()


def main() -> int:
    args = parse_args()
    corpus = load_corpus(args)
    seeds = [int(value.strip()) for value in args.seeds.split(",") if value.strip()]
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    runs: list[dict[str, object]] = []

    for case in corpus:
        for seed in seeds:
            wav_path = output_dir / f"{case['id']}_seed{seed}.wav"
            cmd = [
                str(args.exe.resolve()), "-m", str(args.models_dir.resolve()),
                "--model-identifier", args.model_name,
                "--speaker-embedding", str(args.input_json.resolve()),
                "--tts-profile", "offgrid-callback", "--no-play-streaming",
                "--temperature", str(args.temperature), "--top-k", str(args.top_k),
                "--top-p", str(args.top_p), "--repetition-penalty", str(args.repetition_penalty),
                "--max-tokens", str(args.max_tokens), "--seed", str(seed),
                "-t", case["text"], "-o", str(wav_path),
            ]
            started = time.perf_counter()
            timed_out = False
            try:
                completed = subprocess.run(
                    cmd, cwd=ROOT, text=True, capture_output=True, encoding="utf-8",
                    errors="replace", timeout=args.timeout_sec,
                )
                log = completed.stdout + completed.stderr
                returncode = completed.returncode
            except subprocess.TimeoutExpired as exc:
                log = (exc.stdout or "") + (exc.stderr or "")
                returncode = None
                timed_out = True
            wall_sec = time.perf_counter() - started
            termination = TERMINATION_RE.search(log)
            throughput = THROUGHPUT_RE.search(log)
            max_gap = WINDOW_GAP_RE.search(log)
            row = {
                "case": case["id"], "seed": seed, "returncode": returncode,
                "timed_out": timed_out, "wall_sec": round(wall_sec, 3),
                "termination": termination.group(1) if termination else "timeout" if timed_out else "unknown",
                "audio_tokens": int(termination.group(2)) if termination else None,
                "duration_sec": wav_duration(wav_path),
                "throughput_x_realtime": float(throughput.group(1)) if throughput else None,
                "max_window_gap_ms": int(max_gap.group(1)) if max_gap else None,
            }
            runs.append(row)
            print(json.dumps(row, separators=(",", ":")))

    eos_runs = [row for row in runs if row["termination"] == "eos"]
    numeric_throughput = [float(row["throughput_x_realtime"]) for row in runs if row["throughput_x_realtime"] is not None]
    summary = {
        "runs": len(runs),
        "natural_eos": len(eos_runs),
        "natural_eos_rate": len(eos_runs) / len(runs) if runs else 0.0,
        "token_limit": sum(row["termination"] == "token_limit" for row in runs),
        "safety_limit": sum(row["termination"] == "safety_limit" for row in runs),
        "timeouts": sum(bool(row["timed_out"]) for row in runs),
        "process_failures": sum(row["returncode"] not in (0, None) for row in runs),
        "mean_throughput_x_realtime": sum(numeric_throughput) / len(numeric_throughput) if numeric_throughput else None,
    }
    report = {
        "settings": {
            "model": args.model_name, "seeds": seeds, "temperature": args.temperature,
            "top_k": args.top_k, "top_p": args.top_p,
            "repetition_penalty": args.repetition_penalty, "max_tokens": args.max_tokens,
        },
        "corpus": corpus, "summary": summary, "runs": runs,
    }
    report_path = output_dir / "report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    print(f"Report: {report_path}")
    # The text-relative safety limit is an accepted bounded completion mode.
    # Fail only for absolute token exhaustion, timeout, or a failed process.
    return 0 if summary["token_limit"] == 0 and summary["timeouts"] == 0 and summary["process_failures"] == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
