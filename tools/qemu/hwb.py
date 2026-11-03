#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the bare-metal HWB success and fatal-fault checks, one VM at a time."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=["win-qemu", "vmware", "wsl-qemu"], required=True)
    parser.add_argument(
        "--cpus", type=int, default=4, help="host CPUs: management, executors and helpers"
    )
    parser.add_argument("--vcpus", type=int, default=2, help="executors, one window instance each")
    args = parser.parse_args()
    if not 2 <= args.vcpus < args.cpus:
        parser.error("needs 2 <= --vcpus < --cpus (the management CPU runs no executor)")
    output = ROOT / "build/hwb"
    output.mkdir(parents=True, exist_ok=True)
    results = []
    for test, extra in [
        ("hostwin", ""),
        ("hostwin-fatal", ""),
        ("hostwin-fatal-outside", " hwb_fatal_outside=1"),
    ]:
        mode = "hostwin" if test == "hostwin" else "hostwin-fatal"
        log = output / f"{test}-{args.backend}-{args.vcpus}v.log"
        command = [
            "tools/qemu/run.sh",
            "--backend",
            args.backend,
            "--cpus",
            str(args.cpus),
            "--memory",
            "2G",
            "--cmdline",
            f"reserve_mib=512 vcpus={args.vcpus} selftest={mode}{extra}",
            "--timeout",
            "120",
            "--log",
            str(log),
        ]
        load = os.getloadavg()
        print(f"HWB command: {command}; load={load}", flush=True)
        started = time.monotonic()
        result = subprocess.run(command, cwd=ROOT)
        text = log.read_text(errors="replace")
        required = [
            "HWB local passed",
            "HWB remote passed",
            "HWB roots passed",
            "HWB terminal target passed",
            "HWB product passed",
        ]
        if test == "hostwin":
            expected = 0
            required.append("HWB selftest passed: fills>0 backend_failures=0")
        else:
            expected = 1
            required += [
                "HWB fatal test: client installed;",
                "host exc: vector=14 #PF",
                "panic: fatal host exception",
                "fault outside all reservations"
                if extra
                else "declined in-reservation fault at non-site RIP",
            ]
        passed = (
            result.returncode == expected
            and all(marker in text for marker in required)
            and "panic: HWB selftest:" not in text
        )
        results.append(
            dict(
                test=test,
                backend=args.backend,
                command=command,
                load=load,
                seconds=time.monotonic() - started,
                status=result.returncode,
                passed=passed,
            )
        )
        (output / f"results-{args.backend}-{args.vcpus}v.json").write_text(
            json.dumps(results, indent=2) + "\n"
        )
        if not passed:
            raise SystemExit(
                f"HWB {test} failed: expected status={expected}, got {result.returncode}; {log}"
            )
    print(f"HWB passed on {args.backend} with {args.vcpus} executors", flush=True)


if __name__ == "__main__":
    main()
