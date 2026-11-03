#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Paired process measurements of an already-built Tier1 phase benchmark.

Both binaries must use the same benchmark driver and build flags. This
runner only executes them; it does not modify either checkout or build.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, default=Path.cwd())
    parser.add_argument("--phase", choices=("fold", "isel", "entry", "i1", "known"), required=True)
    parser.add_argument(
        "--case", action="append", required=True, help="operation count:repetitions"
    )
    parser.add_argument("--cpu", type=int, required=True)
    parser.add_argument("--runs", type=int, default=9)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    cases = [tuple(map(int, case.split(":"))) for case in args.case]
    if any(len(case) != 2 or min(case) < 1 for case in cases):
        parser.error("each --case needs two positive integers")
    os.sched_setaffinity(0, {args.cpu})
    roots = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    binary = Path("build") / ("tier1-" + args.phase + "-bench")
    metric = {
        "fold": "ns_per_fold",
        "isel": "warm_ns",
        "entry": "ns_per_lookup",
        "i1": "ns_per_fold",
        "known": "ns_per_analysis",
    }[args.phase]
    records = []
    for count, repetitions in cases:
        for run in range(args.runs):
            order = ("baseline", "candidate") if run % 2 == 0 else ("candidate", "baseline")
            for variant in order:
                root = roots[variant]
                command = [str(root / binary), str(count), str(repetitions)]
                output = subprocess.check_output(command, cwd=root, text=True)
                for line in output.splitlines():
                    if not line.startswith(args.phase + "-bench "):
                        raise RuntimeError("unexpected benchmark output: " + line)
                    fields = dict(item.split("=", 1) for item in line.split()[1:])
                    records.append(
                        dict(
                            variant=variant,
                            run=run,
                            count=count,
                            repetitions=repetitions,
                            mode=fields.get("mode", "chain"),
                            ns=float(fields[metric]),
                            checksum=fields["checksum"],
                            fields=fields,
                        )
                    )
    summary = []
    for count, repetitions in cases:
        modes = sorted({row["mode"] for row in records if row["count"] == count})
        for mode in modes:
            rows = [row for row in records if row["count"] == count and row["mode"] == mode]
            assert len(rows) == 2 * args.runs
            assert len({row["checksum"] for row in rows}) == 1, (count, mode, "checksum mismatch")
            samples = {
                variant: [row["ns"] for row in rows if row["variant"] == variant]
                for variant in roots
            }
            medians = {variant: statistics.median(values) for variant, values in samples.items()}
            paired = [
                100 * (new / old - 1) for old, new in zip(samples["baseline"], samples["candidate"])
            ]
            summary.append(
                dict(
                    count=count,
                    repetitions=repetitions,
                    mode=mode,
                    median_ns=medians,
                    time_change_pct=100 * (medians["candidate"] / medians["baseline"] - 1),
                    median_paired_change_pct=statistics.median(paired),
                    paired_change_pct=paired,
                )
            )
    sources = {}
    for variant, root in roots.items():
        sources[variant] = {
            str(path): hashlib.sha256((root / path).read_bytes()).hexdigest()
            for path in (
                Path("engine/tier1/ir.c"),
                Path("engine/tier1/ir.h"),
                Path("engine/tier1/opt.c"),
                Path("engine/tier1/isel.c"),
            )
        }
    print(
        json.dumps(
            dict(
                phase=args.phase,
                cpu=args.cpu,
                runs=args.runs,
                metric=metric,
                platform=platform.platform(),
                source_sha256=sources,
                summary=summary,
                records=records,
            ),
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
