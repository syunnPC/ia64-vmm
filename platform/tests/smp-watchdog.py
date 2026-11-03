#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check progress renewal and case diagnostics in private test binaries."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cflags", required=True)
    parser.add_argument("--ldflags", default="")
    parser.add_argument("objects", nargs="+")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="smp-watchdog-") as directory:
        obj = Path(directory) / "probe.o"
        binary = Path(directory) / "platform"
        cc = shlex.split(args.cc)
        subprocess.run(
            cc
            + shlex.split(args.cflags)
            + ["-c", "tests/host/smp/watchdog_probe.c", "-o", str(obj)],
            check=True,
        )
        subprocess.run(
            cc
            + args.objects
            + [str(obj)]
            + shlex.split(args.ldflags)
            + [
                "-pthread",
                "-Wl,--wrap=alarm,--wrap=platform_irq_inject,--wrap=hal_cpu_request,"
                "--wrap=hal_port_in,--wrap=engine_run",
                "-o",
                str(binary),
            ],
            check=True,
        )
        for case in (
            "iosapic",
            "latch",
            "ownership",
            "pci-race",
            "iosapic-intx",
            "teardown-1",
            "teardown-2",
            "teardown-3",
            "teardown-4",
        ):
            for stall in (False, True):
                env = dict(os.environ)
                # Setup and teardown alone cannot satisfy a long work loop.
                env["SMP_WATCHDOG_PROBE_MIN_ARMS"] = (
                    "4" if case in ("iosapic", "latch", "pci-race", "iosapic-intx") else "2"
                )
                if stall:
                    env["SMP_WATCHDOG_PROBE_STALL"] = "1"
                start, load = time.monotonic(), os.getloadavg()
                result = subprocess.run(
                    [str(binary), case],
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=30 if stall else None,
                )
                print(
                    f"{case} stall={stall} rc={result.returncode} "
                    f"seconds={time.monotonic() - start:.3f} load={load}/{os.getloadavg()}",
                    flush=True,
                )
                if result.returncode != (124 if stall else 0) or (
                    stall and f"SMP watchdog: test={case} (no progress)" not in result.stderr
                ):
                    raise RuntimeError(result.stdout + result.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
