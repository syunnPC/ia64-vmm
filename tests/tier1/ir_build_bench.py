#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build one common driver against already-built baseline/candidate objects.

Neither checkout is built by this script: make -Bn only obtains the candidate
test link command. New benchmark executables are written under candidate/build.
Baseline production objects and headers are paired with the common test harness;
use only revisions with a compatible test-harness ABI.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--baseline", type=Path, required=True)
    p.add_argument("--candidate", type=Path, default=Path("."))
    p.add_argument("--driver", choices=("builder", "lift"), required=True)
    p.add_argument("--count", type=int, default=32, help="builder op/parameter count")
    p.add_argument("--repeat", type=int, default=10000)
    p.add_argument("--short", action="store_true", help="one-bundle lift controls")
    p.add_argument(
        "--same-process", action="store_true", help="interleave both versions in ABBA order"
    )
    p.add_argument("--cpu", type=int, required=True)
    p.add_argument("--pairs", type=int, default=9)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    if min(a.count, a.repeat, a.pairs) < 1 or a.count > 8192:
        p.error("counts, repeats and pairs must be positive; count <= 8192")
    roots = {"baseline": a.baseline.resolve(), "candidate": a.candidate.resolve()}
    candidate = roots["candidate"]
    flags = {v: (root / "build/hosted/.flags").read_text().strip() for v, root in roots.items()}
    assert flags["baseline"] == flags["candidate"], "build flags differ"
    os.sched_setaffinity(0, {a.cpu})
    lines = subprocess.check_output(
        ["make", "-Bn", "build/tier1-test"], cwd=candidate, text=True
    ).splitlines()
    link = next(
        shlex.split(line) for line in reversed(lines) if line.endswith("-o build/tier1-test")
    )
    harness = "build/hosted/tests/tier1/main.o"
    assert link.count(harness) == 1
    production = [
        arg
        for arg in link
        if arg.startswith("build/hosted/")
        and arg.endswith(".o")
        and not arg.startswith("build/hosted/tests/")
    ]
    source = (
        candidate
        / "tests/tier1"
        / ("bench_ir_build.c" if a.driver == "builder" else "compile_bench.c")
    )
    binaries = {}
    for variant, root in roots.items():
        binaries[variant] = candidate / "build" / ("tier1-ir-" + a.driver + "-" + variant)
        cflags = shlex.split(flags[variant])
        cflags = [
            "-I" + str(root / "include")
            if arg == "-Iinclude"
            else "-I" + str(root)
            if arg == "-I."
            else arg
            for arg in cflags
        ]
        argv = [
            str(source)
            if arg == harness
            else str(binaries[variant])
            if arg == "build/tier1-test"
            else str(root / arg)
            if arg in production
            else arg
            for arg in link[1:]
        ]
        subprocess.run(cflags + argv, cwd=candidate, check=True)
    paired_binary = None
    extra_sources = []
    if a.same_process:
        # Rename reference IR definitions and their callers in lift.o.
        reference = roots["baseline"]
        ir_object = reference / "build/hosted/engine/tier1/ir.o"
        nm = subprocess.check_output(["nm", "-g", "--defined-only", str(ir_object)], text=True)
        symbols = [line.split()[-1] for line in nm.splitlines()]
        assert all(symbol.startswith(("t1_ir_", "t1_op_desc")) for symbol in symbols)
        rename = candidate / "build/tier1-ir-reference.syms"
        rename.write_text(
            "".join(f"{symbol} {symbol}_reference\n" for symbol in [*symbols, "t1_lift"])
        )
        reference_ir = candidate / "build/tier1-ir-reference.o"
        subprocess.run(
            ["objcopy", "--redefine-syms=" + str(rename), str(ir_object), str(reference_ir)],
            check=True,
        )
        additions = [str(reference_ir)]
        cflags = shlex.split(flags["candidate"])
        if a.driver == "builder":
            for variant, root in roots.items():
                obj = candidate / ("build/tier1-builder-" + variant + ".o")
                local_flags = shlex.split(flags[variant])
                local_flags = [
                    "-I" + str(root / "include")
                    if arg == "-Iinclude"
                    else "-I" + str(root)
                    if arg == "-I."
                    else arg
                    for arg in local_flags
                ]
                definitions = (
                    ["-D" + symbol + "=" + symbol + "_reference" for symbol in symbols]
                    if variant == "baseline"
                    else []
                )
                name = "reference" if variant == "baseline" else "candidate"
                subprocess.run(
                    [
                        *local_flags,
                        *definitions,
                        "-DT1_BUILD_ONLY=t1_bench_build_" + name,
                        "-c",
                        str(source),
                        "-o",
                        str(obj),
                    ],
                    cwd=candidate,
                    check=True,
                )
                additions.append(str(obj))
            paired_source = candidate / "tests/tier1/ir_build_pair_bench.c"
        else:
            reference_lift = candidate / "build/tier1-lift-reference.o"
            subprocess.run(
                [
                    "objcopy",
                    "--redefine-syms=" + str(rename),
                    str(reference / "build/hosted/engine/tier1/lift.o"),
                    str(reference_lift),
                ],
                check=True,
            )
            additions += [
                str(reference_lift),
                str(candidate / "tests/tier1/lift_pair_bench.c"),
                "-Wl,--wrap=t1_lift",
            ]
            extra_sources.append(candidate / "tests/tier1/lift_pair_bench.c")
            paired_source = source
        extra_sources.append(paired_source)
        paired_binary = candidate / ("build/tier1-ir-" + a.driver + "-paired")
        argv = [
            str(paired_source)
            if arg == harness
            else str(paired_binary)
            if arg == "build/tier1-test"
            else arg
            for arg in link[1:]
        ]
        subprocess.run([*cflags, *argv, *additions], cwd=candidate, check=True)
    env = dict(os.environ, T1_BENCH_CPU="1")
    if a.short:
        env["T1_BENCH_SHORT"] = "1"
    else:
        env.pop("T1_BENCH_SHORT", None)
    args = [str(a.count), str(a.repeat)] if a.driver == "builder" else [str(a.repeat), "lift"]

    def run(variant):
        proc = subprocess.run(
            [str(binaries[variant]), *args],
            cwd=candidate,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if proc.returncode:
            raise RuntimeError(proc.stdout[-3000:] + proc.stderr[-3000:])
        rows, checksums = [], {}
        for line in proc.stdout.splitlines():
            if line.startswith("ir-build-bench "):
                fields = dict(field.split("=", 1) for field in line.split()[1:])
                rows.append(
                    dict(
                        mode=fields["mode"],
                        ns=float(fields["ns_per_build"]),
                        checksum=fields["checksum"],
                        fields=fields,
                    )
                )
            elif line.startswith("correctness,"):
                _, mode, checksum = line.split(",")
                checksums[mode] = checksum
            elif line.startswith("bench,lift,"):
                _, _, mode, repeats, ns = line.split(",")
                rows.append(
                    dict(mode=mode, ns=float(ns), checksum=checksums[mode], repeats=int(repeats))
                )
        assert rows, proc.stdout
        return rows

    def run_paired():
        paired_args = (
            [str(a.count), str(a.repeat)] if a.driver == "builder" else [str(a.repeat * 2), "lift"]
        )
        proc = subprocess.run(
            [str(paired_binary), *paired_args],
            cwd=candidate,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if proc.returncode:
            raise RuntimeError(proc.stdout[-3000:] + proc.stderr[-3000:])
        rows, checksums = [], {}
        for line in proc.stdout.splitlines():
            if line.startswith("ir-build-paired "):
                fields = dict(field.split("=", 1) for field in line.split()[1:])
                rows.append(
                    dict(
                        mode=fields["mode"],
                        variant=fields["variant"],
                        ns=float(fields["ns"]),
                        checksum=fields["checksum"],
                        repeats=int(fields["repetitions"]),
                    )
                )
            elif line.startswith("correctness,"):
                _, mode, checksum = line.split(",")
                checksums[mode] = checksum
            elif line.startswith("lift-paired,"):
                _, mode, variant, repeats, ns = line.split(",")
                rows.append(
                    dict(
                        mode=mode,
                        variant=variant,
                        ns=float(ns),
                        checksum=checksums[mode],
                        repeats=int(repeats),
                    )
                )
        assert rows, proc.stdout
        return rows

    identity = lambda rows: [(row["mode"], row["checksum"]) for row in rows]
    warmup = {variant: run(variant) for variant in roots}
    assert identity(warmup["baseline"]) == identity(warmup["candidate"]), "output differs"
    raw = []
    for pair in range(a.pairs):
        if a.same_process:
            rows = run_paired()
            for variant in roots:
                selected = [row for row in rows if row["variant"] == variant]
                assert identity(selected) == identity(warmup[variant]), "unstable paired output"
            raw.extend(dict(row, pair=pair) for row in rows)
            continue
        order = ("baseline", "candidate") if pair % 2 == 0 else ("candidate", "baseline")
        for variant in order:
            rows = run(variant)
            assert identity(rows) == identity(warmup[variant]), "unstable output"
            raw.extend(dict(row, pair=pair, variant=variant) for row in rows)
    summary = []
    for mode, checksum in identity(warmup["baseline"]):
        values = {
            v: [row["ns"] for row in raw if row["variant"] == v and row["mode"] == mode]
            for v in roots
        }
        medians = {v: statistics.median(ns) for v, ns in values.items()}
        paired = [
            100 * (new / old - 1) for old, new in zip(values["baseline"], values["candidate"])
        ]
        summary.append(
            dict(
                mode=mode,
                checksum=checksum,
                median_ns=medians,
                median_time_change_pct=100 * (medians["candidate"] / medians["baseline"] - 1),
                paired_change_pct=paired,
                median_paired_change_pct=statistics.median(paired),
            )
        )
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    result = dict(
        driver=a.driver,
        count=a.count,
        repeat=a.repeat,
        short=a.short,
        cpu=a.cpu,
        same_process=a.same_process,
        order="ABBA" if a.same_process else "AB/BA alternating",
        pairs=a.pairs,
        platform=platform.platform(),
        time_metric="thread CPU nanoseconds",
        build_flags=flags,
        source_sha256={str(p.relative_to(candidate)): sha(p) for p in [source, *extra_sources]},
        roots={v: str(root) for v, root in roots.items()},
        production_sha256={
            v: {obj: sha(root / obj) for obj in production} for v, root in roots.items()
        },
        executable_sha256={v: sha(path) for v, path in binaries.items()},
        paired_executable_sha256=sha(paired_binary) if paired_binary else None,
        warmup=warmup,
        raw=raw,
        summary=summary,
    )
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
