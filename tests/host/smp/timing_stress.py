#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Repeat host SMP tests under bounded CPU load; report only to stdout.

Build first with make -j6 build/tests/smp-exec build/tests/hal-smp
build/tests/smp-purge build/tests/smp-platform. --bin-dir can select saved
baseline binaries; --hogs 0 reuses an already running load.
Every run prints elapsed time and all three load averages.
Only the exact PIDs of this script's own stress children are stopped.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hog", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--runs", type=int, default=20)
    parser.add_argument(
        "--jobs", type=int, default=1, help="independent host iterations in parallel (1..3)"
    )
    parser.add_argument("--hogs", type=int, default=4)
    parser.add_argument("--hog-seconds", type=int, default=1800)
    parser.add_argument(
        "--tests",
        nargs="+",
        choices=["smp-exec", "hal-smp", "smp-purge", "smp-platform"],
        default=["smp-exec", "hal-smp", "smp-purge", "smp-platform"],
    )
    parser.add_argument("--purge-cases", nargs="+", default=["stress", "1000", "8"])
    parser.add_argument(
        "--exec-cases",
        nargs="*",
        default=[],
        help="selected executor cases; default: complete suite",
    )
    parser.add_argument(
        "--hal-cases", nargs="*", default=[], help="selected HAL cases; default: complete suite"
    )
    parser.add_argument(
        "--platform-cases",
        nargs="+",
        default=[
            "iosapic",
            "latch",
            "ownership",
            "pci-race",
            "iosapic-intx",
            "teardown-1",
            "teardown-2",
            "teardown-3",
            "teardown-4",
        ],
    )
    parser.add_argument("--bin-dir", type=Path, default=Path("build/tests"))
    args = parser.parse_args()
    if args.hog:
        until = time.monotonic() + args.hog_seconds
        while time.monotonic() < until:
            pass
        return 0
    if args.jobs < 1 or args.jobs > 3:
        parser.error("--jobs must be 1..3")
    if args.runs < 1 or args.hogs < 0 or args.hog_seconds < 1:
        parser.error("--runs and --hog-seconds must be positive; --hogs must be nonnegative")
    cases = {"smp-exec": args.exec_cases, "hal-smp": args.hal_cases, "smp-purge": args.purge_cases}
    commands = [
        (name, case)
        for name in args.tests
        for case in (
            [[case] for case in args.platform_cases] if name == "smp-platform" else [cases[name]]
        )
    ]
    for name, _ in commands:
        if not (args.bin_dir / name).is_file():
            parser.error(f"missing {args.bin_dir / name}: build the test binaries first")
    hogs = []
    failures = {name: 0 for name, _ in commands}
    try:
        for _ in range(args.hogs):
            hogs.append(
                subprocess.Popen(
                    [
                        sys.executable,
                        str(Path(__file__).resolve()),
                        "--hog",
                        "--hog-seconds",
                        str(args.hog_seconds),
                    ],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            )
        print(
            json.dumps(
                dict(
                    affinity=sorted(os.sched_getaffinity(0)),
                    hog_pids=[p.pid for p in hogs],
                    runs=args.runs,
                    jobs=args.jobs,
                    commands=commands,
                )
            ),
            flush=True,
        )
        record_lock = threading.Lock()

        def iteration(run):
            for name, cases in commands:
                if any(proc.poll() is not None for proc in hogs):
                    raise RuntimeError("load lifetime exhausted; increase --hog-seconds")
                start, load = time.monotonic(), os.getloadavg()
                result = subprocess.run(
                    [str((args.bin_dir / name).resolve()), *cases],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                )
                if any(proc.poll() is not None for proc in hogs):
                    raise RuntimeError("load lifetime exhausted; increase --hog-seconds")
                row = dict(
                    run=run,
                    test=name,
                    cases=cases,
                    rc=result.returncode,
                    seconds=time.monotonic() - start,
                    load_start=load,
                    load_end=os.getloadavg(),
                )
                with record_lock:
                    failures[name] += result.returncode != 0
                    if result.stdout:
                        print(result.stdout.decode(errors="replace").rstrip(), flush=True)
                    print(json.dumps(row), flush=True)

        if args.jobs == 1:
            for run in range(1, args.runs + 1):
                iteration(run)
        else:
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                list(pool.map(iteration, range(1, args.runs + 1)))
        print("failures:", failures, flush=True)
    finally:
        for proc in hogs:
            if proc.poll() is None:
                proc.terminate()
        for proc in hogs:
            proc.wait()
    return int(any(failures.values()))


if __name__ == "__main__":
    sys.exit(main())
