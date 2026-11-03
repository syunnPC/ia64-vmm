#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Alternate saved/current fast-path shim and Tier0 binaries on one CPU; record load."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/bench"))
import harness_bench


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--shim-only", action="store_true", help="measure only the four shim workloads"
    )
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--segment", type=int, default=40000000)
    parser.add_argument("--cpu", type=int, default=max(os.sched_getaffinity(0)))
    args = parser.parse_args()
    binaries = [
        ("before", args.before / "review", args.before / "tier1-test"),
        ("after", ROOT / "build/tests/review", ROOT / "build/tier1-test"),
    ]
    data = dict(
        head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        flags="SAN=0 TIER0_STATS=0 TIER1_STATS=0; -O2",
        cpu=args.cpu,
        segment=args.segment,
        runs=args.runs,
        binaries={
            str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for _, a, b in binaries
            for p in ((a,) if args.shim_only else (a, b))
        },
        samples=[],
    )
    data["fixtures_sha256"] = {
        name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
        for name in (
            "tests/review/fast_path_bench.c",
            "tests/review/tier0_data_pointer.c",
            "mk/review.mk",
        )
    }
    if args.shim_only:
        data["product_sha256"] = {
            str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (args.before / "vmm.elf", ROOT / "build/vmm.elf")
        }
    data["diff_sha256"] = hashlib.sha256(
        subprocess.check_output(["git", "diff"], cwd=ROOT)
    ).hexdigest()
    args.out.parent.mkdir(parents=True, exist_ok=True)

    def save():
        args.out.write_text(json.dumps(data, indent=2) + "\n")

    for rep in range(args.runs):
        order = binaries if rep % 2 == 0 else binaries[::-1]
        for revision, shim, _ in order:
            command = ["taskset", "-c", str(args.cpu), str(shim), "fast-path-bench"]
            before = os.getloadavg()
            run = subprocess.run(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                env=dict(os.environ, FAST_PATH_BENCH="1"),
            )
            data["samples"].append(
                dict(
                    revision=revision,
                    repeat=rep,
                    kind="shim",
                    command=command,
                    rc=run.returncode,
                    output=run.stdout,
                    stderr=run.stderr,
                    load_before=before,
                    load_after=os.getloadavg(),
                )
            )
            save()
            if run.returncode:
                raise SystemExit(f"shim failed: {run.stderr}")
        for program in (
            () if args.shim_only else ("speed", "gather_phys", "gather_virt", "shladd_xor")
        ):
            for revision, _, binary in order:
                sample = harness_bench.sample(str(binary), args.cpu, "tier0", program, args.segment)
                sample.update(
                    revision=revision,
                    repeat=rep,
                    kind="asm",
                    command=[
                        "taskset",
                        "-c",
                        str(args.cpu),
                        str(binary),
                        f"--bench=tier0:{program}:{args.segment}",
                    ]
                    + harness_bench.images(),
                )
                data["samples"].append(sample)
                save()
    print(f"{len(data['samples'])} samples: {args.out}")


if __name__ == "__main__":
    main()
