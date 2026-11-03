#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract IA-64 display configs from the supplied media, never the host kernel."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu.config import load_local_env, required

SOURCES = {
    "gentoo": ("/boot/gentoo-config", None),
    "debian": (
        "/pool-ia64/main/l/linux/linux-image-6.1.0-9-itanium_6.1.27-1_ia64.deb",
        "boot/config-6.1.0-9-itanium",
    ),
    "sles": (
        "/suse/ia64/kernel-default-base-3.0.101-63.1.ia64.rpm",
        "./boot/config-3.0.101-63-default",
    ),
}
OPTIONS = [
    "CONFIG_EFI",
    "CONFIG_EFI_PCDP",
    "CONFIG_VT",
    "CONFIG_VT_CONSOLE",
    "CONFIG_VGA_CONSOLE",
    "CONFIG_FB",
    "CONFIG_FRAMEBUFFER_CONSOLE",
    "CONFIG_FB_EFI",
    "CONFIG_DRM",
    "CONFIG_DRM_BOCHS",
    "CONFIG_DRM_VMWGFX",
    "CONFIG_DRM_I915",
    "CONFIG_DRM_AMDGPU",
    "CONFIG_SERIAL_8250_CONSOLE",
]


def cpio_file(data, wanted):
    at = 0
    while at + 110 <= len(data):
        h = data[at : at + 110]
        if h[:6] not in [b"070701", b"070702"]:
            raise ValueError("RPM payload is not newc cpio")
        size, length = int(h[54:62], 16), int(h[94:102], 16)
        name = data[at + 110 : at + 110 + length - 1].decode()
        start = (at + 110 + length + 3) & ~3
        if start + size > len(data):
            raise ValueError("truncated cpio payload")
        if name == wanted:
            return data[start : start + size]
        if name == "TRAILER!!!":
            break
        at = (start + size + 3) & ~3
    raise ValueError(f"kernel config missing: {wanted}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/display-config")
    args = parser.parse_args()
    load_local_env()
    media = json.loads(Path(required("IA64_MEDIA_MANIFEST")).read_text())
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, (path, member) in SOURCES.items():
        iso = Path(required("IA64_IMAGES_DIR")) / media[name]["iso"]
        package = args.out / (
            name + (".deb" if name == "debian" else ".rpm" if name == "sles" else ".config")
        )
        commands = [
            [
                "xorriso",
                "-osirrox",
                "on",
                "-overwrite",
                "on",
                "-indev",
                str(iso),
                "-extract",
                path,
                str(package),
            ]
        ]
        subprocess.run(commands[-1], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if name == "debian":
            unpacked = args.out / "debian-package"
            commands.append(["dpkg-deb", "-x", str(package), str(unpacked)])
            subprocess.run(commands[-1], check=True)
            config = (unpacked / member).read_bytes()
        elif name == "sles":
            commands.append(["7z", "x", "-so", str(package)])
            config = cpio_file(
                subprocess.run(commands[-1], check=True, capture_output=True).stdout, member
            )
        else:
            config = package.read_bytes()
            package.chmod(package.stat().st_mode | 0o200)
        (args.out / f"{name}.config").write_bytes(config)
        lines = config.decode().splitlines()
        settings = {
            key: next(
                (line.split("=", 1)[1] for line in lines if line.startswith(key + "=")),
                "n" if f"# {key} is not set" in lines else "absent",
            )
            for key in OPTIONS
        }
        rows.append(
            {
                "os": name,
                "iso": iso.name,
                "member": path,
                "package_member": member,
                "sha256": hashlib.sha256(config).hexdigest(),
                "options": settings,
                "commands": commands,
            }
        )
        print(name, json.dumps(settings), flush=True)
    (args.out / "display-configs.json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
