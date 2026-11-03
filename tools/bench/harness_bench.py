#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""asm kernels on interp, Tier0, Tier1 and the product path Tier0+Tier1 in
the hosted Tier1 harness.

One process per sample (`build/tier1-test --bench=ENGINE:PROGRAM[:SEGMENT]`,
tests/tier1/bench.c): cold = the first SEGMENT instructions of a fresh
process, warm = the next SEGMENT. Samples are interleaved (repetition outer,
program x engine inner), each pinned to one CPU, with /proc/loadavg read
around every sample. A cell (program, engine) whose cold or warm spread
(max - min) / median exceeds --max-spread is re-run (fresh, interleaved
samples) up to --reruns times; every sample stays in the JSON.

Engine `tier0+tier1` is the product
path (tier0_run with Tier1 as its upper tier, regions compiled by hotness at
the harness thresholds, loop 2 and trace 8). Every sample runs with
T1H_HOSTWIN_CHECK=0: the harness otherwise runs the MMU-log
completeness check at every Tier1 entry, which the product does not.

Usage: tools/bench/harness_bench.py --out build/bench/harness.json
"""

import os
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

PROGRAMS = ["speed", "gather_phys", "gather_virt", "shladd_xor"]
ENGINES = ["interp", "tier0", "tier1", "tier0+tier1"]


def images():
    found = sorted(str(p) for p in (common.REPO / "build/tests/tier1").glob("*.bin"))
    found.append(str(common.REPO / "build/tests/ia64/speed.bin"))
    found += sorted(str(p) for p in (common.REPO / "build/tests/tier1/frag").glob("*.bin"))
    return found


def bench_env():
    """The harness without the MMU-log completeness check (module docstring)."""
    return dict(os.environ, T1H_HOSTWIN_CHECK="0")


def sample(binary, cpu, engine, program, segment):
    spec = f"{engine}:{program}" + (f":{segment}" if segment else "")
    before = common.loadavg()
    r = subprocess.run(
        common.pin(cpu) + [binary, f"--bench={spec}"] + images(),
        capture_output=True,
        text=True,
        cwd=common.REPO,
        env=bench_env(),
    )
    after = common.loadavg()
    if r.returncode != 0:
        raise SystemExit(f"harness_bench: {spec} failed ({r.returncode}):\n{r.stderr}")
    out = {"engine": engine, "program": program, "loadavg_before": before, "loadavg_after": after}
    for line in r.stdout.splitlines():
        f = line.split()
        if len(f) == 11 and f[0] == "bench":
            out.update(start=float(f[4]), cold=float(f[6]), warm=float(f[8]), segment=int(f[10]))
        elif f and f[0] == "bench-t1" and f[3] != "n/a":
            out["t1"] = {k: float(v) for k, v in zip(f[3::2], f[4::2])}
    if "warm" not in out:
        raise SystemExit(f"harness_bench: {spec}: no bench line:\n{r.stdout}")
    return out


def cells_over(samples, max_spread):
    cells = {}
    for s in samples:
        cells.setdefault((s["program"], s["engine"]), []).append(s)
    bad = []
    for key, rows in cells.items():
        for metric in ("cold", "warm"):
            if common.spread([r[metric] for r in rows])[3] > max_spread:
                bad.append(key)
                break
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(common.REPO / "build/tier1-test"))
    ap.add_argument("--programs", nargs="+", default=PROGRAMS)
    ap.add_argument("--engines", nargs="+", default=ENGINES)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--segment", type=int, default=0, help="0: bench.c's default (4M)")
    ap.add_argument("--max-spread", type=float, default=0.10)
    ap.add_argument("--reruns", type=int, default=2)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    cpu, busy = common.pick_cpu()
    data = {
        "kind": "harness",
        "cpu": cpu,
        "cpu_busy_at_start": busy,
        "host": common.host_info(),
        "repo": common.git_describe(common.REPO),
        "runs": args.runs,
        "max_spread": args.max_spread,
        "samples": [],
        "reruns": [],
    }
    print(f"harness_bench: pinned to CPU {cpu}", flush=True)
    for rep in range(args.runs):
        for program in args.programs:
            for engine in args.engines:
                s = sample(args.binary, cpu, engine, program, args.segment)
                s["round"] = 0
                data["samples"].append(s)
                print(
                    f"  rep {rep} {program:8} {engine:6} cold {s['cold'] / 1e6:8.1f} "
                    f"warm {s['warm'] / 1e6:8.1f} Minsn/s  load {s['loadavg_before'][0]:.2f}",
                    flush=True,
                )
        common.write_json(args.out, data)
    # Each cell is judged on the samples of its latest round only.
    pending = {(p, e) for p in args.programs for e in args.engines}
    for rnd in range(1, args.reruns + 1):
        bad = [
            k
            for k in cells_over(
                [s for s in data["samples"] if s["round"] == rnd - 1], args.max_spread
            )
            if k in pending
        ]
        pending = set(bad)
        if not bad:
            break
        print(f"harness_bench: round {rnd} re-runs {bad}", flush=True)
        data["reruns"] += [{"cell": list(k), "round": rnd} for k in bad]
        for rep in range(args.runs):
            for program, engine in bad:
                s = sample(args.binary, cpu, engine, program, args.segment)
                s["round"] = rnd
                data["samples"].append(s)
        common.write_json(args.out, data)
    common.write_json(args.out, data)
    print(f"harness_bench: wrote {args.out}")


if __name__ == "__main__":
    main()
