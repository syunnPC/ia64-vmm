#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Correctness pass of the harness engines on every job (no timing).

tests/tier1/bench.c checks only that each segment retired its budget, so
this runs every job's variants 0 and 1 (speed: 0) with interp, Tier0 and
Tier1 (production regions, t1h_tier1_set_flags(0)) through
`build/bench-jobdump --run`, which checks the exact retired count and the
final values the interpreter recorded at dump time.

Usage: verify_harness.py --out FILE.json [--cpu N]
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_engines  # noqa: E402
import common  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--cpu", type=int)
    args = ap.parse_args()
    results, bad = [], 0
    jobs = sorted((common.REPO / "build/bench/jobs").glob("*-slow.bin")) + [
        common.REPO / "build/bench/jobs/speed.bin"
    ]
    for job in jobs:
        variants = "0" if job.name == "speed.bin" else "0,1"
        for engine in ("interp", "tier0", "tier1"):
            r = subprocess.run(
                common.pin(args.cpu)
                + [
                    str(bench_engines.JOBDUMP),
                    f"--run={engine}",
                    f"--job={job}",
                    f"--variants={variants}",
                ]
                + bench_engines.images(),
                cwd=common.REPO,
                capture_output=True,
                text=True,
            )
            runs = []
            for line in r.stdout.splitlines():
                f = line.split()
                if f and f[0] == "jobrun":
                    run = {
                        "engine": f[1],
                        "job": f[2],
                        "variant": int(f[4]),
                        "retired": int(f[6]),
                        "ok": int(f[10]),
                    }
                    run.update({k: bench_engines.number(v) for k, v in zip(f[11::2], f[12::2])})
                    runs.append(run)
            ok = r.returncode == 0 and runs and all(x["ok"] for x in runs)
            bad += not ok
            results.append(
                {
                    "job": job.name,
                    "engine": engine,
                    "rc": r.returncode,
                    "runs": runs,
                    "stderr": r.stderr[-500:],
                }
            )
            print(
                f"{job.name:22} {engine:6} "
                + " ".join(
                    f"v{x['variant']}:{x['retired']}:{'ok' if x['ok'] else 'FAIL'}" for x in runs
                )
                + ("" if ok else f"  FAILED rc {r.returncode} {r.stderr[-200:]}"),
                flush=True,
            )
    common.write_json(
        args.out, {"repo": common.git_describe(common.REPO), "results": results, "failures": bad}
    )
    print(f"verify_harness: {len(results)} job x engine runs, {bad} failures")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
