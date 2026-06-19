#!/usr/bin/env python3
from __future__ import annotations

import argparse
import cmath
import json
import math
import wave
from array import array
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def load_wav_mono_i16(path: Path) -> tuple[list[int], int]:
    with wave.open(str(path), "rb") as w:
        channels = w.getnchannels()
        sample_width = w.getsampwidth()
        sample_rate = w.getframerate()
        nframes = w.getnframes()
        raw = w.readframes(nframes)

    if sample_width != 2:
        raise ValueError(f"Unsupported sample width for {path}: {sample_width} bytes")

    vals = array("h")
    vals.frombytes(raw)
    samples = list(vals)

    if channels == 1:
        return samples, sample_rate

    mono: list[int] = []
    for i in range(0, len(samples), channels):
        frame = samples[i : i + channels]
        mono.append(int(sum(frame) / len(frame)))
    return mono, sample_rate


def detect_file(
    path: Path,
    frame_ms: float,
    hop_ms: float,
    dft_n: int,
    decim: int,
    min_rms: float,
    region_gap_sec: float,
    halfsec_window_frames: int,
    top_n: int,
) -> dict:
    samples, sample_rate = load_wav_mono_i16(path)
    frame = max(1, int(sample_rate * frame_ms / 1000.0))
    hop = max(1, int(sample_rate * hop_ms / 1000.0))
    win = [0.5 - 0.5 * math.cos(2.0 * math.pi * i / (dft_n - 1)) for i in range(dft_n)]
    prev_spec: list[float] | None = None

    frames: list[tuple[float, float, float, float, float, float]] = []
    for start in range(0, len(samples) - frame + 1, hop):
        seg = samples[start : start + frame]
        rms = math.sqrt(sum((s / 32768.0) * (s / 32768.0) for s in seg) / len(seg))

        ds = seg[::decim][:dft_n]
        if len(ds) < dft_n:
            ds += [0] * (dft_n - len(ds))
        ds = [(ds[i] / 32768.0) * win[i] for i in range(dft_n)]

        mags: list[float] = []
        ps: list[float] = []
        for k in range(dft_n // 2 + 1):
            accum = 0j
            for n0, value in enumerate(ds):
                accum += value * cmath.exp(-2j * math.pi * k * n0 / dft_n)
            mag = abs(accum) + 1e-12
            mags.append(mag)
            ps.append(mag * mag)

        ps_sum = sum(ps)
        freqs = [k * (sample_rate / decim) / dft_n for k in range(dft_n // 2 + 1)]
        centroid = sum(freq * p for freq, p in zip(freqs, ps)) / ps_sum
        flatness = math.exp(sum(math.log(p) for p in ps) / len(ps)) / (ps_sum / len(ps))
        hi_ratio = sum(p for freq, p in zip(freqs, ps) if freq >= 2500.0) / ps_sum
        flux = 0.0 if prev_spec is None else math.sqrt(sum((a - b) * (a - b) for a, b in zip(mags, prev_spec)) / len(mags))
        prev_spec = mags

        score = 0.0
        if rms >= min_rms:
            score = (
                2.0 * flatness
                + 0.15 * flux
                + 1.8 * hi_ratio
                + max(0.0, centroid - 1600.0) / 1600.0
                - 0.25 * rms
            )

        frames.append((start / sample_rate, score, rms, centroid, flatness, hi_ratio))

    voice_frames = [f for f in frames if f[2] >= min_rms]
    ranked = sorted(voice_frames, key=lambda item: item[1], reverse=True)
    threshold = ranked[min(30, len(ranked) - 1)][1] if ranked else 0.0

    regions: list[dict] = []
    current: dict | None = None
    region_frame_dur = frame / sample_rate
    for t, score, rms, centroid, flatness, hi_ratio in sorted(ranked, key=lambda item: item[0]):
        if score < threshold:
            continue
        if current is None or t - current["end"] > region_gap_sec:
            current = {
                "start": t,
                "end": t + region_frame_dur,
                "max_score": score,
                "sum_score": score,
                "sum_rms": rms,
                "sum_centroid": centroid,
                "sum_flatness": flatness,
                "sum_hi_ratio": hi_ratio,
                "count": 1,
            }
            regions.append(current)
        else:
            current["end"] = t + region_frame_dur
            current["max_score"] = max(current["max_score"], score)
            current["sum_score"] += score
            current["sum_rms"] += rms
            current["sum_centroid"] += centroid
            current["sum_flatness"] += flatness
            current["sum_hi_ratio"] += hi_ratio
            current["count"] += 1

    region_rows = []
    for region in regions:
        count = region["count"]
        region_rows.append(
            {
                "start_sec": round(region["start"], 2),
                "end_sec": round(region["end"], 2),
                "dur_sec": round(region["end"] - region["start"], 2),
                "mean_score": round(region["sum_score"] / count, 3),
                "max_score": round(region["max_score"], 3),
                "mean_rms": round(region["sum_rms"] / count, 4),
                "mean_centroid_hz": round(region["sum_centroid"] / count, 1),
                "mean_flatness": round(region["sum_flatness"] / count, 4),
                "mean_hi_ratio": round(region["sum_hi_ratio"] / count, 4),
            }
        )
    region_rows.sort(key=lambda row: row["dur_sec"] * row["mean_score"], reverse=True)

    smoothed: list[tuple[float, float, float, float]] = []
    for i, row in enumerate(frames):
        chunk = frames[max(0, i - halfsec_window_frames // 2) : min(len(frames), i + halfsec_window_frames // 2 + 1)]
        mean_score = sum(item[1] for item in chunk) / len(chunk)
        mean_rms = sum(item[2] for item in chunk) / len(chunk)
        mean_centroid = sum(item[3] for item in chunk) / len(chunk)
        smoothed.append((row[0], mean_score, mean_rms, mean_centroid))
    smoothed.sort(key=lambda item: item[1], reverse=True)

    return {
        "file": str(path),
        "duration_sec": round(len(samples) / sample_rate, 3),
        "frame_ms": frame_ms,
        "hop_ms": hop_ms,
        "voice_frames": len(voice_frames),
        "score_threshold": round(threshold, 6),
        "top_suspect_regions": region_rows[:top_n],
        "top_frames": [
            {
                "time_sec": round(t, 2),
                "score": round(score, 3),
                "rms": round(rms, 4),
                "centroid_hz": round(centroid, 1),
                "flatness": round(flatness, 4),
                "hi_ratio": round(hi_ratio, 4),
            }
            for t, score, rms, centroid, flatness, hi_ratio in ranked[:top_n]
        ],
        "top_halfsec_centers": [
            {
                "center_sec": round(t, 2),
                "mean_score": round(score, 3),
                "mean_rms": round(rms, 4),
                "mean_centroid_hz": round(centroid, 1),
            }
            for t, score, rms, centroid in smoothed[:top_n]
        ],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Heuristic detector for synthetic / metallic spans in WAV files.")
    parser.add_argument("paths", nargs="+", help="WAV files or glob patterns to analyze.")
    parser.add_argument("--json-out", type=Path, help="Optional path to write the full JSON report.")
    parser.add_argument("--top-n", type=int, default=10, help="How many regions/frames to keep per file.")
    return parser.parse_args()


def expand_paths(patterns: list[str]) -> list[Path]:
    out: list[Path] = []
    for pattern in patterns:
        p = Path(pattern)
        if any(ch in pattern for ch in "*?[]"):
            out.extend(sorted(PROJECT_ROOT.glob(pattern)))
        elif p.is_absolute():
            out.append(p)
        else:
            out.append((PROJECT_ROOT / p).resolve())
    unique: list[Path] = []
    seen: set[Path] = set()
    for p in out:
        rp = p.resolve()
        if rp not in seen and rp.exists():
            seen.add(rp)
            unique.append(rp)
    return unique


def print_summary(report: list[dict]) -> None:
    for item in report:
        print(f"\n{Path(item['file']).name}")
        print(f"  duration: {item['duration_sec']} s")
        print(f"  suspect threshold: {item['score_threshold']}")
        if item["top_halfsec_centers"]:
            centers = ", ".join(
                f"{row['center_sec']}s(score={row['mean_score']})" for row in item["top_halfsec_centers"][:5]
            )
            print(f"  worst half-second centers: {centers}")
        if item["top_suspect_regions"]:
            print("  top suspect regions:")
            for row in item["top_suspect_regions"][:5]:
                print(
                    f"    {row['start_sec']}-{row['end_sec']}s "
                    f"(score={row['mean_score']}, centroid={row['mean_centroid_hz']}Hz, "
                    f"flatness={row['mean_flatness']}, hi={row['mean_hi_ratio']})"
                )


def main() -> int:
    args = parse_args()
    paths = expand_paths(args.paths)
    if not paths:
        raise SystemExit("No matching WAV files found.")

    report = [
        detect_file(
            path=path,
            frame_ms=40.0,
            hop_ms=20.0,
            dft_n=256,
            decim=4,
            min_rms=0.008,
            region_gap_sec=0.14,
            halfsec_window_frames=25,
            top_n=args.top_n,
        )
        for path in paths
    ]

    print_summary(report)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"\nWrote JSON report: {args.json_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
