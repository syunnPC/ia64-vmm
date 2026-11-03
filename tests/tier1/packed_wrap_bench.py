#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pair complete baseline/candidate formation and lifting in one executable.

Build both checkouts first with SAN=0 TIER0_STATS=0 TIER1_STATS=0. The driver
times whole compilation with the normal arena, or dispatched execution of
already compiled code at one shared executable address. It checks independent
interpreter results and byte-identical unchanged controls before timing.
"""

import argparse
import hashlib
import json
import platform
import shlex
import statistics
import subprocess
from pathlib import Path


def run(command, root, **kwargs):
    return subprocess.run(command, cwd=root, check=True, text=True, **kwargs)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--cpu", type=int, required=True)
    parser.add_argument("--stage", choices=["run", "compile"], default="run")
    parser.add_argument("--reps", type=int, default=100)
    parser.add_argument("--pairs", type=int, default=9)
    parser.add_argument(
        "--common-simd-ref",
        help="optional local git ref supplying the same SIMD helper object to all engines",
    )
    parser.add_argument(
        "--source-nat",
        action="store_true",
        help="measure exact NaT propagation with one NaT source input",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    base = args.baseline.resolve()
    targets = {"engine/tier1/form.c", "engine/tier1/lift.c", "engine/tier1/natguard.c"}
    common = {}
    for directory in ["engine", "ia64", "core", "include"]:
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in (".c", ".h", ".def"):
                continue
            relative = path.relative_to(root).as_posix()
            if relative in targets:
                continue
            if path.read_bytes() != (base / relative).read_bytes():
                raise SystemExit("unrelated production difference: " + relative)
            common[relative] = digest(path)
    flags = root / "build/hosted/.flags"
    if flags.read_bytes() != (base / "build/hosted/.flags").read_bytes():
        raise SystemExit("hosted flags differ")
    recipe = run(
        [
            "make",
            "-Bn",
            "-f",
            "Makefile",
            "-f",
            "tests/tier1/compile_bench.mk",
            "SAN=0",
            "TIER0_STATS=0",
            "TIER1_STATS=0",
            "build/tier1-compile-bench",
        ],
        root,
        stdout=subprocess.PIPE,
    ).stdout.splitlines()
    commands = [
        shlex.split(line)
        for line in recipe
        if line.startswith("gcc ")
        and "tests/tier1/compile_bench.c" in line
        and " -o build/tier1-compile-bench" in line
    ]
    if len(commands) != 1:
        raise SystemExit("expected the canonical compile-bench link recipe")
    command = commands[0]
    out = root / "build/packed-wrap-pair"
    out.mkdir(exist_ok=True)
    common_override = None
    if args.common_simd_ref:
        source = out / "common-simd.c"
        source.write_text(
            run(
                ["git", "show", args.common_simd_ref + ":ia64/simd.c"], root, stdout=subprocess.PIPE
            ).stdout
        )
        obj = out / "common-simd.o"
        build = shlex.split(flags.read_text()) + ["-c", str(source), "-o", str(obj)]
        run(build, root)
        command[command.index("build/hosted/ia64/simd.o")] = str(obj)
        common_override = {
            "ref": args.common_simd_ref,
            "source_sha256": digest(source),
            "object_sha256": digest(obj),
            "build": build,
        }
    symbols = []
    for name in ["form", "lift", "natguard"]:
        result = run(
            ["nm", "-g", "--defined-only", str(base / f"build/hosted/engine/tier1/{name}.o")],
            root,
            stdout=subprocess.PIPE,
        ).stdout
        symbols.extend(line.split()[-1] for line in result.splitlines() if line.split())
    rename = out / "rename.txt"
    rename.write_text("".join(f"{name} baseline_{name}\n" for name in symbols))
    for name in ["form", "lift", "natguard"]:
        dest = out / f"{name}-baseline.o"
        run(
            [
                "objcopy",
                "--redefine-syms=" + str(rename),
                str(base / f"build/hosted/engine/tier1/{name}.o"),
                str(dest),
            ],
            root,
        )
        command.append(str(dest))
    command[command.index("tests/tier1/compile_bench.c")] = "tests/tier1/packed_wrap_bench.c"
    command[command.index("-o") + 1] = str(out / "bench")
    for name in ["t1_form_request", "t1_lift", "t1_natguard", "t1_arena_place", "t1_arena_release"]:
        command.append("-Wl,--wrap=" + name)
    run(command, root)
    invocation = [
        "taskset",
        "-c",
        str(args.cpu),
        str(out / "bench"),
        args.stage,
        str(args.reps),
        str(args.pairs),
        "nat" if args.source_nat else "clear",
    ]
    result = subprocess.run(
        invocation, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if result.returncode:
        print(result.stdout)
        print(result.stderr)
        result.check_returncode()
    observations = {}
    for line in result.stdout.splitlines():
        fields = line.split(",")
        if fields[0] != "pair":
            continue
        before, after = int(fields[5]) / int(fields[4]), int(fields[6]) / int(fields[4])
        observations.setdefault(fields[2], []).append(
            {"baseline_ns": before, "candidate_ns": after, "change_pct": 100 * (after / before - 1)}
        )
        if args.stage == "run":
            tier0 = int(fields[7]) / int(fields[4])
            observations[fields[2]][-1].update(
                tier0_ns=tier0,
                baseline_vs_tier0_pct=100 * (before / tier0 - 1),
                candidate_vs_tier0_pct=100 * (after / tier0 - 1),
            )
    summary = {
        name: {
            "baseline_median_ns": statistics.median(row["baseline_ns"] for row in rows),
            "candidate_median_ns": statistics.median(row["candidate_ns"] for row in rows),
            "paired_median_change_pct": statistics.median(row["change_pct"] for row in rows),
            "improved_pairs": sum(row["candidate_ns"] < row["baseline_ns"] for row in rows),
            "pairs": len(rows),
        }
        for name, rows in observations.items()
    }
    if args.stage == "run":
        for name, rows in observations.items():
            summary[name].update(
                tier0_median_ns=statistics.median(row["tier0_ns"] for row in rows),
                baseline_vs_tier0_pct=statistics.median(
                    row["baseline_vs_tier0_pct"] for row in rows
                ),
                candidate_vs_tier0_pct=statistics.median(
                    row["candidate_vs_tier0_pct"] for row in rows
                ),
            )
    report = {
        "stage": args.stage,
        "clock": "CLOCK_THREAD_CPUTIME_ID",
        "order": "rotated ABC/CBA (baseline Tier1, candidate Tier1, Tier0)"
        if args.stage == "run"
        else "ABBA/BAAB",
        "common_simd_override": common_override,
        "source_nat": args.source_nat,
        "cpu": args.cpu,
        "reps": args.reps,
        "pairs": args.pairs,
        "summary": summary,
        "observations": observations,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "build": command,
        "run": invocation,
        "flags": flags.read_text(),
        "platform": platform.platform(),
        "compiler": run(["gcc", "--version"], root, stdout=subprocess.PIPE).stdout,
        "target_sha256": {
            name: {"baseline": digest(base / name), "candidate": digest(root / name)}
            for name in sorted(targets)
        },
        "object_sha256": {
            str(path): digest(path)
            for name in ["form", "lift", "natguard"]
            for path in [
                base / f"build/hosted/engine/tier1/{name}.o",
                root / f"build/hosted/engine/tier1/{name}.o",
            ]
        },
        "unchanged_sources": common,
        "binary_sha256": digest(out / "bench"),
        "driver_sha256": digest(root / "tests/tier1/packed_wrap_bench.c"),
        "runner_sha256": digest(Path(__file__)),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
