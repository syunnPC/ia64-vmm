#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Time GRUB from live serial output; use the unchanged, shared VM runner."""

import argparse
import json
import pathlib
import re
import subprocess
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", choices=("interp", "tier0"), required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--seconds", type=int, default=90)
    args = parser.parse_args()
    output = ROOT / "build/tests/tier0"
    output.mkdir(parents=True, exist_ok=True)
    log = output / f"{args.label}-{args.engine}.serial.log"
    command = [
        "tools/qemu/run.sh",
        "--no-build",
        "--backend",
        "win-qemu",
        "--cdrom",
        "install-ia64-minimal-20240404T093405Z.iso",
        "--cmdline",
        f"exit=isa-debug-exit reserve_mib=128 engine={args.engine} timeout_sec={args.seconds}",
        "--timeout",
        str(args.seconds + 20),
        "--log",
        str(log),
    ]
    started = time.monotonic()
    process = subprocess.Popen(
        command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
    )
    lines = []
    run = []

    def reader():
        for line in process.stdout:
            lines.append(line)
            if line.startswith("run directory: "):
                run.append(pathlib.Path(line.strip().split(": ", 1)[1]))
            print(line, end="", flush=True)

    thread = threading.Thread(target=reader)
    thread.start()
    menu = first_serial = guest_start = None
    while process.poll() is None:
        if run:
            try:
                serial = (run[0] / "serial.log").read_bytes()
            except FileNotFoundError:
                serial = b""
            now = time.monotonic() - started
            if serial and first_serial is None:
                first_serial = now
            if guest_start is None and b"engine executor host CPU" in serial:
                guest_start = now
            # The Gentoo GRUB menu title follows startup and filesystem reads.
            if menu is None and b"Boot LiveCD (kernel: gentoo)" in serial:
                menu = now
                print(f"live GRUB menu: {menu:.3f}s", flush=True)
        time.sleep(0.02)
    thread.join()
    record = dict(
        engine=args.engine,
        label=args.label,
        command=command,
        exit=process.returncode,
        menu_seconds=menu,
        first_serial_seconds=first_serial,
        guest_start_seconds=guest_start,
        total_seconds=time.monotonic() - started,
    )
    if log.exists():
        serial = log.read_text(errors="replace")
        for name in (
            "regions",
            "publications",
            "evictions",
            "protection_changes",
            "lookups",
            "cross_page_validations",
        ):
            match = re.search(r"\b" + name + r"=(\d+)", serial)
            if match:
                record[name] = int(match[1])
    (output / f"{args.label}-{args.engine}.runner.log").write_text("".join(lines))
    (output / f"{args.label}-{args.engine}.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record), flush=True)


if __name__ == "__main__":
    main()
