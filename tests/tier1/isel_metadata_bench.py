#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare prebuilt selectors in one process, with ABBA call order.

The reference checkout is never built or changed. Independent whole-production
executables first check output identity; the paired executable then uses the
exact reference isel.o, with its exported names renamed, beside the candidate.
This requires matching hosted build flags and compatible IR/MIR/rule layouts.
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
    p.add_argument("--driver", choices=("chain", "real"), required=True)
    p.add_argument("--count", type=int, default=64)
    p.add_argument("--repeat", type=int, default=1000)
    p.add_argument("--short", action="store_true")
    p.add_argument("--cpu", type=int, required=True)
    p.add_argument("--pairs", type=int, default=9)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    if not 1 <= a.count <= 2048 or not 1 <= a.repeat <= (2**32 - 9) // 2 or a.pairs < 1:
        p.error(
            "count must be 1..2048; repeat and pairs must be positive and repeat must fit twice"
        )
    roots = {"baseline": a.baseline.resolve(), "candidate": a.candidate.resolve()}
    candidate = roots["candidate"]
    flags = {v: (r / "build/hosted/.flags").read_text().strip() for v, r in roots.items()}
    assert flags["baseline"] == flags["candidate"], "build flags differ"
    os.sched_setaffinity(0, {a.cpu})
    lines = subprocess.check_output(
        ["make", "-Bn", "SAN=0", "TIER0_STATS=0", "TIER1_STATS=0", "build/tier1-test"],
        cwd=candidate,
        text=True,
    ).splitlines()
    link = next(
        shlex.split(line) for line in reversed(lines) if line.endswith("-o build/tier1-test")
    )
    main_object = "build/hosted/tests/tier1/main.o"
    assert link.count(main_object) == 1
    production = [
        arg
        for arg in link
        if arg.startswith("build/hosted/")
        and arg.endswith(".o")
        and not arg.startswith("build/hosted/tests/")
    ]
    source = (
        candidate / "tests/tier1" / ("bench_isel.c" if a.driver == "chain" else "compile_bench.c")
    )
    binaries = {}
    for variant, root in roots.items():
        binaries[variant] = candidate / ("build/tier1-isel-metadata-" + a.driver + "-" + variant)
        cflags = [
            "-I" + str(root / "include")
            if arg == "-Iinclude"
            else "-I" + str(root)
            if arg == "-I."
            else arg
            for arg in shlex.split(flags[variant])
        ]
        argv = [
            str(source)
            if arg == main_object
            else str(binaries[variant])
            if arg == "build/tier1-test"
            else str(root / arg)
            if arg in production
            else arg
            for arg in link[1:]
        ]
        subprocess.run([*cflags, *argv], cwd=candidate, check=True)

    reference_object = roots["baseline"] / "build/hosted/engine/tier1/isel.o"
    nm = subprocess.check_output(["nm", "-g", "--defined-only", str(reference_object)], text=True)
    symbols = [line.split()[-1] for line in nm.splitlines()]
    assert set(symbols) == {
        "t1_isel",
        "t1_rules",
        "t1_rule_count",
        "t1_test_isel_crosscheck",
        "t1_test_isel_candidates",
    }
    rename = candidate / "build/tier1-isel-metadata-reference.syms"
    rename.write_text("".join(f"{symbol} {symbol}_reference\n" for symbol in symbols))
    private_reference = candidate / "build/tier1-isel-metadata-reference.o"
    subprocess.run(
        [
            "objcopy",
            "--redefine-syms=" + str(rename),
            str(reference_object),
            str(private_reference),
        ],
        check=True,
    )
    paired_binary = candidate / ("build/tier1-isel-metadata-" + a.driver + "-paired")
    additions = [str(private_reference)]
    sources = [source, Path(__file__).resolve()]
    if a.driver == "real":
        wrapper = candidate / "tests/tier1/isel_pair_bench.c"
        additions += [str(wrapper), "-Wl,--wrap=t1_isel"]
        sources.append(wrapper)
    argv = [
        str(source)
        if arg == main_object
        else str(paired_binary)
        if arg == "build/tier1-test"
        else arg
        for arg in link[1:]
    ]
    subprocess.run([*shlex.split(flags["candidate"]), *argv, *additions], cwd=candidate, check=True)
    env = dict(os.environ, T1_BENCH_CPU="1")
    if a.short:
        env["T1_BENCH_SHORT"] = "1"
    else:
        env.pop("T1_BENCH_SHORT", None)

    def run(binary, paired=False):
        repeat = a.repeat * (2 if paired else 1)
        args = [str(a.count), str(repeat)] if a.driver == "chain" else [str(repeat), "isel"]
        proc = subprocess.run(
            [str(binary), *args],
            cwd=candidate,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if proc.returncode:
            raise RuntimeError(proc.stdout[-4000:] + proc.stderr[-4000:])
        rows, checksums = [], {}
        for line in proc.stdout.splitlines():
            if line.startswith("isel-bench "):
                fields = dict(field.split("=", 1) for field in line.split()[1:])
                checksums["chain"] = fields["checksum"]
                if not paired:
                    rows.append(
                        dict(
                            mode="chain",
                            ns=float(fields["warm_ns"]),
                            checksum=fields["checksum"],
                            fields=fields,
                        )
                    )
            elif line.startswith("correctness,"):
                _, mode, checksum = line.split(",")
                checksums[mode] = checksum
            elif line.startswith("bench,isel,") and not paired:
                _, _, mode, samples, ns = line.split(",")
                rows.append(
                    dict(mode=mode, ns=float(ns), samples=int(samples), checksum=checksums[mode])
                )
            elif line.startswith("isel-paired,") and paired:
                _, mode, variant, samples, ns, *checksum = line.split(",")
                rows.append(
                    dict(
                        mode=mode,
                        variant=variant,
                        samples=int(samples),
                        ns=float(ns),
                        checksum=checksum[0] if checksum else checksums[mode],
                    )
                )
        assert rows, proc.stdout
        return rows

    identity = lambda rows: [(row["mode"], row["checksum"]) for row in rows]
    warmup = {v: run(binary) for v, binary in binaries.items()}
    assert identity(warmup["baseline"]) == identity(warmup["candidate"]), (
        "independent output differs"
    )
    raw = []
    for pair in range(a.pairs):
        rows = run(paired_binary, paired=True)
        for variant in roots:
            selected = [row for row in rows if row["variant"] == variant]
            assert identity(selected) == identity(warmup[variant]), "paired output differs"
            assert all(row["samples"] == a.repeat for row in selected)
        raw.extend(dict(row, pair=pair) for row in rows)
    summary = []
    for mode, checksum in identity(warmup["baseline"]):
        values = {
            v: [row["ns"] for row in raw if row["mode"] == mode and row["variant"] == v]
            for v in roots
        }
        median = {v: statistics.median(ns) for v, ns in values.items()}
        changes = [100 * (b / a - 1) for a, b in zip(values["baseline"], values["candidate"])]
        summary.append(
            dict(
                mode=mode,
                checksum=checksum,
                median_ns=median,
                paired_change_pct=changes,
                median_paired_change_pct=statistics.median(changes),
            )
        )
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    result = dict(
        driver=a.driver,
        count=a.count,
        repeat=a.repeat,
        short=a.short,
        cpu=a.cpu,
        pairs=a.pairs,
        order="same-process ABBA",
        time_metric="thread CPU nanoseconds",
        platform=platform.platform(),
        build_flags=flags,
        warmup=warmup,
        raw=raw,
        summary=summary,
        source_sha256={str(p.relative_to(candidate)): sha(p) for p in sources},
        roots={v: str(r) for v, r in roots.items()},
        production_sha256={v: {obj: sha(r / obj) for obj in production} for v, r in roots.items()},
        executable_sha256={v: sha(binary) for v, binary in binaries.items()},
        paired_executable_sha256=sha(paired_binary),
    )
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
