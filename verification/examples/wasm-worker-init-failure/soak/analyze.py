#!/usr/bin/env python3
"""Summarize fixed-window resource medians and enforce predeclared soak bounds."""

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


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--window-seconds", required=True, type=int)
    parser.add_argument("--minimum-events", required=True, type=int)
    parser.add_argument("--duration-seconds", required=True, type=int)
    parser.add_argument("--sample-interval-seconds", required=True, type=int)
    return parser.parse_args()


def main():
    args = parse_args()
    if args.window_seconds <= 0 or args.duration_seconds <= 0:
        raise SystemExit("window and duration must be positive")
    if args.sample_interval_seconds <= 0:
        raise SystemExit("sample interval must be positive")
    if args.duration_seconds % args.window_seconds != 0:
        raise SystemExit("duration must be an integer multiple of the fixed window")
    if args.sample_interval_seconds >= args.window_seconds:
        raise SystemExit("sample interval must be smaller than the fixed window")
    with args.input.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise SystemExit("sample CSV is empty")
    required = {"elapsed_seconds", "events", *METRICS}
    missing = required.difference(rows[0])
    if missing:
        raise SystemExit(f"sample CSV lacks columns: {sorted(missing)}")

    all_values = []
    for row in rows:
        values = {name: int(row[name]) for name in required}
        all_values.append(values)

    previous_elapsed = None
    previous_events = None
    for values in all_values:
        elapsed = values["elapsed_seconds"]
        events = values["events"]
        if elapsed < 0:
            raise SystemExit("elapsed seconds must be non-negative")
        if previous_elapsed is not None and elapsed <= previous_elapsed:
            raise SystemExit("elapsed seconds are not strictly increasing")
        if previous_events is not None and events < previous_events:
            raise SystemExit("event count decreased between samples")
        previous_elapsed = elapsed
        previous_events = events

    parsed = []
    for values in all_values:
        if values["elapsed_seconds"] < args.duration_seconds:
            parsed.append(values)
    if not parsed:
        raise SystemExit("sample CSV has no rows inside the declared measured duration")
    terminal = all_values[-1]
    if terminal["elapsed_seconds"] != args.duration_seconds:
        raise SystemExit("sample CSV must end with exactly one duration-boundary sample")
    if terminal["events"] < args.minimum_events:
        raise SystemExit(
            f"observed {terminal['events']} events, expected at least {args.minimum_events}"
        )

    windows = {}
    for row in parsed:
        index = row["elapsed_seconds"] // args.window_seconds
        windows.setdefault(index, []).append(row)
    expected_windows = args.duration_seconds // args.window_seconds
    expected_samples = (
        args.window_seconds + args.sample_interval_seconds - 1
    ) // args.sample_interval_seconds
    minimum_samples = max(1, expected_samples - 1)
    windows = {index: group for index, group in windows.items() if index < expected_windows}
    for index in range(expected_windows):
        group = windows.get(index, [])
        if not group:
            raise SystemExit(f"fixed window {index} has no samples")
        if len(group) < minimum_samples:
            raise SystemExit(
                f"fixed window {index} has {len(group)} samples, "
                f"expected at least {minimum_samples}"
            )
        elapsed = [row["elapsed_seconds"] for row in group]
        if min(elapsed) > index * args.window_seconds + args.sample_interval_seconds or max(
            elapsed
        ) < (index + 1) * args.window_seconds - args.sample_interval_seconds:
            raise SystemExit(f"fixed window {index} is incomplete")
    if len(windows) < 2:
        raise SystemExit("need at least two fixed windows")

    summaries = []
    for index in sorted(windows):
        group = windows[index]
        summary = {
            "window": index,
            "start_seconds": index * args.window_seconds,
            "end_seconds": (index + 1) * args.window_seconds,
            "events": max(row["events"] for row in group),
        }
        for metric in METRICS:
            values = [row[metric] for row in group]
            summary[f"{metric}_median"] = int(statistics.median(values))
            summary[f"{metric}_max"] = max(values)
        summaries.append(summary)

    with args.output.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=summaries[0].keys())
        writer.writeheader()
        writer.writerows(summaries)

    first, last = summaries[0], summaries[-1]
    event_delta = last["events"] - first["events"]
    if event_delta <= 0:
        raise SystemExit("event count did not grow across measured windows")
    absolute_limits = {
        "fd_count": 8,
        "thread_count": 0,
        "server_memory_allocated": 32 * 1024 * 1024,
        "server_memory_heap_size": 32 * 1024 * 1024,
        "rss_kb": 64 * 1024,
        "rss_anon_kb": 64 * 1024,
        "pss_kb": 64 * 1024,
        "private_dirty_kb": 64 * 1024,
    }
    trend_limits = {
        "server_memory_allocated": 1 * 1024 * 1024,
        "server_memory_heap_size": 1 * 1024 * 1024,
        "rss_kb": 4 * 1024,
        "rss_anon_kb": 4 * 1024,
        "pss_kb": 4 * 1024,
        "private_dirty_kb": 4 * 1024,
    }
    failures = []
    # Use stable edge sequences rather than individual samples/windows. Trend is a least-squares
    # fit over every fixed-window median, normalized by that run's observed event count.
    edge_count = max(1, min(2, len(summaries) // 2))
    for metric, limit in absolute_limits.items():
        if any(summary[f"{metric}_median"] < 0 for summary in summaries):
            continue
        first_edge = statistics.median(
            summary[f"{metric}_median"] for summary in summaries[:edge_count]
        )
        last_edge = statistics.median(
            summary[f"{metric}_median"] for summary in summaries[-edge_count:]
        )
        growth = last_edge - first_edge
        if growth > limit:
            failures.append(f"{metric} stable-window median growth {growth} > {limit}")
        # A two-window smoke run has only one edge-to-edge segment, so normal allocator warm-up is
        # amplified by per-100-event normalization. Keep its absolute bounds, but reserve the
        # least-squares trend gate for the three-or-more windows used by formal runs.
        if metric in trend_limits and len(summaries) >= 3:
            xs = [summary["events"] for summary in summaries]
            ys = [summary[f"{metric}_median"] for summary in summaries]
            x_mean = statistics.mean(xs)
            y_mean = statistics.mean(ys)
            denominator = sum((value - x_mean) ** 2 for value in xs)
            slope = 0 if denominator == 0 else sum(
                (x - x_mean) * (y - y_mean) for x, y in zip(xs, ys)
            ) / denominator
            per_100 = max(0, slope) * 100
            if per_100 > trend_limits[metric]:
                failures.append(
                    f"{metric} window-regression trend {per_100:.1f}/100 events "
                    f"> {trend_limits[metric]}"
                )
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    trend = (
        "window_regression_per_100_events"
        if len(summaries) >= 3
        else "not_enforced_under_3_windows"
    )
    print(
        f"PASS samples={len(parsed)} windows={len(summaries)} events={terminal['events']} "
        f"last_window_events={parsed[-1]['events']} "
        f"stable_edge_windows={edge_count} trend={trend}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
