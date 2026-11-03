#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Gentoo VGA acceptance; explicit topology and private MKS refresh only.

Legacy VGA: --display vbios adds
display=vbios (the VMM POSTs the card's legacy VBIOS) and needs a backend
whose CPU offers SVM: wsl-qemu (TCG -cpu max) with --vga cirrus."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu.config import load_local_env, required
from qemu.windows import local_path, windows_path
from vmware.backend import capture_screen
from qemu.run import guest_serial_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=["win-qemu", "vmware", "wsl-qemu"], required=True)
    parser.add_argument("--firmware", choices=["reference", "new"], default="reference")
    parser.add_argument("--debug-serial", choices=["off", "console"], default="off")
    parser.add_argument("--guest-timeout", type=int, help="bounded guest duration in seconds")
    parser.add_argument("--out", type=Path, default=ROOT / "build/guest-display")
    parser.add_argument(
        "--vga", choices=["cirrus", "std"], help="QEMU display device (runner default: std)"
    )
    parser.add_argument(
        "--display",
        choices=["gop", "vbios"],
        default="gop",
        help="VMM display policy; vbios POSTs the legacy VBIOS",
    )
    args = parser.parse_args()
    duration = (
        args.guest_timeout
        if args.guest_timeout is not None
        else (900 if args.firmware == "new" else 320)
    )
    if duration < 1:
        parser.error("--guest-timeout must be positive")
    load_local_env()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    vmrun = local_path(required("VMWARE_DIR")) / "vmrun.exe" if args.backend == "vmware" else None
    suffix = "-new" if args.firmware == "new" else ""
    prefix = args.out / (
        "gentoo-"
        + args.backend
        + suffix
        + "-"
        + args.debug_serial
        + (f"-{args.vga}" if args.vga else "")
        + ("-vbios" if args.display == "vbios" else "")
    )
    # A bounded retry must not accept a previous run's image or serial log.
    for extension in [".png", ".log"]:
        Path(str(prefix) + extension).unlink(missing_ok=True)
    command = [
        sys.executable,
        "tools/qemu/run.py",
        "--backend",
        args.backend,
        "--firmware",
        args.firmware,
        "--no-build",
        "--debug-serial",
        args.debug_serial,
        "--cdrom",
        "install-ia64-minimal-20240404T093405Z.iso",
        "--cmdline",
        f"reserve_mib=128 engine=tier0 timeout_sec={duration} code_validity=compare-always handoff_steps=33554432"
        + (" display=vbios" if args.display == "vbios" else ""),
        "--timeout",
        str(duration + 60),
        "--sendkey-after",
        "Special console options",
        "--sendkey-delay",
        "3",
        "--sendkey",
        "ret",
        "--screenshot",
        str(prefix) + ".png",
        "--screenshot-on",
        "EFI loader done",
        "--screenshot-delay",
        "70",
        "--log",
        str(prefix) + ".log",
        "--keep",
    ]
    if args.backend in ("win-qemu", "wsl-qemu"):
        command += ["--machine", "pc"]
    if args.vga:
        command += ["--vga", args.vga]
    output = Path(str(prefix) + ".runner.log")
    print("Command:", command, flush=True)
    refreshed = False
    capture_error = None
    next_capture = 0
    with output.open("w") as sink:
        process = subprocess.Popen(command, cwd=ROOT, stdout=sink, stderr=subprocess.STDOUT)
        while process.poll() is None:
            # Headless MKS first captures its old surface, then refreshes VGA.
            # Repeat only this test's own capture; runner defaults stay intact.
            text = output.read_text(errors="replace")
            if (
                vmrun
                and not refreshed
                and time.monotonic() >= next_capture
                and "vmware: screenshot captured" in text
            ):
                match = re.search(r"^run directory: (.+)$", text, re.M)
                if match:
                    temp = Path(match[1])
                    time.sleep(2)
                    try:
                        capture_screen(
                            vmrun,
                            temp,
                            windows_path(temp / "vmm.vmx"),
                            temp / "screen.png",
                            time.monotonic() + 20,
                        )
                    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
                        capture_error = str(error)
                        print(
                            "Own MKS capture failed; retrying in five seconds:", error, flush=True
                        )
                    else:
                        shutil.copyfile(temp / "screen.png", Path(str(prefix) + ".png"))
                        refreshed = True
                        capture_error = None
                        print("Refreshed own MKS VGA screenshot", flush=True)
                else:
                    capture_error = "Own private VM directory was not recorded"
                next_capture = time.monotonic() + 5
            time.sleep(0.25)
        code = process.wait()
    print("Runner exit:", code, "(5 is the bounded kernel timeout)", flush=True)
    print("Screenshot:", str(prefix) + ".png", flush=True)
    log = Path(str(prefix) + ".log")
    serial = guest_serial_file(log) if log.exists() else None
    raw = serial.read_bytes() if serial else b""
    metadata = json.loads(serial.with_suffix(serial.suffix + ".json").read_text()) if serial else {}
    serial_ok = (
        not raw if args.debug_serial == "off" else b"Linux version 6.6.24-gentoo-ia64" in raw
    )
    serial_ok = serial_ok and metadata.get("complete", False) and metadata.get("lost_bytes") == 0
    Path(str(prefix) + ".result.json").write_text(
        json.dumps(
            {
                "backend": args.backend,
                "firmware": args.firmware,
                "debug_serial": args.debug_serial,
                "command": command,
                "runner_exit": code,
                "mks_refreshed": refreshed,
                "capture_error": capture_error,
                "screenshot": str(prefix) + ".png",
                "log": str(prefix) + ".log",
                "guest_serial": str(serial) if serial else None,
                "guest_serial_bytes": len(raw),
                "serial_metadata": metadata,
                "serial_ok": serial_ok,
            },
            indent=2,
        )
        + "\n"
    )
    # Inspect PNG for kernel text; the image alone is not a full OS boot pass.
    capture_ok = args.backend != "vmware" or refreshed
    return (
        0
        if code in [0, 5] and capture_ok and serial_ok and Path(str(prefix) + ".png").exists()
        else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
