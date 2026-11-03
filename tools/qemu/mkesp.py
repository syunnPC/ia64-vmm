#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage a Limine UEFI ESP; optional modules may arrive from concurrent jobs."""

import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qemu.config import ROOT, load_local_env
from qemu.bootmedia import stage_bootmedia


def main():
    load_local_env()
    root = ROOT
    build = root / "build"
    esp = build / "esp"
    esp.mkdir(parents=True, exist_ok=True)
    stage_bootmedia(root, esp)
    shutil.copyfile(build / "vmm.elf", esp / "vmm.elf")
    cmdline = os.environ.get("VMM_CMDLINE", "exit=isa-debug-exit reserve_mib=128")
    if "\n" in cmdline or "\r" in cmdline or len(cmdline.encode()) > 511:
        raise SystemExit("VMM_CMDLINE must be one line of at most 511 bytes")
    config = [
        "timeout: 0",
        "",
        "/IA-64 VMM",
        "    protocol: limine",
        "    path: boot():/vmm.elf",
        f"    cmdline: {cmdline}",
    ]
    # GOP mode inherited from Limine, in WxH[xBPP] form.
    resolution = os.environ.get("VMM_LIMINE_RESOLUTION", "")
    if resolution:
        if not all(part.isdigit() for part in resolution.split("x")) or resolution.count(
            "x"
        ) not in (1, 2):
            raise SystemExit("VMM_LIMINE_RESOLUTION must be WxH or WxHxBPP")
        config.append(f"    resolution: {resolution}")
    # Options-file tokens precede the entry's command line (core/options.h).
    options = os.environ.get("VMM_OPTIONS_FILE", "")
    options_dest = esp / "vmm-options.conf"
    if options:
        source = Path(options)
        if not source.is_file():
            raise SystemExit(f"VMM_OPTIONS_FILE does not name a file: {options}")
        shutil.copyfile(source, options_dest)
        config.extend(["    module_path: boot():/vmm-options.conf", "    module_string: options"])
    else:
        options_dest.unlink(missing_ok=True)
    for filename, name in [
        ("firmware.elf", "firmware"),
        ("bootdisk.img", "bootdisk"),
        ("config", "config"),
    ]:
        source, dest = build / filename, esp / filename
        if name == "bootdisk" and os.environ.get("VMM_PHYSICAL_MEDIA") == "1":
            if dest.exists():
                dest.unlink()
            continue
        if source.is_file():
            shutil.copyfile(source, dest)
            config.extend([f"    module_path: boot():/{filename}", f"    module_string: {name}"])
        elif dest.exists():
            dest.unlink()
    (esp / "limine.conf").write_text("\n".join(config) + "\n")
    image = build / "esp.img.tmp"
    used = sum(p.stat().st_size for p in esp.rglob("*") if p.is_file())
    size = max(64 * 1024 * 1024, ((used * 2 + 1024 * 1024 - 1) // (1024 * 1024)) * 1024 * 1024)
    with image.open("wb") as file:
        file.truncate(size)
    subprocess.run(["mformat", "-i", str(image), "-F", "::"], check=True)
    subprocess.run(["mcopy", "-i", str(image), "-s", str(esp / "EFI"), "::/"], check=True)
    for path in sorted(esp.iterdir()):
        if path.is_file():
            subprocess.run(["mcopy", "-i", str(image), str(path), "::/"], check=True)
    image.replace(build / "esp.img")
    print(f"esp: {size // 1048576} MiB FAT32, {used} bytes staged")


if __name__ == "__main__":
    main()
