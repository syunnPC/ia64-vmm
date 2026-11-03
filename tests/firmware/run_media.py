#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Boot hello from directly assigned media with the new firmware, serially."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/loaders"))
from screen import edk2_menu_matches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", action="append", choices=["win-qemu", "vmware", "wsl-qemu"])
    parser.add_argument("--engine", action="append", choices=["interp", "tier0"])
    parser.add_argument(
        "--medium",
        action="append",
        choices=[
            "sata-disk",
            "sata-cd",
            "usb-disk-xhci",
            "usb-cd-xhci",
            "usb-disk-ehci",
            "usb-cd-ehci",
            "nvme",
        ],
    )
    parser.add_argument("--disk", type=Path, default=ROOT / "build/media-hello-fat.img")
    parser.add_argument("--cd", type=Path, default=ROOT / "build/media-hello.iso")
    parser.add_argument("--output", type=Path, default=ROOT / "build/firmware-media")
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument(
        "--keep", action="store_true", help="retain private backend files for diagnosis"
    )
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("--output must be under build/")
    if not args.disk.is_file() or not args.cd.is_file():
        parser.error("prepare --disk and --cd hello fixtures first")
    output.mkdir(parents=True, exist_ok=True)
    results = (
        json.loads((output / "media.json").read_text())
        if args.resume and (output / "media.json").exists()
        else []
    )
    binary = hashlib.sha256(
        (ROOT / "build/firmware-new/ia64-firmware-new.elf").read_bytes()
    ).hexdigest()
    done = {
        (r["backend"], r["engine"], r["medium"], r.get("firmware_sha256"))
        for r in results
        if r["passed"]
    }
    golden = (ROOT / "tests/efi/hello/hello-uefi27.golden").read_text()
    for backend in args.backend or ["win-qemu", "vmware"]:
        for engine in args.engine or ["interp", "tier0"]:
            for medium in args.medium or [
                "sata-disk",
                "sata-cd",
                "usb-disk-xhci",
                "usb-cd-xhci",
                "usb-disk-ehci",
                "usb-cd-ehci",
                "nvme",
            ]:
                if backend == "vmware" and medium.startswith("usb-"):
                    continue
                if (backend, engine, medium, binary) in done:
                    continue
                attempt = 1 + sum(
                    r["backend"] == backend and r["engine"] == engine and r["medium"] == medium
                    for r in results
                )
                prefix = output / f"{backend}-{medium}-{engine}-a{attempt}"
                log, screen = prefix.with_suffix(".log"), prefix.with_suffix(".png")
                screen.unlink(missing_ok=True)
                option = {"sata-disk": "--disk", "sata-cd": "--cdrom", "nvme": "--nvme"}.get(
                    medium, "--usb-disk" if medium.startswith("usb-disk") else "--usb-cdrom"
                )
                media = args.cd if option in ["--cdrom", "--usb-cdrom"] else args.disk
                command = [
                    sys.executable,
                    str(ROOT / "tools/qemu/run.py"),
                    "--firmware",
                    "new",
                    "--no-build",
                    "--backend",
                    backend,
                    option,
                    str(media.resolve()),
                    "--timeout",
                    str(args.timeout + 60),
                    "--cmdline",
                    f"engine={engine} timeout_sec={args.timeout} halt_retry_us=0 code_validity=compare-always",
                    "--log",
                    str(log),
                    "--screenshot",
                    str(screen),
                    "--screenshot-on",
                    "BDS: Boot",
                    "--screenshot-delay",
                    "0.25",
                ]
                if medium.endswith(("xhci", "ehci")):
                    command += ["--usb-controller", medium.rsplit("-", 1)[1]]
                if args.keep:
                    command += ["--keep"]
                print("RUN " + prefix.name, flush=True)
                started = time.monotonic()
                with prefix.with_suffix(".runner.log").open("w") as stream:
                    status = subprocess.run(
                        command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT
                    ).returncode
                serial = log.read_text(errors="replace") if log.is_file() else ""
                guest = "".join(
                    line[8:] + "\n" for line in serial.splitlines() if line.startswith("[guest] ")
                )
                visual, text = edk2_menu_matches(
                    screen,
                    ["IA-64 VMM boot menu", "UEFI"],
                    ROOT
                    / "third_party/edk2-ipf/MdeModulePkg/Universal/Console/GraphicsConsoleDxe/LaffStd.c",
                )
                prefix.with_suffix(".screen.txt").write_text(text + "\n")
                result = dict(
                    backend=backend,
                    engine=engine,
                    medium=medium,
                    status=status,
                    seconds=time.monotonic() - started,
                    command=command,
                    log=str(log.relative_to(ROOT)),
                    screenshot=str(screen.relative_to(ROOT)),
                    firmware_sha256=binary,
                    golden=golden in guest,
                    menu="BDS: Boot" in guest,
                    success="EFI ResetSystem type=2 status=0" in serial,
                    pixels=visual,
                )
                # ESP staging can rebuild firmware even with --no-build. Do not
                # attribute a boot to a hash sampled before that rebuild.
                result["firmware_changed"] = (
                    hashlib.sha256(
                        (ROOT / "build/firmware-new/ia64-firmware-new.elf").read_bytes()
                    ).hexdigest()
                    != binary
                )
                result["passed"] = (
                    not result["firmware_changed"]
                    and status == 0
                    and all(result[key] for key in ["golden", "menu", "success", "pixels"])
                )
                results.append(result)
                (output / "media.json").write_text(json.dumps(results, indent=2) + "\n")
                print(
                    json.dumps(
                        {
                            key: result[key]
                            for key in [
                                "backend",
                                "engine",
                                "medium",
                                "status",
                                "seconds",
                                "passed",
                            ]
                        }
                    ),
                    flush=True,
                )
    latest = {
        (r["backend"], r["engine"], r["medium"]): r
        for r in results
        if r.get("firmware_sha256") == binary
    }
    return 0 if latest and all(r["passed"] for r in latest.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
