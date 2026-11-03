#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build separate VMM and guest-test disks for a Generation 2 Hyper-V VM."""

import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("usb_build", ROOT / "tools/usb-stick/build.py")
usb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(usb)


def convert(source, destination):
    temporary = destination.with_name(destination.name + ".tmp")
    try:
        subprocess.run(
            [
                "qemu-img",
                "convert",
                "-f",
                "raw",
                "-O",
                "vhdx",
                "-o",
                "subformat=dynamic,block_size=1048576",
                str(source),
                str(temporary),
            ],
            check=True,
        )
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def build(output):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hyperv-", dir=output) as directory:
        work = Path(directory)
        tree = work / "esp"
        usb.stage_bootmedia(ROOT, tree)
        for source, name in [
            ("build/vmm.elf", "vmm.elf"),
            ("build/firmware-new/release/ia64-firmware-new.elf", "firmware-new.elf"),
            ("build/efi/hello.efi", "hello.efi"),
            ("build/vmmcfg/vmmcfg.efi", usb.CONFIGURE_PATH),
        ]:
            shutil.copyfile(ROOT / source, tree / name)
        (tree / usb.OPTIONS_PATH).write_text(
            "placement=logical-thread\nengine=tier0\nmedia_log=1\nbootmenu=0\n"
        )
        (tree / "limine.conf").write_text(usb.entries({}))
        usb.copy_notices(tree)
        vmm = work / "vmm.img"
        usb.disk_image(tree, vmm)
        convert(vmm, output / "ia64-vmm.vhdx")

        guest = work / "guest.img"
        with guest.open("wb") as file:
            file.truncate(16 * 1024 * 1024)
        subprocess.run(["mkfs.fat", "-F", "16", str(guest)], check=True)
        subprocess.run(["mmd", "-i", str(guest), "::/EFI", "::/EFI/BOOT"], check=True)
        subprocess.run(
            [
                "mcopy",
                "-i",
                str(guest),
                str(ROOT / "build/efi/hello.efi"),
                "::/EFI/BOOT/BOOTIA64.EFI",
            ],
            check=True,
        )
        convert(guest, output / "guest-test.vhdx")
    print(f"Hyper-V disks: {output / 'ia64-vmm.vhdx'}, {output / 'guest-test.vhdx'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/hyperv")
    args = parser.parse_args()
    build(args.output_dir.resolve())


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(f"hyperv-image: {error}")
