#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Paired Tier0 sizing, emission and native-region measurements."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--current", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument(
        "--case",
        choices=("control", "home-first", "home-second", "home-immediate", "home-extended"),
        required=True,
    )
    parser.add_argument("--first-reg", type=int, choices=(8, 32, 48, 96), default=48)
    parser.add_argument("--insns", type=int, choices=range(1, 49), default=48)
    parser.add_argument("--entry-offset", type=int, choices=(0, 16, 32, 48), default=0)
    parser.add_argument("--mask", type=int, choices=range(16), default=0)
    parser.add_argument("--cpu", type=int, default=min(os.sched_getaffinity(0)))
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--measure-count", type=int, default=10000)
    parser.add_argument("--emit-count", type=int, default=10000)
    parser.add_argument("--run-count", type=int, default=1000000)
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="use objects already built with both STATS flags set to 0",
    )
    args = parser.parse_args()
    if min(args.repeats, args.measure_count, args.emit_count, args.run_count) <= 0:
        parser.error("counts must be positive")
    if args.cpu not in os.sched_getaffinity(0):
        parser.error("CPU is outside the current affinity set")
    # Import the existing object-linking helper by exact path; this also
    # works under python3 -I without adding the checkout to sys.path.
    path = Path(__file__).resolve().with_name("codegen_bench.py")
    spec = importlib.util.spec_from_file_location("tier0_codegen_link", path)
    linker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(linker)
    driver = path.with_name("destination-home-bench.c")
    samples = {"baseline": [], "current": []}
    with tempfile.TemporaryDirectory(prefix="tier0-destination-home-") as temp:
        programs = {name: Path(temp) / name for name in samples}
        for name, root in (("baseline", args.baseline), ("current", args.current)):
            linker.binary(root.resolve(), driver, programs[name], not args.no_build)
        os.sched_setaffinity(0, {args.cpu})
        command = [
            args.case,
            str(args.mask),
            str(args.first_reg),
            str(args.measure_count),
            str(args.emit_count),
            str(args.run_count),
            str(args.insns),
            str(args.entry_offset),
        ]
        for program in programs.values():
            subprocess.run([str(program), *command], check=True, stdout=subprocess.DEVNULL)
        for repeat in range(args.repeats):
            for name in tuple(samples) if repeat % 2 == 0 else tuple(reversed(samples)):
                output = subprocess.check_output([str(programs[name]), *command], text=True)
                samples[name].append(json.loads(output))
    if len({run["checksum"] for runs in samples.values() for run in runs}) != 1:
        raise RuntimeError("final native execution checksums differ between checkouts")
    keys = ("bytes", "measure_ns_per_region", "emit_ns_per_region", "run_ns_per_instruction")
    summary = {}
    for name, runs in samples.items():
        if len({run["bytes"] for run in runs}) != 1:
            raise RuntimeError("code size changed across identical emissions")
        summary[name] = {key: statistics.median(run[key] for run in runs) for key in keys}
    print(
        json.dumps(
            {
                "case": args.case,
                "first_reg": args.first_reg,
                "host_isa_mask": args.mask,
                "cpu": args.cpu,
                "platform": platform.platform(),
                "insns": args.insns,
                "entry_offset": args.entry_offset,
                "counts": {
                    "repeats": args.repeats,
                    "measure": args.measure_count,
                    "emit": args.emit_count,
                    "run": args.run_count,
                },
                "note": "Synthetic native regions; excludes lookup, publication and dispatch.",
                "summary": summary,
                "samples": samples,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
