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
# One fio job per profile, so its queue depth is the total concurrency.
profiles = ("qd1", "qd8", "qd32", "qd64", "qd128", "qd256")
queue_depth = {
    "qd1": 1,
    "qd8": 8,
    "qd32": 32,
    "qd64": 64,
    "qd128": 128,
    "qd256": 256,
}
# Low depth measures per-command overhead, where the work pool must not cost
# more than the single-vCPU baseline. High depth is where fan-out has to pay
# off: the gate there is a multiple of the baseline, not parity with it.
minimum_baseline_ratio = {
    "qd1": 0.95,
    "qd8": 0.95,
    "qd32": 0.95,
    "qd64": 2.00,
    "qd128": 4.00,
    "qd256": 8.00,
}


def threshold_summary():
    """Describe minimum_baseline_ratio without repeating equal thresholds."""
    groups = []
    for profile in profiles:
        if profile not in minimum_baseline_ratio:
            continue
        ratio = minimum_baseline_ratio[profile]
        if groups and groups[-1][0] == ratio:
            groups[-1][2] = profile
        else:
            groups.append([ratio, profile, profile])
    return "; ".join(
        f"`{first}`-`{last}` >= {ratio:.0%}" if first != last else f"`{first}` >= {ratio:.0%}"
        for ratio, first, last in groups
    )


failures = []
# Check-run annotations are public; job logs and artifacts are not.
highlights = []

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
    f"`workpoolSize={os.getenv('WORKPOOL_SIZE', 'unknown')}`, per-vCPU command queues and in-place completions",
    f"- Build type: `{os.getenv('BUILD_TYPE', 'unknown')}` for both versions",
    "- Runner: 4 vCPUs, 3 of them given to the backend's work pool through its `overlaybd.json`",
    "- I/O: 4 KiB random reads from the image's largest allocated data extent, `libaio`, `O_DIRECT`; "
    "one fio job per profile, all concurrency from `iodepth`, which the profile name gives",
    f"- Required current/baseline IOPS: {threshold_summary()}",
    "",
    "Both versions ran sequentially on the same GitHub runner against the same prewarmed file cache. "
    "Values are the median of three 15-second fio runs, each with a 3-second ramp that is not measured.",
    "",
    "| fio profile | queue depth | baseline IOPS | current IOPS | baseline ratio | IOPS change | baseline latency | current latency | latency change |",
    "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
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
            f"| `{profile}` | {queue_depth[profile]} | "
            f"{baseline_iops:,.0f} | {current_iops:,.0f} | "
            f"{iops_ratio:.1%} | {delta(current_iops, baseline_iops):+.1f}% | "
            f"{baseline_lat:,.1f} us | "
            f"{current_lat:,.1f} us | {delta(current_lat, baseline_lat):+.1f}% |"
        )
        highlights.append(
            f"{profile} iops {baseline_iops:,.0f} -> {current_iops:,.0f} "
            f"baseline ratio {iops_ratio:.2f}, "
            f"latency {baseline_lat:,.0f} us -> {current_lat:,.0f} us"
        )
        if profile in minimum_baseline_ratio and iops_ratio < minimum_baseline_ratio[profile]:
            failures.append(
                f"{profile} current/baseline IOPS is {iops_ratio:.1%}; "
                f"required >= {minimum_baseline_ratio[profile]:.0%}"
            )
    else:
        baseline_iops = f"{median(rows, 'baseline', profile, 'iops'):,.0f}" if baseline_runs else "timeout/no data"
        current_iops = f"{median(rows, 'current', profile, 'iops'):,.0f}" if current_runs else "timeout/no data"
        baseline_lat = f"{median(rows, 'baseline', profile, 'clat_us'):,.1f} us" if baseline_runs else "—"
        current_lat = f"{median(rows, 'current', profile, 'clat_us'):,.1f} us" if current_runs else "—"
        lines.append(
            f"| `{profile}` | {queue_depth[profile]} | "
            f"{baseline_iops} | {current_iops} | — | — | "
            f"{baseline_lat} | {current_lat} | — |"
        )
        failures.append(f"{profile} has incomplete benchmark data")

summary = "\n".join(lines) + "\n"
(result_dir / "summary.md").write_text(summary)
print(summary)

if highlights:
    # A plain factor, since '%' must be percent-escaped in workflow commands.
    print("::notice title=TCMU performance summary::" + "; ".join(highlights))

if failures:
    for failure in failures:
        print(f"::error title=TCMU performance regression::{failure}")
    sys.exit(1)
