#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Paired loop-program/compiled-C measurements of interpreter and Tier0.

Build each revision's tier1-test and bench-jobdump first. Prepare compiled-C
jobs with tools/bench/bench_engines.py (the slow variants). Every revision
runs the same guest inputs; alternate revision order on one pinned host CPU.
--rev LABEL=BUILD_DIR may be repeated; the final revision is the candidate.
An rc=0 means all samples were valid, not that a performance target passed.
"""

import argparse
import hashlib
from pathlib import Path
import statistics
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/bench"))
import bench_engines as bench  # noqa: E402
import common  # noqa: E402
import harness_bench  # noqa: E402


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rev", action="append", required=True, metavar="LABEL=BUILD_DIR")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument(
        "--engine",
        action="append",
        choices=("tier0", "interp"),
        help="select an engine for focused profiling; default: both",
    )
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    if args.rounds < 1 or len(args.rev) < 2:
        ap.error("at least two revisions and one round are required")
    revs = []
    for spec in args.rev:
        label, sep, directory = spec.partition("=")
        if not sep or not label or not directory:
            ap.error("--rev requires LABEL=BUILD_DIR")
        revs.append((label, Path(directory).resolve()))
    if len({label for label, _ in revs}) != len(revs):
        ap.error("revision labels must be unique")
    hashes = {
        str(d / binary): digest(d / binary)
        for _, d in revs
        for binary in ("tier1-test", "bench-jobdump")
    }
    cpu, busy = common.pick_cpu()
    data = dict(
        cpu=cpu,
        busy=busy,
        host=common.host_info(),
        rounds=args.rounds,
        revisions=[(label, str(d)) for label, d in revs],
        binary_sha256=hashes,
        samples=[],
        comparisons=[],
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    cells = [("asm", p) for p in harness_bench.PROGRAMS] + [
        ("csuite", f"csuite:{k}") for k in range(6)
    ]
    for engine in args.engine or ("tier0", "interp"):
        for rep in range(args.rounds):
            for suite, program in cells:
                for label, directory in revs if rep % 2 == 0 else revs[::-1]:
                    bench.JOBDUMP = directory / "bench-jobdump"
                    if suite == "asm":
                        sample = harness_bench.sample(
                            str(directory / "tier1-test"), cpu, engine, program, 40000000
                        )
                    else:
                        sample = bench.harness_csuite_sample(engine, program, cpu)
                        if sample["rc"] or not sample["ok"]:
                            raise SystemExit(
                                f"{label} {engine} {program}: invalid compiled-C sample"
                            )
                    sample.update(repeat=rep, revision=label, suite=suite)
                    data["samples"].append(sample)
                    common.write_json(args.out, data)
        for baseline, _ in revs[:-1]:
            for _, program in cells:
                row = dict(engine=engine, program=program, baseline=baseline, candidate=revs[-1][0])
                for metric in ("cold", "warm"):
                    values = lambda label: [
                        r[metric]
                        for r in data["samples"]
                        if r["engine"] == engine
                        and r["program"] == program
                        and r["revision"] == label
                    ]
                    before, after = values(baseline), values(revs[-1][0])
                    row[metric] = dict(
                        baseline=statistics.median(before),
                        candidate=statistics.median(after),
                        rate_change=statistics.median(after) / statistics.median(before) - 1,
                        baseline_spread=common.spread(before)[3],
                        candidate_spread=common.spread(after)[3],
                    )
                data["comparisons"].append(row)
                print(
                    engine,
                    baseline,
                    program,
                    " ".join(f"{k} {row[k]['rate_change']:+.3%}" for k in ("cold", "warm")),
                    flush=True,
                )
    changed = [path for path, sha in hashes.items() if digest(Path(path)) != sha]
    data["changed_binaries"] = changed
    common.write_json(args.out, data)
    if changed:
        raise SystemExit("benchmark binaries changed during measurement: " + ", ".join(changed))


if __name__ == "__main__":
    main()
