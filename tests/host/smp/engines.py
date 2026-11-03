#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Threaded engine foundations; reuse the normal shim objects, no guest VM."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]


def run(command, **kwargs):
    print("command:", shlex.join(map(str, command)), "load:", os.getloadavg(), flush=True)
    started = time.monotonic()
    result = subprocess.run(command, cwd=ROOT, **kwargs)
    print(
        "rc:",
        result.returncode,
        "seconds:",
        round(time.monotonic() - started, 3),
        "load:",
        os.getloadavg(),
        flush=True,
    )
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tsan", action="store_true")
    parser.add_argument(
        "--stats", action="store_true", help="exercise counters emitted into shared RX code"
    )
    parser.add_argument(
        "--watchdog-check",
        action="store_true",
        help="check alarm renewal and a stalled worker diagnostic",
    )
    parser.add_argument(
        "--scaling", action="store_true", help="paired 1/4 executor compiled-C kernels"
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=60,
        help="seconds without completed work before the foundation watchdog fires",
    )
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("--timeout must be positive")
    if args.tsan:
        os.environ.setdefault("TSAN_OPTIONS", "halt_on_error=1")
    if args.scaling:
        args.stats = True
    if args.scaling and args.watchdog_check:
        parser.error("--watchdog-check requires the foundation suite")
    cc = shlex.split(os.environ.get("HOSTCC", "gcc"))
    flags = [
        "-std=c11",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Iinclude",
        "-I.",
        "-DIA64VMM_HOSTED=1",
        f"-DIA64VMM_TIER0_STATS={int(args.stats)}",
        "-DIA64VMM_TIER1_STATS=0",
    ]
    if not args.scaling:
        flags.append("-DIA64VMM_TIER0_TEST_HOOKS=1")
    if args.tsan:
        flags += ["-fsanitize=thread", "-fno-omit-frame-pointer", "-Wno-error=tsan"]
    with tempfile.TemporaryDirectory(prefix="threaded-engines-") as directory:
        out = Path(directory)
        sources = [
            p
            for folder in ("ia64", "ia64/decode", "engine/cache", "engine/interp", "engine/tier0")
            for p in sorted(ROOT.joinpath(folder).glob("*.c"))
        ]
        sources += [
            ROOT / p
            for p in (
                "engine/engine.c",
                "core/log.c",
                "core/guest_mem.c",
                "tests/hal/hal_linux.c",
                "tests/host/smp/scaling.c" if args.scaling else "tests/host/smp/engines.c",
            )
        ]
        if args.watchdog_check:
            sources.append(ROOT / "tests/host/smp/watchdog_probe.c")
        # Standalone objects keep TSAN and mutation builds out of normal build/.
        objects = {}
        for i, source in enumerate(sources):
            obj = out / f"{i}.o"
            if run(cc + flags + ["-c", str(source), "-o", str(obj)]):
                return 1
            objects[str(source.relative_to(ROOT))] = obj
        executable = out / "engines"
        linkflags = (
            ["-pthread"]
            + (
                []
                if args.scaling
                else ["-Wl,--wrap=hal_alloc_pages,--wrap=hal_free_pages,--wrap=tier0_check_code"]
            )
            + (["-fsanitize=thread"] if args.tsan else [])
        )
        if args.watchdog_check:
            linkflags.append("-Wl,--wrap=alarm")
        if run(cc + list(map(str, objects.values())) + linkflags + ["-o", str(executable)]):
            return 1
        if args.scaling:
            import json

            # Use four different cores when Linux exposes the topology;
            # adjacent logical CPUs can be sibling threads of two cores.
            cpus, cores = [], set()
            for cpu in sorted(os.sched_getaffinity(0)):
                topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
                try:
                    core = (
                        (topology / "physical_package_id").read_text().strip(),
                        (topology / "core_id").read_text().strip(),
                    )
                except OSError:
                    core = (0, cpu)
                if core not in cores:
                    cpus.append(cpu)
                    cores.add(core)
                if len(cpus) == 4:
                    break
            if len(cpus) < 4:
                raise RuntimeError("scaling requires four allowed host CPUs")
            rows = []
            output = ROOT / "build/tests/threaded-engines/scaling.json"
            for repeat in range(3):
                for kernel in range(6):
                    for count in (1, 4) if repeat % 2 == 0 else (4, 1):
                        command = [
                            str(executable),
                            str(count),
                            str(kernel),
                            str(ROOT / f"build/bench/jobs/csuite-{kernel}-slow.bin"),
                            str(ROOT / "build/bench/csuite/csuite.bin"),
                        ] + list(map(str, cpus[:count]))
                        before = os.getloadavg()
                        result = subprocess.run(
                            command, cwd=ROOT, text=True, capture_output=True, timeout=180
                        )
                        if result.returncode:
                            raise RuntimeError(result.stderr)
                        row = json.loads(result.stdout)
                        row.update(
                            repeat=repeat,
                            host_cpus=cpus[:count],
                            load_before=before,
                            load_after=os.getloadavg(),
                        )
                        rows.append(row)
                        output.write_text(json.dumps(rows, indent=2) + "\n")
                        print(row, flush=True)
            return 0
        env = dict(os.environ, SMP_ENGINE_WATCHDOG_SECONDS=str(args.timeout))
        if run([str(executable)], env=env):
            return 1
        if args.watchdog_check:
            start, load = time.monotonic(), os.getloadavg()
            stalled = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                env=dict(env, SMP_WATCHDOG_PROBE_STALL="1"),
                timeout=args.timeout + 30,
            )
            print(
                "watchdog stall rc:",
                stalled.returncode,
                "seconds:",
                time.monotonic() - start,
                "load:",
                load,
                os.getloadavg(),
                flush=True,
            )
            if (
                stalled.returncode != 124
                or "SMP watchdog: test=smp-engines (no progress)" not in stalled.stderr
            ):
                raise RuntimeError(stalled.stdout + stalled.stderr)
