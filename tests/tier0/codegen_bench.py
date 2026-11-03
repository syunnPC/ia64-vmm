#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Paired synthetic Tier0 measurements with one driver and two checkouts."""

import argparse
import json
import os
from pathlib import Path
import shlex
import statistics
import subprocess
import tempfile


def binary(root, driver, output, build):
    flags = ["TIER0_STATS=0", "TIER1_STATS=0"]
    if build:
        subprocess.run(
            ["make", "-j2", *flags, "build/tier0-test"],
            cwd=root,
            check=True,
            stdout=subprocess.DEVNULL,
        )
    dry = subprocess.check_output(["make", "-Bn", *flags, "build/tier0-test"], cwd=root, text=True)
    commands = [shlex.split(line) for line in dry.replace("\\\n", " ").splitlines() if line.strip()]
    main = next(c for c in commands if "-c" in c and "tests/tier0/main.c" in c)
    link = next(
        c for c in reversed(commands) if "-o" in c and c[c.index("-o") + 1] == "build/tier0-test"
    )
    compile_flags = main[1 : main.index("-c")]
    compile_flags = [flag for flag in compile_flags if flag not in ("-MMD", "-MP")]
    arguments = []
    for index, argument in enumerate(link[1:], 1):
        if argument == "-o" or (index > 1 and link[index - 1] == "-o"):
            continue
        if argument.endswith("/tests/tier0/main.o") or argument.startswith("-Wl,--wrap="):
            continue
        arguments.append(argument)
    subprocess.run(
        [link[0], *compile_flags, str(driver), *arguments, "-o", str(output)], cwd=root, check=True
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--current", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument(
        "--case",
        choices=("compact", "extensions", "immediates", "extract", "stacked", "control"),
        required=True,
    )
    parser.add_argument("--mask", type=int, choices=range(16), default=0)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--emit-count", type=int, default=20000)
    parser.add_argument("--run-count", type=int, default=1000000)
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="use objects already built with both STATS flags set to 0",
    )
    args = parser.parse_args()
    if min(args.repeats, args.emit_count, args.run_count) <= 0:
        parser.error("counts must be positive")
    driver = Path(__file__).resolve().with_name("codegen-bench.c")
    samples = {"baseline": [], "current": []}
    with tempfile.TemporaryDirectory(prefix="tier0-codegen-") as temp:
        programs = {name: Path(temp) / name for name in samples}
        for name, root in (("baseline", args.baseline), ("current", args.current)):
            binary(root.resolve(), driver, programs[name], not args.no_build)
        cpu = min(os.sched_getaffinity(0))
        os.sched_setaffinity(0, {cpu})
        command = [args.case, str(args.mask), str(args.emit_count), str(args.run_count)]
        for program in programs.values():
            subprocess.run([str(program), *command], check=True, stdout=subprocess.DEVNULL)
        for repeat in range(args.repeats):
            for name in tuple(samples) if repeat % 2 == 0 else tuple(reversed(samples)):
                output = subprocess.check_output([str(programs[name]), *command], text=True)
                samples[name].append(json.loads(output))
    summary = {}
    if len({run["checksum"] for runs in samples.values() for run in runs}) != 1:
        raise RuntimeError("final native execution states differ between checkouts")
    for name, runs in samples.items():
        if len({run["bytes"] for run in runs}) != 1:
            raise RuntimeError("code size changed across identical emissions")
        summary[name] = {
            key: statistics.median(run[key] for run in runs)
            for key in ("bytes", "emit_ns_per_region", "run_ns_per_instruction")
        }
    print(
        json.dumps(
            {
                "case": args.case,
                "host_isa_mask": args.mask,
                "cpu": cpu,
                "note": "Synthetic 48-instruction regions; execution excludes lookup/dispatch.",
                "summary": summary,
                "samples": samples,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
