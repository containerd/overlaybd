#!/usr/bin/env python3

import csv
import json
import os
import re
import statistics
import sys
from pathlib import Path


def load_runs(result_dir: Path):
    rows = []
    for path in sorted(result_dir.glob("*.json")):
        match = re.fullmatch(r"(baseline|current)-(.+)-(\d+)", path.stem)
        if not match:
            continue
        label, profile, run = match.groups()
        with path.open() as file:
            job = json.load(file)["jobs"][0]["read"]
        rows.append(
            {
                "label": label,
                "profile": profile,
                "run": int(run),
                "iops": float(job["iops"]),
                "bw_bytes": float(job["bw_bytes"]),
                "clat_us": float(job["clat_ns"]["mean"]) / 1000,
            }
        )
    return rows


def median(rows, label, profile, metric):
    return statistics.median(
        row[metric]
        for row in rows
        if row["label"] == label and row["profile"] == profile
    )


def delta(current, baseline):
    return (current / baseline - 1) * 100


result_dir = Path(sys.argv[1])
result_dir.mkdir(parents=True, exist_ok=True)
rows = load_runs(result_dir)
profiles = ("qd1-j1", "qd32-j8")
minimum_iops_ratio = {
    "qd1-j1": 0.90,
    "qd32-j8": 3.50,
}
failures = []

if rows:
    with (result_dir / "raw-results.csv").open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)

lines = [
    "# OverlayBD TCMU performance comparison",
    "",
    f"- Baseline: `{os.getenv('BASELINE_SHA', 'unknown')}` with its checked-in configuration",
    f"- Current: `{os.getenv('GITHUB_SHA', 'working tree')}` with "
    "`workpoolSize=8` and batched dispatcher migration across WorkPool vCPUs",
    f"- Build type: `{os.getenv('BUILD_TYPE', 'unknown')}` for both versions",
    "- I/O: 4 KiB random reads, `libaio`, `O_DIRECT`; profile names encode queue depth and job count",
    "- Required current/baseline IOPS: `qd1-j1 >= 90%`, `qd32-j8 >= 350%`",
    "",
    "Both versions ran sequentially on the same GitHub runner. Values are the median of three 15-second fio runs after a 45-second cache warmup.",
    "",
    "| fio profile | baseline IOPS | current IOPS | baseline ratio | IOPS change | baseline latency | current latency | latency change |",
    "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
]
for profile in profiles:
    baseline_runs = [row for row in rows if row["label"] == "baseline" and row["profile"] == profile]
    current_runs = [row for row in rows if row["label"] == "current" and row["profile"] == profile]
    if baseline_runs and current_runs:
        baseline_iops = median(rows, "baseline", profile, "iops")
        current_iops = median(rows, "current", profile, "iops")
        baseline_lat = median(rows, "baseline", profile, "clat_us")
        current_lat = median(rows, "current", profile, "clat_us")
        iops_ratio = current_iops / baseline_iops
        lines.append(
            f"| `{profile}` | {baseline_iops:,.0f} | {current_iops:,.0f} | "
            f"{iops_ratio:.1%} | {delta(current_iops, baseline_iops):+.1f}% | "
            f"{baseline_lat:,.1f} us | "
            f"{current_lat:,.1f} us | {delta(current_lat, baseline_lat):+.1f}% |"
        )
        if iops_ratio < minimum_iops_ratio[profile]:
            failures.append(
                f"{profile} current/baseline IOPS is {iops_ratio:.1%}; "
                f"required >= {minimum_iops_ratio[profile]:.0%}"
            )
    else:
        baseline_iops = f"{median(rows, 'baseline', profile, 'iops'):,.0f}" if baseline_runs else "timeout/no data"
        current_iops = f"{median(rows, 'current', profile, 'iops'):,.0f}" if current_runs else "timeout/no data"
        baseline_lat = f"{median(rows, 'baseline', profile, 'clat_us'):,.1f} us" if baseline_runs else "—"
        current_lat = f"{median(rows, 'current', profile, 'clat_us'):,.1f} us" if current_runs else "—"
        lines.append(
            f"| `{profile}` | {baseline_iops} | {current_iops} | — | — | "
            f"{baseline_lat} | {current_lat} | — |"
        )
        failures.append(f"{profile} has incomplete benchmark data")

summary = "\n".join(lines) + "\n"
(result_dir / "summary.md").write_text(summary)
print(summary)

if failures:
    for failure in failures:
        print(f"::error title=TCMU performance regression::{failure}")
    sys.exit(1)
