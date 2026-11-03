#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tier0 measurements, load for every sample.

Each binary directory holds tier1-test, bench-jobdump and tier0-test of one
revision. Revisions alternate on one pinned CPU (BENCH_CPU
or the least busy CPU); the host is shared, so every sample records the
1-minute load average before and after it.

  bench         asm and compiled-C Tier0 cells (warm/cold rates), paired
  instructions  host user instructions/cycles of the same cells (perf stat)
  stores        tier0-test --bench-bundle-stores (per-store cost on a code page)
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/bench"))
import bench_engines as bench  # noqa: E402
import common  # noqa: E402
import harness_bench  # noqa: E402

CELLS = [("asm", p) for p in bench.SUITES["asm"][0]] + [
    ("csuite", p) for p in bench.SUITES["csuite"][0]
]


def revisions(args):
    return [tuple(item.split("=", 1)) for item in args.rev]


def cell_command(directory, suite, program, cpu):
    if suite == "asm":
        return (
            common.pin(cpu)
            + [str(directory / "tier1-test"), f"--bench=tier0:{program}"]
            + harness_bench.images()
        )
    return (
        common.pin(cpu)
        + [
            str(directory / "bench-jobdump"),
            "--run=tier0",
            f"--job={bench.job_file(program, 'slow')}",
            "--variants=0,1,1,1",
        ]
        + bench.images()
    )


def run_bench(args, cpu, data, save):
    for rep in range(args.rounds):
        order = revisions(args) if rep % 2 == 0 else revisions(args)[::-1]
        for suite, program in CELLS:
            for name, directory in order:
                bench.JOBDUMP = Path(directory) / "bench-jobdump"
                if suite == "asm":
                    s = harness_bench.sample(
                        str(Path(directory) / "tier1-test"), cpu, "tier0", program, 0
                    )
                else:
                    s = bench.harness_csuite_sample("tier0", program, cpu)
                s.update(suite=suite, repeat=rep, revision=name)
                data["samples"].append(s)
                save()


def run_instructions(args, cpu, data, save):
    for rep in range(args.rounds):
        order = revisions(args) if rep % 2 == 0 else revisions(args)[::-1]
        for suite, program in CELLS:
            for name, directory in order:
                before = common.loadavg()
                command = ["perf", "stat", "-x,", "-e", "instructions:u,cycles:u"] + cell_command(
                    Path(directory), suite, program, cpu
                )
                r = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                counters = {}
                for line in r.stderr.splitlines():
                    fields = line.split(",")
                    if len(fields) > 2 and fields[0].isdigit():
                        counters[fields[2]] = int(fields[0])
                data["samples"].append(
                    dict(
                        program=program,
                        suite=suite,
                        revision=name,
                        repeat=rep,
                        rc=r.returncode,
                        counters=counters,
                        loadavg_before=before,
                        loadavg_after=common.loadavg(),
                    )
                )
                save()


def run_stores(args, cpu, data, save):
    for rep in range(args.rounds):
        order = revisions(args) if rep % 2 == 0 else revisions(args)[::-1]
        for name, directory in order:
            command = common.pin(cpu) + [
                str(Path(directory) / "tier0-test"),
                "--bench-bundle-stores",
            ]
            before = os.getloadavg()
            r = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            data["stores"].append(
                dict(
                    revision=name,
                    repeat=rep,
                    command=command,
                    rc=r.returncode,
                    output=r.stdout,
                    loadavg_before=before,
                    loadavg_after=os.getloadavg(),
                )
            )
            save()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("kind", choices=["bench", "instructions", "stores"])
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument(
        "--rev",
        action="append",
        required=True,
        metavar="NAME=DIR",
        help="binary directory of one revision (repeat; alternated)",
    )
    ap.add_argument("--rounds", type=int, default=3)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    cpu, busy = common.pick_cpu()
    data = dict(kind=args.kind, cpu=cpu, busy=busy, revisions=args.rev, samples=[], stores=[])

    def save():
        (args.out / "measurements.json").write_text(json.dumps(data, indent=1) + "\n")

    {"bench": run_bench, "instructions": run_instructions, "stores": run_stores}[args.kind](
        args, cpu, data, save
    )
    save()


if __name__ == "__main__":
    main()
