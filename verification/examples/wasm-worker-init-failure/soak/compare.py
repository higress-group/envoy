#!/usr/bin/env python3
"""Enforce recovery-run growth relative to its paired healthy control."""

import argparse
import csv
import statistics
import sys
from pathlib import Path


METRICS = (
    "fd_count",
    "thread_count",
    "server_memory_allocated",
    "server_memory_heap_size",
    "rss_kb",
    "rss_anon_kb",
    "pss_kb",
    "private_dirty_kb",
)


def read(path):
    with path.open(newline="", encoding="utf-8") as source:
        return [{key: int(value) for key, value in row.items()} for row in csv.DictReader(source)]


def growth_and_slope(rows, metric):
    edge_count = max(1, min(2, len(rows) // 2))
    first_edge = statistics.median(row[f"{metric}_median"] for row in rows[:edge_count])
    last_edge = statistics.median(row[f"{metric}_median"] for row in rows[-edge_count:])
    xs = [row["events"] for row in rows]
    ys = [row[f"{metric}_median"] for row in rows]
    x_mean = statistics.mean(xs)
    y_mean = statistics.mean(ys)
    denominator = sum((value - x_mean) ** 2 for value in xs)
    slope = 0 if denominator == 0 else sum(
        (x - x_mean) * (y - y_mean) for x, y in zip(xs, ys)
    ) / denominator
    return last_edge - first_edge, slope * 100


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--actual", required=True, type=Path)
    parser.add_argument("--control", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    actual = read(args.actual)
    control = read(args.control)
    if len(actual) < 2 or len(control) < 2:
        raise SystemExit("paired comparison requires at least two windows in each run")
    trend_enforced = len(actual) >= 3 and len(control) >= 3

    growth_limits = {
        "fd_count": 4,
        "thread_count": 0,
        "server_memory_allocated": 16 * 1024 * 1024,
        "server_memory_heap_size": 16 * 1024 * 1024,
        "rss_kb": 32 * 1024,
        "rss_anon_kb": 32 * 1024,
        "pss_kb": 32 * 1024,
        "private_dirty_kb": 32 * 1024,
    }
    trend_limits = {
        "server_memory_allocated": 512 * 1024,
        "server_memory_heap_size": 512 * 1024,
        "rss_kb": 2 * 1024,
        "rss_anon_kb": 2 * 1024,
        "pss_kb": 2 * 1024,
        "private_dirty_kb": 2 * 1024,
    }
    failures = []
    records = []
    for metric in METRICS:
        if any(row[f"{metric}_median"] < 0 for row in actual + control):
            continue
        actual_growth, actual_slope = growth_and_slope(actual, metric)
        control_growth, control_slope = growth_and_slope(control, metric)
        extra_growth = actual_growth - control_growth
        extra_slope = actual_slope - control_slope
        records.append(
            {
                "metric": metric,
                "actual_stable_growth": f"{actual_growth:.1f}",
                "control_stable_growth": f"{control_growth:.1f}",
                "extra_stable_growth": f"{extra_growth:.1f}",
                "actual_trend_per_100_events": f"{actual_slope:.1f}",
                "control_trend_per_100_events": f"{control_slope:.1f}",
                "extra_trend_per_100_events": f"{extra_slope:.1f}",
            }
        )
        if extra_growth > growth_limits[metric]:
            failures.append(
                f"{metric} recovery-minus-control stable growth {extra_growth:.1f} "
                f"> {growth_limits[metric]}"
            )
        if trend_enforced and metric in trend_limits and extra_slope > trend_limits[metric]:
            failures.append(
                f"{metric} recovery-minus-control trend {extra_slope:.1f}/100 events "
                f"> {trend_limits[metric]}"
            )
    with args.output.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=records[0].keys())
        writer.writeheader()
        writer.writerows(records)
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    trend_mode = "enforced" if trend_enforced else "recorded_only"
    print(f"PASS paired stable-window growth; per-100-event trends={trend_mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
