#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run every predeclared register-cycle comparison, retaining all samples.

Run from the repository root after building pcopy_cycle_bench.mk targets.
The product emitter/encoder/string objects use the ordinary hosted harness;
native measurements execute the emitted code at the same address per pair.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--cpu", type=int)
parser.add_argument("--native-iterations", type=int, default=200000)
parser.add_argument("--native-repeat", type=int, default=120)
parser.add_argument("--emit-repeat", type=int, default=1000)
parser.add_argument("--guest-repeat", type=int, default=1000)
parser.add_argument("--compile-repeat", type=int, default=60)
parser.add_argument(
    "--compiler-only", action="store_true", help="skip unchanged native-code measurements"
)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
prefix = [] if args.cpu is None else ["taskset", "-c", str(args.cpu)]
cases = [
    (
        "mir-native",
        "tier1-pcopy-cycle-bench-bm",
        ["run", args.native_iterations, args.native_repeat],
        {},
    ),
    ("mir-product-emission", "tier1-pcopy-cycle-bench-bm", ["emit", 1, args.emit_repeat], {}),
    ("guest-product-emission", "tier1-emit-change-bench-bm", [args.emit_repeat, "emit"], {}),
]
for profile in ("SWAP", "CYCLE"):
    for live in (False, True):
        name = "swap" if profile == "SWAP" else "cycle10"
        name += "-live" if live else "-restore"
        env = {f"T1_BENCH_COPY_{profile}": "1"}
        if live:
            env["T1_BENCH_COPY_LIVE"] = "1"
        cases.append(
            (f"guest-{name}-native", "tier1-pcopy-native-bench-bm", [args.guest_repeat], env)
        )
        cases.append(
            (
                f"guest-{name}-product-emission",
                "tier1-emit-change-bench-bm",
                [args.emit_repeat, "emit"],
                env,
            )
        )
        cases.append(
            (
                f"guest-{name}-compile",
                "tier1-emit-change-bench",
                [args.compile_repeat, "compile"],
                env,
            )
        )

if args.compiler_only:
    cases = [case for case in cases if not case[0].endswith("native")]

records = []
start_all = time.monotonic()
for name, binary, argv, changes in cases:
    command = prefix + [f"build/{binary}"] + [str(x) for x in argv]
    env = {key: value for key, value in os.environ.items() if not key.startswith("T1_BENCH_")}
    env.update(changes)
    start = time.monotonic()
    with (args.output / f"{name}.csv").open("w") as output:
        result = subprocess.run(
            command, env=env, stdout=output, stderr=subprocess.STDOUT, check=False
        )
    elapsed = time.monotonic() - start
    records.append(
        {
            "name": name,
            "command": command,
            "environment": changes,
            "elapsed_seconds": elapsed,
            "returncode": result.returncode,
        }
    )
    (args.output / "commands.json").write_text(json.dumps(records, indent=2) + "\n")
    print(f"{name}: status {result.returncode}, {elapsed:.2f}s", flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
print(f"All {len(cases)} series passed in {time.monotonic() - start_all:.2f}s", flush=True)
