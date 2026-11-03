#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot unchanged Linux media via real GRUB keyboard input with a verbose UART console."""

import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu.config import load_local_env, required


def typed_keys(text):
    special = {
        " ": "spc",
        "/": "slash",
        "=": "equal",
        ",": "comma",
        "-": "minus",
        ".": "dot",
        "_": "shift-minus",
        ";": "semicolon",
    }
    for char in text:
        if char in special:
            yield special[char]
        elif char.isupper():
            yield "shift-" + char.lower()
        elif char.isascii() and char.isalnum():
            yield char
        else:
            raise ValueError(f"unsupported HMP character {char!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=["debian", "gentoo"], required=True)
    parser.add_argument("--engine", choices=["interp", "tier0"], default="tier0")
    parser.add_argument("--guest-timeout", type=int, default=1000)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--kernel-args",
        default="loglevel=8 console=ttyS0,115200",
        help="kernel parameters typed after the media's own",
    )
    parser.add_argument("--cmdline-extra", default="", help="extra VMM command-line options")
    parser.add_argument("--firmware", choices=["reference", "new"], default="reference")
    parser.add_argument(
        "--firmware-image", type=Path, help="diagnostic firmware ELF for the private ESP only"
    )
    args = parser.parse_args()
    load_local_env()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("--output must be inside build/")
    if args.guest_timeout <= 0:
        parser.error("--guest-timeout must be positive")
    output.mkdir(parents=True, exist_ok=True)
    media = json.loads(Path(required("IA64_MEDIA_MANIFEST")).read_text())[args.target]
    iso = Path(required("IA64_IMAGES_DIR")) / media["iso"]
    parameters = args.kernel_args
    if args.target == "debian":
        commands = [f"linux /install/vmlinuz {parameters}", "initrd /install/initrd.gz", "boot"]
    else:
        commands = [
            f"linux /boot/gentoo dokeymap looptype=squashfs loop=/image.squashfs cdroot {parameters}",
            "initrd /boot/gentoo.igz",
            "boot",
        ]
    # One GRUB script line: keys typed while `linux` reads the kernel would
    # overflow the 16-byte PS/2 queue and be lost.
    keys = ["c", *typed_keys("; ".join(commands)), "ret"]
    command = [
        str(ROOT / "tools/qemu/run.sh"),
        "--no-build",
        "--backend",
        "win-qemu",
        "--cdrom",
        str(iso),
        "--firmware",
        args.firmware,
        "--memory",
        "4096",
        # console=ttyS0 needs the debug UART (PNP0501 + PCDP/SPCR console).
        "--cmdline",
        f"reserve_mib=128 engine={args.engine} timeout_sec={args.guest_timeout} "
        f"code_validity=compare-always debug_serial=console"
        + (" " + args.cmdline_extra if args.cmdline_extra else ""),
        "--timeout",
        str(args.guest_timeout + 120),
        "--log",
        str(output / "serial.log"),
        "--screenshot",
        str(output / "menu.png"),
        "--screenshot-on",
        media["gate"],
        "--screenshot-delay",
        "1",
        "--sendkey-after",
        media["gate"],
        "--sendkey-delay",
        "3",
        "--sendkey-interval",
        ".15",
    ]
    for key in keys:
        command += ["--sendkey", key]
    if args.firmware_image:
        command += ["--firmware-image", str(args.firmware_image.resolve())]
    (output / "command.json").write_text(
        json.dumps(dict(command=command, grub_commands=commands), indent=2) + "\n"
    )
    with (output / "runner.log").open("w") as log:
        result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    print("runner status", result.returncode, "; log", output / "serial.log")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
