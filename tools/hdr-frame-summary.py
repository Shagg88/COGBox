"""Summarize an explicitly selected gameplay window from COGBox frame CSVs."""
import argparse
import csv
import json
import statistics
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summarize(path, start_frame, end_frame):
    selected = []
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            try:
                frame = int(row["frame"])
                milliseconds = float(row["frame_ms"])
                if frame < start_frame or frame > end_frame or milliseconds <= 0:
                    continue
                selected.append(row)
            except (ValueError, TypeError, KeyError):
                continue  # A running game's final CSV row may not be complete yet.
    if not selected:
        raise ValueError("No complete frames in the requested measurement window")
    times = [float(row["frame_ms"]) for row in selected]
    gpu = [float(row["our_gpu_ms"]) for row in selected if float(row["our_gpu_ms"]) > 0]
    cpu = [float(row["renderer_own_cpu_ms"]) for row in selected]
    return {
        "csv": str(path),
        "first_frame": int(selected[0]["frame"]),
        "last_frame": int(selected[-1]["frame"]),
        "rows": len(selected),
        "elapsed_s": float(selected[-1]["time_s"]) - float(selected[0]["time_s"]),
        "mean_ms": statistics.mean(times),
        "median_ms": statistics.median(times),
        "p95_ms": percentile(times, 0.95),
        "p99_ms": percentile(times, 0.99),
        "max_ms": max(times),
        "mean_fps": 1000 / statistics.mean(times),
        "mean_frame_change_ms": statistics.mean([abs(b - a) for a, b in zip(times, times[1:])]) if len(times) > 1 else 0,
        "over_33ms": sum(value > 33.3333 for value in times),
        "over_50ms": sum(value > 50 for value in times),
        "stall_rows": sum(row["spike"] == "2" for row in selected),
        "renderer_cpu_mean_ms": statistics.mean(cpu),
        "renderer_gpu_mean_ms": statistics.mean(gpu) if gpu else None,
        "renderer_gpu_samples": len(gpu),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--start-frame", type=int, required=True)
    parser.add_argument("--end-frame", type=int, required=True)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    if arguments.start_frame > arguments.end_frame:
        parser.error("start-frame must not exceed end-frame")
    result = summarize(arguments.csv, arguments.start_frame, arguments.end_frame)
    text = json.dumps(result, indent=2)
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
