#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run and aggregate the IR1 hot decoder versus fast-cache benchmark."""

import argparse
import csv
import statistics
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


FLOAT_FIELDS = (
    "hot_tick_ns",
    "hot_mono_ns",
    "fast_lookup_tick_ns",
    "fast_lookup_mono_ns",
    "fast_emit_tick_ns",
    "fast_emit_mono_ns",
    "latxs_hot_on_tick_ns",
    "latxs_hot_on_mono_ns",
    "latxs_hot_off_tick_ns",
    "latxs_hot_off_mono_ns",
)


def parse_output(output):
    rows = []
    for line in output.splitlines():
        if not line.startswith("BENCH\t"):
            continue
        fields = line.split("\t")
        if fields[1] == "case":
            continue
        if len(fields) != 16:
            raise RuntimeError(f"unexpected benchmark columns: {len(fields)}")
        row = {
            "case": fields[1],
            "class": fields[2],
            "mode": int(fields[3]),
            "bytes": fields[4],
            "checksum": fields[15],
        }
        for name, value in zip(FLOAT_FIELDS, fields[5:15]):
            row[name] = float(value)
        rows.append(row)
    if not rows:
        raise RuntimeError("benchmark produced no BENCH rows")
    return rows


def median(values):
    return statistics.median(values)


def fmt(value):
    return f"{value:.3f}"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    if args.rounds < 1:
        raise SystemExit("--rounds must be positive")
    if not args.binary.is_file():
        raise SystemExit(f"benchmark binary not found: {args.binary}")

    args.output.mkdir(parents=True, exist_ok=True)
    all_rows = []
    for round_index in range(1, args.rounds + 1):
        proc = subprocess.run(
            [str(args.binary)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        (args.output / f"run-{round_index}.log").write_text(proc.stdout)
        if proc.returncode != 0:
            raise SystemExit(
                f"benchmark round {round_index} failed: {proc.returncode}"
            )
        rows = parse_output(proc.stdout)
        for row in rows:
            row["round"] = round_index
        all_rows.extend(rows)

    grouped = defaultdict(list)
    for row in all_rows:
        grouped[row["case"]].append(row)

    expected_cases = set(grouped)
    checksums = {}
    for case, rows in grouped.items():
        if len(rows) != args.rounds:
            raise RuntimeError(f"{case}: expected {args.rounds} rows")
        values = {row["checksum"] for row in rows}
        if len(values) != 1:
            raise RuntimeError(f"{case}: checksum changed across rounds: {values}")
        checksums[case] = values.pop()

    summary = []
    for case, rows in sorted(grouped.items()):
        item = {
            "case": case,
            "class": rows[0]["class"],
            "mode": rows[0]["mode"],
            "bytes": rows[0]["bytes"],
            "checksum": checksums[case],
        }
        for field in FLOAT_FIELDS:
            values = [row[field] for row in rows]
            item[f"{field}_median"] = median(values)
            item[f"{field}_min"] = min(values)
            item[f"{field}_max"] = max(values)
        item["fast_lookup_vs_hot_ratio"] = (
            item["fast_lookup_mono_ns_median"] /
            item["hot_mono_ns_median"]
        )
        item["fast_lookup_minus_hot_ns"] = (
            item["fast_lookup_mono_ns_median"] -
            item["hot_mono_ns_median"]
        )
        item["latxs_hot_on_minus_off_ns"] = (
            item["latxs_hot_on_mono_ns_median"] -
            item["latxs_hot_off_mono_ns_median"]
        )
        summary.append(item)

    if len(expected_cases) != 15:
        raise RuntimeError(f"expected 15 cases, got {len(expected_cases)}")

    raw_fields = ["round", "case", "class", "mode", "bytes", "checksum",
                  *FLOAT_FIELDS]
    with (args.output / "raw.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=raw_fields,
                                extrasaction="ignore")
        writer.writeheader()
        writer.writerows(sorted(all_rows, key=lambda row: (row["case"],
                                                           row["round"])))

    summary_fields = list(summary[0])
    with (args.output / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=summary_fields)
        writer.writeheader()
        writer.writerows(summary)

    lines = [
        "# IR1 hot decoder vs fast cache microbenchmark",
        "",
        f"Binary: `{args.binary}`",
        f"Rounds: {args.rounds}",
        "",
        "| Case | Class | Mode | Bytes | Hot ns | Fast lookup ns | Fast emit ns | Fast/Hot | Latxs hot-on ns | Latxs hot-off ns |",
        "|---|---|---:|---|---:|---:|---:|---:|---:|---:|",
    ]
    for item in summary:
        lines.append(
            "| {case} | {class} | {mode} | `{bytes}` | {hot} | {lookup} | "
            "{emit} | {ratio:.3f}x | {on} | {off} |".format(
                case=item["case"],
                **{"class": item["class"]},
                mode=item["mode"],
                bytes=item["bytes"],
                hot=fmt(item["hot_mono_ns_median"]),
                lookup=fmt(item["fast_lookup_mono_ns_median"]),
                emit=fmt(item["fast_emit_mono_ns_median"]),
                ratio=item["fast_lookup_vs_hot_ratio"],
                on=fmt(item["latxs_hot_on_mono_ns_median"]),
                off=fmt(item["latxs_hot_off_mono_ns_median"]),
            )
        )
    lines.extend([
        "",
        "Times are medians of `CLOCK_MONOTONIC_RAW` per-call measurements.",
        "`rdtime.d` results are retained in `summary.csv` for cross-checking.",
    ])
    (args.output / "report.md").write_text("\n".join(lines) + "\n")
    print(f"results written to {args.output}")


if __name__ == "__main__":
    main()
