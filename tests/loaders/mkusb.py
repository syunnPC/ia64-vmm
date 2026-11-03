#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Build a Debian IA-64 MBR/FAT32 USB loader fixture from unchanged ISO files."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu.config import load_local_env, required


def main():
    load_local_env()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--iso", type=Path, help="image (default: the debian row of IA64_MEDIA_MANIFEST)"
    )
    parser.add_argument("--output", type=Path, default=ROOT / "build/debian-usb.img")
    args = parser.parse_args()
    iso = args.iso or Path(
        json.loads(Path(required("IA64_MEDIA_MANIFEST")).read_text())["debian"]["iso"]
    )
    source = iso if iso.is_absolute() else Path(required("IA64_IMAGES_DIR")) / iso
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("--output must be under build/")
    if not source.is_file():
        parser.error("ISO file not found: " + str(source))
    output.parent.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location(
        "stick_builder", ROOT / "tools/usb-stick/build.py"
    )
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    with tempfile.TemporaryDirectory(prefix="usb-image-", dir=output.parent) as directory:
        tree = Path(directory)
        for name in [".disk", "boot", "install"]:
            subprocess.run(
                [
                    "xorriso",
                    "-osirrox",
                    "on",
                    "-indev",
                    str(source),
                    "-extract",
                    "/" + name,
                    str(tree / name),
                ],
                check=True,
            )
        subprocess.run(
            ["mcopy", "-i", str(tree / "boot/grub/efi.img"), "-s", "::/efi", str(tree)], check=True
        )
        boot = tree / "efi/boot/bootia64.efi"
        if not boot.is_file():
            raise RuntimeError("ISO has no /efi/boot/bootia64.efi")
        files = {
            str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(tree.rglob("*"))
            if p.is_file()
        }
        # Reuse the project's MBR/FAT32 ESP writer.
        builder.disk_image(tree, output)
        output.with_suffix(".json").write_text(
            json.dumps(
                {
                    "iso": str(source),
                    "iso_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                    "image_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                    "unchanged_iso_files": files,
                },
                indent=2,
            )
            + "\n"
        )
    print(output)


if __name__ == "__main__":
    main()
