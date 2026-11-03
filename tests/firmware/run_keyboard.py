#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Validate consumed BDS keys with PS/2 or an exclusive USB keyboard."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/loaders"))
from run import consumed_keys
from screen import edk2_menu_matches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", action="append", choices=["win-qemu", "vmware", "wsl-qemu"])
    parser.add_argument("--engine", action="append", choices=["interp", "tier0"])
    parser.add_argument("--keyboard", action="append", choices=["ps2", "xhci", "ehci"])
    parser.add_argument("--output", type=Path, default=ROOT / "build/firmware-media/keyboard")
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("--output must be under build/")
    output.mkdir(parents=True, exist_ok=True)
    records = (
        json.loads((output / "results.json").read_text())
        if args.resume and (output / "results.json").exists()
        else []
    )
    binary = hashlib.sha256(
        (ROOT / "build/firmware-new/ia64-firmware-new.elf").read_bytes()
    ).hexdigest()
    golden = (ROOT / "tests/efi/hello/hello-uefi27.golden").read_text()
    for backend in args.backend or ["win-qemu", "vmware"]:
        for engine in args.engine or ["interp", "tier0"]:
            for keyboard in args.keyboard or ["ps2", "xhci", "ehci"]:
                if backend == "vmware" and keyboard != "ps2":
                    continue
                previous = [
                    r
                    for r in records
                    if (r["backend"], r["engine"], r["keyboard"], r["firmware_sha256"])
                    == (backend, engine, keyboard, binary)
                ]
                if previous and previous[-1]["passed"]:
                    continue
                attempt = 1 + sum(
                    (r["backend"], r["engine"], r["keyboard"]) == (backend, engine, keyboard)
                    for r in records
                )
                prefix = output / f"{backend}-{keyboard}-{engine}-a{attempt}"
                command = [
                    sys.executable,
                    str(ROOT / "tools/qemu/run.py"),
                    "--no-build",
                    "--firmware",
                    "new",
                    "--backend",
                    backend,
                    "--disk",
                    str(ROOT / "build/media-hello-fat.img"),
                    "--cdrom",
                    str(ROOT / "build/media-hello.iso"),
                    "--timeout",
                    "360",
                    "--cmdline",
                    f"engine={engine} timeout_sec=300 halt_retry_us=0 code_validity=compare-always bootmenu=1",
                    "--log",
                    str(prefix.with_suffix(".log")),
                    "--screenshot",
                    str(prefix.with_suffix(".png")),
                    "--screenshot-on",
                    "BDS: Boot",
                    "--screenshot-delay",
                    ".25",
                    "--sendkey-after",
                    "BDS: Boot",
                    "--sendkey-delay",
                    ".75",
                    "--sendkey-interval",
                    ".6",
                    "--sendkey",
                    "down",
                    "--sendkey",
                    "up",
                    "--sendkey",
                    "ret",
                ]
                if keyboard != "ps2":
                    command += ["--usb-keyboard", "--usb-controller", keyboard]
                print("RUN " + prefix.name, flush=True)
                started = time.monotonic()
                with prefix.with_suffix(".runner.log").open("w") as runner:
                    status = subprocess.run(
                        command, cwd=ROOT, stdout=runner, stderr=subprocess.STDOUT
                    ).returncode
                serial = prefix.with_suffix(".log").read_text(errors="replace")
                keys, observed = consumed_keys(serial, ["down", "up", "ret"])
                pixels, text = edk2_menu_matches(
                    prefix.with_suffix(".png"),
                    ["IA-64 VMM boot menu", "UEFI"],
                    ROOT
                    / "third_party/edk2-ipf/MdeModulePkg/Universal/Console/GraphicsConsoleDxe/LaffStd.c",
                )
                prefix.with_suffix(".screen.txt").write_text(text + "\n")
                guest = "".join(
                    line[8:] + "\n" for line in serial.splitlines() if line.startswith("[guest] ")
                )
                result = dict(
                    backend=backend,
                    engine=engine,
                    keyboard=keyboard,
                    firmware_sha256=binary,
                    command=command,
                    status=status,
                    seconds=time.monotonic() - started,
                    keys=observed,
                    input_consumed=keys,
                    pixels=pixels,
                    selected_two="Selected 2:" in serial,
                    selected_one="Selected 1:" in serial,
                    golden=golden in guest,
                    success="EFI ResetSystem type=2 status=0" in serial,
                    log=str(prefix.with_suffix(".log").relative_to(ROOT)),
                    screenshot=str(prefix.with_suffix(".png").relative_to(ROOT)),
                )
                result["firmware_changed"] = (
                    hashlib.sha256(
                        (ROOT / "build/firmware-new/ia64-firmware-new.elf").read_bytes()
                    ).hexdigest()
                    != binary
                )
                result["passed"] = (
                    not result["firmware_changed"]
                    and status == 0
                    and all(
                        result[k]
                        for k in [
                            "input_consumed",
                            "pixels",
                            "selected_two",
                            "selected_one",
                            "golden",
                            "success",
                        ]
                    )
                )
                records.append(result)
                (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
                print(
                    json.dumps(
                        {
                            k: result[k]
                            for k in ["backend", "engine", "keyboard", "passed", "seconds"]
                        }
                    ),
                    flush=True,
                )
    latest = {
        (r["backend"], r["engine"], r["keyboard"]): r
        for r in records
        if r["firmware_sha256"] == binary
    }
    return 0 if latest and all(r["passed"] for r in latest.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
