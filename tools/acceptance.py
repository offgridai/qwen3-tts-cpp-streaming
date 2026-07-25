#!/usr/bin/env python3
"""Run the lean Qwen3-TTS contract, streaming, fidelity, and reliability checks."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD = ROOT / "build-ninja-cuda"
DEFAULT_TEXT = "Use a reasonably long paragraph for this consistency test."


def executable(build_dir: Path, relative: str) -> Path:
    base = build_dir / relative
    candidates = [base.with_suffix(".exe"), base, base.parent / "Release" / base.with_suffix(".exe").name]
    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()
    raise SystemExit(f"Required executable is missing: {base}. Build the test and CLI targets first.")


def run(command: list[str], label: str) -> subprocess.CompletedProcess[str]:
    print(f"\n[{label}]")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.stdout:
        print(completed.stdout.rstrip())
    if completed.stderr:
        print(completed.stderr.rstrip(), file=sys.stderr)
    if completed.returncode != 0:
        raise RuntimeError(f"{label} failed with exit code {completed.returncode}")
    return completed


def last_json_object(text: str) -> dict[str, object]:
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            return json.loads(line)
    raise RuntimeError("Command did not emit a JSON metrics object")


def require_max(name: str, value: float | int | None, limit: float | int) -> None:
    if value is None or float(value) > float(limit):
        raise RuntimeError(f"{name}: expected <= {limit}, got {value}")
    print(f"PASS {name}: {value} <= {limit}")


def require_min(name: str, value: float | int | None, limit: float | int) -> None:
    if value is None or float(value) < float(limit):
        raise RuntimeError(f"{name}: expected >= {limit}, got {value}")
    print(f"PASS {name}: {value} >= {limit}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--full", action="store_true", help="Run the six-case, six-seed reliability corpus.")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--models-dir", type=Path, default=ROOT / "models")
    parser.add_argument("--model", default="qwen3-tts-0.6b-f16")
    parser.add_argument("--embedding", type=Path, default=ROOT / "reference" / "priestley_0.6b_f16.json")
    parser.add_argument("--limits", type=Path, default=ROOT / "tests" / "acceptance_limits.json")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "tools" / "acceptance_runs")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir.resolve()
    models_dir = args.models_dir.resolve()
    embedding = args.embedding.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    limits = json.loads(args.limits.read_text(encoding="utf-8"))

    wrapper_contract = executable(build_dir, "tests/qwen3_wrapper_contract_test")
    wrapper_model = executable(build_dir, "tests/qwen3_wrapper_model_test")
    streaming_cli = executable(build_dir, "apps/streaming_cli/qwen3_streaming_cli")

    run([str(wrapper_contract)], "wrapper contract")

    model_run = run([
        str(wrapper_model),
        "--models", str(models_dir),
        "--model", args.model,
        "--embedding", str(embedding),
        "--output-dir", str(output_dir),
    ], "model-backed wrapper contract")
    wrapper_metrics = last_json_object(model_run.stdout)
    require_max("cold model load (ms)", wrapper_metrics.get("load_ms"), limits["cold_model_load_ms_max"])
    require_max("resident first 350 ms", wrapper_metrics.get("first_350_ms"), limits["resident_first_350_ms_max"])
    require_max(
        "cold first 350 ms",
        float(wrapper_metrics["load_ms"]) + float(wrapper_metrics["first_350_ms"]),
        limits["cold_first_350_ms_max"],
    )
    require_max("wrapper max callback gap", wrapper_metrics.get("max_callback_gap_ms"), limits["max_callback_gap_ms_max"])
    require_max("wrapper RTF", wrapper_metrics.get("rtf"), limits["rtf_max"])

    callback_json = output_dir / "callback_metrics.json"
    run([
        sys.executable,
        str(ROOT / "tools" / "streaming_callback_benchmark.py"),
        "--exe", str(streaming_cli),
        "--models-dir", str(models_dir),
        "--model-name", args.model,
        "--input-json", str(embedding),
        "--seed", "42",
        "--text", DEFAULT_TEXT,
        "--output-json", str(callback_json),
    ], "streaming cadence")
    callback_metrics = json.loads(callback_json.read_text(encoding="utf-8"))
    require_max("benchmark first 350 ms", callback_metrics.get("first_full_buffer_ms"), limits["resident_first_350_ms_max"])
    require_max("benchmark maximum gap", callback_metrics.get("max_window_gap_ms"), limits["max_callback_gap_ms_max"])
    require_max("benchmark RTF", callback_metrics.get("rtf"), limits["rtf_max"])
    require_min(
        "minimum simulated headroom",
        callback_metrics.get("minimum_playback_headroom_ms"),
        limits["minimum_playback_headroom_ms_min"],
    )
    require_max(
        "simulated underruns",
        callback_metrics.get("simulated_playback_underruns"),
        limits["simulated_playback_underruns_max"],
    )

    fidelity_json = output_dir / "fidelity.json"
    contract_wav = output_dir / "wrapper_contract_a.wav"
    run([
        sys.executable,
        str(ROOT / "tools" / "detect_synthetic_spans.py"),
        str(contract_wav),
        "--json-out", str(fidelity_json),
    ], "fidelity heuristic")
    fidelity = json.loads(fidelity_json.read_text(encoding="utf-8"))[0]
    half_second_scores = [float(row["mean_score"]) for row in fidelity.get("top_halfsec_centers", [])]
    region_scores = [float(row["mean_score"]) for row in fidelity.get("top_suspect_regions", [])]
    require_max(
        "worst half-second score",
        max(half_second_scores, default=0.0),
        limits["worst_half_second_score_max"],
    )
    require_max(
        "worst suspect-region score",
        max(region_scores, default=0.0),
        limits["worst_region_score_max"],
    )

    regression_dir = output_dir / "regression"
    regression_command = [
        sys.executable,
        str(ROOT / "tools" / "streaming_regression_benchmark.py"),
        "--exe", str(streaming_cli),
        "--models-dir", str(models_dir),
        "--model-name", args.model,
        "--input-json", str(embedding),
        "--output-dir", str(regression_dir),
        "--timeout-sec", "120",
    ]
    if args.full:
        regression_command.extend(["--seeds", "40,41,42,43,44,45"])
    else:
        regression_command.extend(["--case", "short", "--seeds", "42"])
    run(regression_command, "reliability corpus" if args.full else "quick reliability")
    regression = json.loads((regression_dir / "report.json").read_text(encoding="utf-8"))
    summary = regression["summary"]
    require_max("absolute token limits", summary.get("token_limit"), 0)
    require_max("timeouts", summary.get("timeouts"), 0)
    require_max("process failures", summary.get("process_failures"), 0)

    report = {
        "mode": "full" if args.full else "quick",
        "wrapper": wrapper_metrics,
        "callback": callback_metrics,
        "fidelity": {
            "worst_half_second_score": max(half_second_scores, default=0.0),
            "worst_region_score": max(region_scores, default=0.0),
        },
        "reliability": summary,
        "limits": limits,
    }
    report_path = output_dir / "acceptance_report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"\nPASS: {'full' if args.full else 'quick'} acceptance suite")
    print(f"Report: {report_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
