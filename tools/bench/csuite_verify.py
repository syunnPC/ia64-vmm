#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks every compiled-C job's checksum (the interpreter's final r8,
recorded by build/bench-jobdump in build/bench/jobs/csuite-*.log) against
the same source compiled for the host (build/bench/csuite/csuite-native).
The wrapper and harness runs are then checked against the job's r8.
"""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def main():
    native = REPO / "build/bench/csuite/csuite-native"
    logs = sorted((REPO / "build/bench/jobs").glob("csuite-*.log"))
    if not logs:
        sys.exit("csuite_verify: no csuite job logs (run bench_engines.py prepare --suite csuite)")
    bad = checked = 0
    for log in logs:
        for line in log.read_text().splitlines():
            f = line.split()
            if len(f) < 10 or f[0] != "jobdump":
                continue
            kernel = f[1].split(":")[1]
            iters, r8 = f[5], int(f[9], 16)
            out = subprocess.run(
                [str(native), kernel, iters], capture_output=True, text=True, check=True
            )
            want = int(out.stdout.split()[3], 16)
            checked += 1
            status = "ok" if want == r8 else "MISMATCH"
            bad += want != r8
            print(
                f"{log.stem:20} kernel {kernel} iters {iters:>4} ia64 {r8:#018x} native {want:#018x} {status}"
            )
    print(f"csuite_verify: {checked} checksums, {bad} mismatches")
    sys.exit(1 if bad or not checked else 0)


if __name__ == "__main__":
    main()
