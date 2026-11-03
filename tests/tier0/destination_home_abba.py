#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare Tier0 emission and native execution in one process with ABBA blocks.

Dependencies and ABI outside engine/tier0/translate.c must match. GNU objcopy
renames the baseline translator; all other objects come from this checkout.
"""

import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument(
        "--case",
        choices=("control", "home-first", "home-second", "home-immediate", "home-extended"),
        required=True,
    )
    parser.add_argument("--first-reg", type=int, choices=(8, 32, 48, 96), default=48)
    parser.add_argument("--mask", type=int, choices=range(16), default=0)
    parser.add_argument("--cpu", type=int, default=min(os.sched_getaffinity(0)))
    parser.add_argument("--blocks", type=int, default=15)
    parser.add_argument("--emit-count", type=int, default=1000)
    parser.add_argument("--run-count", type=int, default=300000)
    parser.add_argument("--objcopy", default="objcopy")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    if min(
        args.blocks, args.emit_count, args.run_count
    ) <= 0 or args.cpu not in os.sched_getaffinity(0):
        parser.error("counts must be positive and CPU must be in the affinity set")
    root = Path(__file__).resolve().parents[2]
    baseline = args.baseline.resolve()
    flags = ["TIER0_STATS=0", "TIER1_STATS=0"]
    if not args.no_build:
        for tree in (baseline, root):
            subprocess.run(
                ["make", "-j2", *flags, "build/tier0-test"],
                cwd=tree,
                check=True,
                stdout=subprocess.DEVNULL,
            )
    dry = subprocess.check_output(["make", "-Bn", *flags, "build/tier0-test"], cwd=root, text=True)
    commands = [shlex.split(line) for line in dry.replace("\\\n", " ").splitlines() if line.strip()]
    compile = next(c for c in commands if "-c" in c and "tests/tier0/main.c" in c)
    link = next(
        c for c in reversed(commands) if "-o" in c and c[c.index("-o") + 1] == "build/tier0-test"
    )
    cflags = [f for f in compile[1 : compile.index("-c")] if f not in ("-MMD", "-MP")]
    arguments = [
        arg
        for index, arg in enumerate(link[1:], 1)
        if arg != "-o"
        and link[index - 1] != "-o"
        and not arg.endswith("/tests/tier0/main.o")
        and not arg.startswith("-Wl,--wrap=")
    ]
    with tempfile.TemporaryDirectory(prefix="tier0-destination-home-abba-") as temp:
        old = Path(temp) / "baseline-translate.o"
        program = Path(temp) / "bench"
        subprocess.run(
            [
                args.objcopy,
                "--redefine-sym",
                "tier0_emit=tier0_emit_baseline",
                str(baseline / "build/hosted/engine/tier0/translate.o"),
                str(old),
            ],
            check=True,
        )
        driver = Path(__file__).resolve().with_name("destination-home-abba.c")
        subprocess.run(
            [link[0], *cflags, str(driver), *arguments, str(old), "-o", str(program)],
            cwd=root,
            check=True,
        )
        os.sched_setaffinity(0, {args.cpu})
        raw = json.loads(
            subprocess.check_output(
                [
                    str(program),
                    args.case,
                    str(args.first_reg),
                    str(args.mask),
                    str(args.blocks),
                    str(args.emit_count),
                    str(args.run_count),
                ],
                text=True,
            )
        )
    summary = {}
    for name in ("baseline", "current"):
        runs = [sample for sample in raw["samples"] if sample["variant"] == name]
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
                "first_reg": args.first_reg,
                "host_isa_mask": args.mask,
                "host_isa": raw["host_isa"],
                "cpu": args.cpu,
                "platform": platform.platform(),
                "counts": {
                    "blocks": args.blocks,
                    "emits_per_sample": args.emit_count,
                    "calls_per_sample": args.run_count,
                    "samples_per_variant": 2 * args.blocks,
                },
                "method": "Same process; one warmup block, then alternating ABBA/BAAB.",
                "scope": "Buffered emission and prebuilt native regions, with identical dependencies outside translate.c.",
                "checksum": raw["checksum"],
                "summary": summary,
                "samples": raw["samples"],
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
