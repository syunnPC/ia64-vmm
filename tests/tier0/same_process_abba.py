#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Same-process ABBA comparison of Tier0 translate.c and x86emit.c.

All other linked dependencies and their ABI must match. GNU objcopy renames
the baseline translator and every baseline emitter symbol, including their
cross-object references. Native execution excludes lookup/dispatch. Always
use fresh STATS=0, SAN=0 objects, or supply --no-build for frozen builds.
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
        choices=(
            "control",
            "adjacent",
            "nop-gap",
            "hint-gap",
            "neutral",
            "neutral-independent",
            "neutral-mixed",
            "self-moves",
            "self-shifts",
            "self-mixed",
            "system-lc",
            "system-pfs",
            "system-br",
            "system-pr",
            "memory-load",
            "memory-store",
            "memory-update",
            "memory-virtual",
        ),
        required=True,
    )
    parser.add_argument("--first-reg", type=int, choices=(8, 32, 48, 96), default=48)
    parser.add_argument("--mask", type=int, choices=range(16), default=0)
    parser.add_argument("--cpu", type=int, default=min(os.sched_getaffinity(0)))
    parser.add_argument("--blocks", type=int, default=15)
    parser.add_argument("--emit-count", type=int, default=1000)
    parser.add_argument("--run-count", type=int, default=300000)
    parser.add_argument("--insns", type=int, default=48)
    parser.add_argument("--offset", type=int, choices=(0, 16, 32, 48), default=0)
    parser.add_argument("--ic", action="store_true")
    parser.add_argument(
        "--budget", type=int, help="native call budget; defaults to the full region"
    )
    parser.add_argument("--objcopy", default="objcopy")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    if min(
        args.blocks, args.emit_count, args.run_count, args.insns
    ) <= 0 or args.cpu not in os.sched_getaffinity(0):
        parser.error("counts must be positive and CPU must be in the affinity set")
    budget = args.insns if args.budget is None else args.budget
    if not 0 <= budget < 1 << 64:
        parser.error("budget must fit uint64_t")
    root = Path(__file__).resolve().parents[2]
    baseline = args.baseline.resolve()
    flags = ["TIER0_STATS=0", "TIER1_STATS=0", "SAN=0"]
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
    with tempfile.TemporaryDirectory(prefix="tier0-same-process-abba-") as temp:
        objects = [
            baseline / "build/hosted/engine/tier0" / (name + ".o")
            for name in ("translate", "x86emit")
        ]
        symbols = set()
        for old in objects:
            for line in subprocess.check_output(
                [args.nm, "-g", "--defined-only", str(old)], text=True
            ).splitlines():
                name = line.split()[-1]
                if name == "tier0_emit" or name.startswith("x86_"):
                    symbols.add(name)
        if "tier0_emit" not in symbols or "x86_load" not in symbols:
            raise RuntimeError("baseline objects do not expose the expected symbols")
        mapping = Path(temp) / "symbols"
        mapping.write_text("".join(f"{name} {name}_baseline\n" for name in sorted(symbols)))
        renamed = []
        for old in objects:
            output = Path(temp) / old.name
            subprocess.run(
                [args.objcopy, "--redefine-syms", str(mapping), str(old), str(output)], check=True
            )
            renamed.append(str(output))
        program = Path(temp) / "bench"
        driver = Path(__file__).resolve().with_name("same-process-abba.c")
        subprocess.run(
            [link[0], *cflags, str(driver), *arguments, *renamed, "-o", str(program)],
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
                    str(args.insns),
                    str(args.offset),
                    str(int(args.ic)),
                    str(budget),
                ],
                text=True,
            )
        )
    metrics = ("bytes", "emit_ns_per_region", "run_ns_per_call")
    if budget:
        metrics += ("run_ns_per_instruction",)
    summary = {}
    for name in ("baseline", "current"):
        runs = [sample for sample in raw["samples"] if sample["variant"] == name]
        if len({run["bytes"] for run in runs}) != 1:
            raise RuntimeError("code size changed across identical emissions")
        summary[name] = {key: statistics.median(run[key] for run in runs) for key in metrics}
    paired = {}
    for key in metrics:
        ratios = []
        for block in range(1, args.blocks + 1):
            means = {
                name: statistics.mean(
                    sample[key]
                    for sample in raw["samples"]
                    if sample["variant"] == name and sample["block"] == block
                )
                for name in summary
            }
            ratios.append(100 * (means["current"] / means["baseline"] - 1))
        paired[key] = {"median_change_pct": statistics.median(ratios), "block_changes_pct": ratios}
    print(
        json.dumps(
            {
                "case": args.case,
                "first_reg": args.first_reg,
                "host_isa_mask": args.mask,
                "host_isa": raw["host_isa"],
                "cpu": args.cpu,
                "platform": platform.platform(),
                "insns": args.insns,
                "offset": args.offset,
                "psr_ic": args.ic,
                "budget": budget,
                "counts": {
                    "blocks": args.blocks,
                    "emits_per_sample": args.emit_count,
                    "calls_per_sample": args.run_count,
                    "samples_per_variant": 2 * args.blocks,
                },
                "method": "Same process; one warmup block, then alternating ABBA/BAAB.",
                "scope": "Buffered emission and prebuilt native regions; dependencies outside translate.c/x86emit.c are shared.",
                "checksum": raw["checksum"],
                "summary": summary,
                "paired": paired,
                "samples": raw["samples"],
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
