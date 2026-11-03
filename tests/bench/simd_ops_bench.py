#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build baseline/candidate scalar SIMD helpers, check them, then measure ABBA.

Uses the product's ISA baseline (mk/config.mk BM_SIMD_CFLAGS: SSE4.2,
POPCNT, AVX). A zero --pairs runs the differential oracle only.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests/common"))
from bm_flags import bm_simd_cflags  # noqa: E402

BM_SIMD_CFLAGS = bm_simd_cflags()

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--baseline", type=Path, required=True)
p.add_argument("--candidate", type=Path, default=Path("."))
p.add_argument(
    "--operation",
    required=True,
    choices=("czx", "pavg", "pcmp", "minmax", "mix", "unpack", "pack", "mux", "psad"),
)
p.add_argument("--cpu", type=int, required=True)
p.add_argument("--pairs", type=int, default=9)
p.add_argument("--sanitize", action="store_true")
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()
assert 0 <= a.pairs <= 100
base, candidate = a.baseline.resolve(), a.candidate.resolve()
outdir = candidate / "build/simd-ops-bench"
outdir.mkdir(parents=True, exist_ok=True)
flags = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", *BM_SIMD_CFLAGS, "-fno-builtin"]
if a.sanitize:
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
commands = []


def run(cmd, **kw):
    commands.append(cmd)
    return subprocess.run(cmd, check=True, **kw)


source = Path("ia64/simd-ops.c")
objects = {}
for variant, work in (("baseline", base), ("candidate", candidate)):
    obj = outdir / (variant + ".o")
    run(
        [
            "gcc",
            *flags,
            "-I" + str(work / "include"),
            "-I" + str(work),
            "-c",
            str(work / source),
            "-o",
            str(obj),
        ]
    )
    objects[variant] = obj
symbols = subprocess.check_output(
    ["nm", "-g", "--defined-only", str(objects["baseline"])], text=True
)
rename = ["objcopy"]
for line in symbols.splitlines():
    name = line.split()[-1]
    if name.startswith("ia64_simd_"):
        rename += ["--redefine-sym", name + "=baseline_" + name]
run([*rename, str(objects["baseline"])])
exe = outdir / ("bench-san" if a.sanitize else "bench")
run(
    [
        "gcc",
        *flags,
        "-I" + str(candidate / "include"),
        "-I" + str(candidate),
        str(candidate / "tests/bench/simd_ops_bench.c"),
        *map(str, objects.values()),
        "-o",
        str(exe),
    ]
)
os.sched_setaffinity(0, {a.cpu})
proc = run(
    [str(exe), a.operation, str(a.pairs)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
)
raw = []
checks = None
for line in proc.stdout.splitlines():
    if line.startswith("oracle-checks="):
        checks = int(line.split("=")[1])
    if not line.startswith("row "):
        continue
    _, pair, segment, candidate_flag, selector, pattern, calls, cpu, wall, checksum = line.split()
    raw.append(
        dict(
            pair=int(pair),
            segment=int(segment),
            variant="candidate" if int(candidate_flag) else "baseline",
            selector=int(selector),
            pattern=pattern,
            calls=int(calls),
            cpu_ns=int(cpu) / int(calls),
            wall_ns=int(wall) / int(calls),
            checksum=int(checksum),
        )
    )
assert checks and (raw or not a.pairs)
results = []
for selector, pattern in dict.fromkeys((r["selector"], r["pattern"]) for r in raw):
    rows = [r for r in raw if (r["selector"], r["pattern"]) == (selector, pattern)]
    assert len(rows) == 4 * a.pairs and len({(r["calls"], r["checksum"]) for r in rows}) == 1
    times = {
        v: [
            statistics.mean(r["cpu_ns"] for r in rows if r["variant"] == v and r["pair"] == i)
            for i in range(a.pairs)
        ]
        for v in ("baseline", "candidate")
    }
    ratios = [100 * (c / b - 1) for b, c in zip(times["baseline"], times["candidate"])]
    results.append(
        dict(
            selector=selector,
            pattern=pattern,
            median_ns={v: statistics.median(t) for v, t in times.items()},
            median_paired_time_change_pct=statistics.median(ratios),
            paired_time_change_pct=ratios,
        )
    )
data = dict(
    operation=a.operation,
    baseline=str(base),
    candidate=str(candidate),
    flags=flags,
    commands=commands,
    method="same-process alternating ABBA/BAAB, average two segments per variant and pair",
    metric="thread CPU ns/call",
    cpu=a.cpu,
    host=platform.platform(),
    oracle_checks=checks,
    source_sha256={
        v: hashlib.sha256((w / source).read_bytes()).hexdigest()
        for v, w in (("baseline", base), ("candidate", candidate))
    },
    executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
    results=results,
    raw=raw,
    stdout=proc.stdout,
    stderr=proc.stderr,
)
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(data, indent=2) + "\n")
print(json.dumps(dict(oracle_checks=checks, results=results), indent=2))
