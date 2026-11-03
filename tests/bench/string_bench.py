#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare two core/string.c versions with identical freestanding flags.

Example: python3 tests/bench/string_bench.py --baseline ../baseline --case memmove --cpu 0
The result is an alternating native microbenchmark, not guest throughput.
"""

import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path, help="baseline checkout")
    parser.add_argument("--candidate", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--case", choices=("memmove", "memcmp"), required=True)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--cc", default=os.environ.get("HOSTCC", "cc"))
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    out = args.candidate.resolve() / "build/string-bench"
    out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_suffix(".c").resolve()
    flags = [
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-tree-loop-distribute-patterns",
        "-mgeneral-regs-only",
        "-fno-stack-protector",
        "-U_FORTIFY_SOURCE",
    ]
    renames = [
        "-D" + name + "=vmm_" + name
        for name in ("memcpy", "memset", "memmove", "memcmp", "strlen", "strcmp", "strncmp")
    ]
    binaries = {}
    for name, root in (
        ("baseline", args.baseline.resolve()),
        ("candidate", args.candidate.resolve()),
    ):
        obj, binary = out / (name + ".o"), out / name
        subprocess.run(
            [
                args.cc,
                *flags,
                *renames,
                "-I" + str(root / "include"),
                "-c",
                str(root / "core/string.c"),
                "-o",
                str(obj),
            ],
            check=True,
        )
        subprocess.run(
            [
                args.cc,
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(source),
                str(obj),
                "-o",
                str(binary),
            ],
            check=True,
        )
        binaries[name] = binary
    results = []
    for n in (8, 64, 256, 4096, 65536):
        for align in (0, 3):
            cases = (
                ((-1, "overlap-forward"), (1, "overlap-backward"), (n + 16, "disjoint"))
                if args.case == "memmove"
                else ((0, "equal"), (0, "first"), (0, "last"))
            )
            for shift, label in cases:
                work = 1 if label == "first" else n
                iterations = max(2000, min(2000000, 33554432 // max(work, 1)))
                samples = {name: [] for name in binaries}
                checksums = set()
                for run in range(args.runs):
                    order = ("baseline", "candidate") if run % 2 == 0 else ("candidate", "baseline")
                    for name in order:
                        value = json.loads(
                            subprocess.check_output(
                                [
                                    str(binaries[name]),
                                    args.case,
                                    str(n),
                                    str(shift),
                                    str(align),
                                    str(iterations),
                                    label if args.case == "memcmp" else "equal",
                                ],
                                text=True,
                            )
                        )
                        samples[name].append(value["ns_per_call"])
                        checksums.add(value["checksum"])
                if len(checksums) != 1:
                    raise RuntimeError("baseline/candidate checksum mismatch")
                medians = {name: statistics.median(values) for name, values in samples.items()}
                results.append(
                    dict(
                        size=n,
                        alignment=align,
                        case=label,
                        iterations=iterations,
                        samples_ns=samples,
                        median_ns=medians,
                        time_change_pct=100 * (medians["candidate"] / medians["baseline"] - 1),
                        checksum=checksums.pop(),
                    )
                )
    print(
        json.dumps(
            dict(
                kind="native string microbenchmark",
                operation=args.case,
                compiler=subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
                platform=platform.platform(),
                affinity=sorted(os.sched_getaffinity(0)),
                flags=flags,
                runs=args.runs,
                results=results,
            ),
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
