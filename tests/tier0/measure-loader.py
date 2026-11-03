#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure the unchanged Gentoo row using a temporary counter-logging VMM."""

import argparse
import importlib.util
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]


def summarize(output, backend, timing):
    log = (output / f"gentoo-{backend}-tier0-scripted.log").read_text(errors="replace")
    split = log.index("kernel entry retired=")
    retired = int(re.search(r"kernel entry retired=(\d+)", log[split:])[1])
    phases = re.split(r"tier0 executed=(\d+)", log[split:])
    executed = int(phases[-2]) - int(phases[1])
    totals = []
    for part in (phases[2], phases[-1]):
        values = [
            int(value)
            for value in re.findall(r"tier0 interpreter reason=\S+ instructions=(\d+)", part)
        ]
        if len(values) != 5:
            raise RuntimeError("missing interpreter reason counters")
        totals.append(values)
    interpreter = sum(totals[1]) - sum(totals[0])
    final_retired = int(re.findall(r"vcpu0 IP=\S+ PSR=\S+ CFM=\S+ retired=(\d+)", log)[-1])
    # A zero interpreter delta proves all kernel instructions are native, even
    # when the interval also contains firmware refill-handler instructions.
    result = dict(
        timing=timing,
        entry_retired=retired,
        final_retired=final_retired,
        interval_native=executed,
        interval_interpreter=interpreter,
        kernel_native_share=1.0 if interpreter == 0 else None,
        note="Interval counts include any firmware refills after kernel entry.",
    )
    assert executed + interpreter == final_retired - retired
    (output / "kernel-counters.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Tier0 kernel measurement: " + json.dumps(result), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=["win-qemu", "vmware"], default="win-qemu")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/tests/tier0/gentoo-counter-probe"
    )
    parser.add_argument("--guest-timeout", type=int, default=400)
    parser.add_argument("--summarize-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.summarize_only:
        summarize(output, args.backend, json.loads((output / "timing.json").read_text()))
        return
    spec = importlib.util.spec_from_file_location("loader_row", ROOT / "tests/loaders/run.py")
    loader = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(ROOT / "tests/loaders"))
    spec.loader.exec_module(loader)
    original_run = subprocess.run
    timing = {}

    def measured_run(command, **kwargs):
        if not command or not str(command[0]).endswith("tools/qemu/run.sh"):
            return original_run(command, **kwargs)
        started = time.monotonic()
        runner_log = Path(kwargs["stdout"].name)
        process = subprocess.Popen(command, **kwargs)
        serial = None
        while process.poll() is None:
            runner = runner_log.read_text(errors="replace") if runner_log.exists() else ""
            if "backend_seconds" not in timing and (
                ": accelerator=" in runner or "VMware:" in runner
            ):
                timing["backend_seconds"] = time.monotonic() - started
            if serial is None:
                match = re.search(r"^run directory: (.+)$", runner, re.M)
                if match:
                    serial = Path(match[1]) / "serial.log"
            if serial is not None and serial.exists() and "kernel_seconds" not in timing:
                log = serial.read_text(errors="replace")
                if "EFI loader done ip=4e84520 slot=0" in log:
                    timing["kernel_seconds"] = time.monotonic() - started
            time.sleep(0.02)
        timing["total_seconds"] = time.monotonic() - started
        (output / "timing.json").write_text(json.dumps(timing, indent=2) + "\n")
        return subprocess.CompletedProcess(command, process.returncode)

    # Compile only a /tmp copy: the production core/ source and runner stay unchanged.
    source = (ROOT / "core/vm.c").read_text()
    old = "            platform->handoff_pending = false;\n            break;"
    new = """            LOGI("kernel entry retired=%llu", (unsigned long long)v->retired);
            tier0_dump_stats();
            platform->handoff_pending = false;
            break;"""
    assert source.count(old) == 1
    source = '#include "engine/tier0/tier0.h"\n' + source.replace(old, new)
    vmm = ROOT / "build/vmm.elf"
    backup = output / "normal-vmm.elf"
    shutil.copy2(vmm, backup)
    with tempfile.TemporaryDirectory(prefix="tier0-loader-probe-") as temp:
        temp = Path(temp)
        probe_source = temp / "vm.c"
        probe_source.write_text(source)
        (output / "vm-probe.c").write_text(source)
        compile_command = shlex.split((ROOT / "build/bm/.flags").read_text())
        compile_command += ["-c", str(probe_source), "-o", str(temp / "vm.o")]
        dry = subprocess.check_output(["make", "-n", "-B", "build/vmm.elf"], cwd=ROOT, text=True)
        line = next(
            line for line in reversed(dry.splitlines()) if "-Wl,-T,host/amd64/linker.ld" in line
        )
        link_command = [
            str(temp / "vm.o")
            if token == "build/bm/core/vm.o"
            else str(output / "probe-vmm.elf")
            if token == "build/vmm.elf"
            else token
            for token in shlex.split(line)
        ]
        with (output / "probe-build.log").open("w") as log:
            original_run(
                compile_command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True
            )
            original_run(link_command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        (output / "probe-commands.json").write_text(
            json.dumps([compile_command, link_command], indent=2) + "\n"
        )
        try:
            shutil.copyfile(output / "probe-vmm.elf", vmm)
            subprocess.run = measured_run
            sys.argv = [
                str(ROOT / "tests/loaders/run.py"),
                "--target",
                "gentoo",
                "--backend",
                args.backend,
                "--engine",
                "tier0",
                "--scenario",
                "scripted",
                "--output",
                str(output),
                "--guest-timeout",
                str(args.guest_timeout),
            ]
            loader.main()
        finally:
            shutil.copy2(backup, vmm)
            subprocess.run = original_run
    summarize(output, args.backend, timing)


if __name__ == "__main__":
    main()
