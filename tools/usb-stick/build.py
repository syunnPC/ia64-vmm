#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a reviewable UEFI FAT32 tree, optionally a portable MBR USB disk image."""

import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu.config import ROOT, load_local_env, required
from qemu.bootmedia import stage_bootmedia
from persistence import NVRAM_PAIRS, prepare_tree


# boot_info.cmdline holds 1023 bytes (include/ia64vmm/bootinfo.h; core/options.h):
# the options file's tokens (vmm-options.conf, edited by vmmcfg.efi) take at most
# 511, an entry's own cmdline at most 511, joined by one space.
ENTRY_CMDLINE_MAX = 511
OPTIONS_TOKENS_MAX = 511
OPTIONS_PATH = "vmm-options.conf"
CONFIGURE_PATH = "EFI/vmm/vmmcfg.efi"
DEFAULT_OPTIONS = """# VMM options: key=value tokens; # starts a comment.
# Saved values override the boot entry. Safe mode and diagnostics retain
# only vmm_media and guest_media; Discovery ignores saved settings.
# Edit with Configure VMM Options, then save and reset.
"""


def options_tokens(text):
    """The tokens of an options file, as the VMM reads them (core/options.c)."""
    tokens = []
    for line in text.splitlines():
        if line.strip().startswith("#"):
            continue
        tokens += line.split()
    return tokens


def check_options(text, source):
    if any(ord(c) > 0x7E or (ord(c) < 0x20 and c not in "\t\r\n") for c in text):
        raise RuntimeError(f"{source}: only printable ASCII is allowed")
    joined = " ".join(options_tokens(text))
    if len(joined.encode()) > OPTIONS_TOKENS_MAX:
        raise RuntimeError(f"{source}: tokens exceed {OPTIONS_TOKENS_MAX} bytes")


GLOBAL_KEYS = {"VMM_OPTIONS_FILE", "NVRAM_PAIRS"}
DIAGNOSTIC_CMDLINE = (
    "exit=halt vmm_media=auto engine=interp reserve_mib=128 placement=logical-thread pause_sec=5"
)


def parse_config(source):
    """KEY=value lines of USB_STICK_CONFIG; '#' starts a comment line."""
    settings = {}
    for number, line in enumerate(Path(source).read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        if not sep or not re.fullmatch(r"[A-Z0-9_]+", key):
            raise RuntimeError(f"{source}:{number}: expected KEY=value")
        if key in settings:
            raise RuntimeError(f"{source}:{number}: duplicate {key}")
        settings[key] = value
    return settings


def entries(settings, nvram=True):
    """The fixed Limine menu and its persistence modules."""
    for key in settings:
        if key not in GLOBAL_KEYS:
            raise RuntimeError(
                f"unknown setting {key}; supported settings: VMM_OPTIONS_FILE, NVRAM_PAIRS"
            )
    # Limine v11.x CONFIG.md: one slash per menu level; directories have no
    # protocol. default_entry is 1-based and selects the normal guest boot.
    config = [
        "timeout: 10",
        "default_entry: 2",
        "interface_resolution: 1024x768",
        "",
        "/Configure VMM Options",
        "    protocol: efi",
        f"    path: boot():/{CONFIGURE_PATH}",
        "",
    ]

    def add_entry(
        title,
        line,
        depth=2,
        options="options-media",
        hello=False,
        variables=False,
        logging=False,
        resolution=False,
    ):
        if len(line.encode()) > ENTRY_CMDLINE_MAX:
            raise RuntimeError(f"{title}: command line exceeds {ENTRY_CMDLINE_MAX} bytes")
        config.extend(
            [
                f"{'/' * depth}{title}",
                "    protocol: limine",
                "    path: boot():/vmm.elf",
                f"    cmdline: {line}",
                "    module_path: boot():/firmware-new.elf",
                "    module_string: firmware",
            ]
        )
        if options:
            config.extend(
                [f"    module_path: boot():/{OPTIONS_PATH}", f"    module_string: {options}"]
            )
        if resolution:
            config.append("    resolution: 1024x768")
        if logging:
            for filename, module in [
                ("persistence-manifest.bin", "persistence"),
                ("log-a.bin", "log-a"),
                ("log-b.bin", "log-b"),
            ]:
                config.extend(
                    [f"    module_path: boot():/{filename}", f"    module_string: {module}"]
                )
        elif options:
            config.extend(
                ["    module_path: boot():/persistence-manifest.bin", "    module_string: media-id"]
            )
        if variables:
            config.extend(["    module_path: boot():/nvram-new.bin", "    module_string: nvram"])
        if hello:
            config.extend(["    module_path: boot():/hello.efi", "    module_string: bootdisk"])
        config.append("")

    boot = "exit=halt bootmenu=1 vmm_media=auto"
    add_entry(
        "Boot from guest media (using user config)",
        boot,
        depth=1,
        options="options",
        variables=nvram,
        logging=nvram,
        resolution=True,
    )
    add_entry(
        "Boot from guest media (safe mode - default settings)",
        boot,
        depth=1,
        variables=nvram,
        resolution=True,
    )
    config.extend(["/Diagnostics", ""])
    add_entry(
        "Discovery", DIAGNOSTIC_CMDLINE.replace("vmm_media=auto", "vmm_media=list"), options=None
    )
    # hostwin raises the reserve to 520 MiB for two vCPUs and ends the run.
    add_entry(
        "Basic Tests",
        DIAGNOSTIC_CMDLINE + " selftest=host selftest=timer selftest=simd selftest=hostwin vcpus=2",
    )
    for title, extra, hello in [
        ("Memory", " selftest=host", True),
        ("Timers", " selftest=timer", True),
        ("SIMD and Native x86 State", " selftest=simd", True),
        ("Host Memory Mapping", " selftest=hostwin vcpus=2", False),
        ("Guest Firmware", "", True),
    ]:
        add_entry(title, DIAGNOSTIC_CMDLINE + extra, hello=hello)
    if nvram:
        add_entry(
            "Persistent Storage", DIAGNOSTIC_CMDLINE + " media_log=1", hello=True, logging=True
        )
    config.extend(["//Fault Handling (expected halt)", ""])
    for title, extra in [
        ("Page Fault", " selftest=pf"),
        ("Invalid Opcode", " selftest=ud"),
        ("General Protection", " selftest=gp"),
        ("AP Exception", " selftest=ap-ud"),
        ("Host-window Fatal", " selftest=hostwin-fatal vcpus=2"),
        ("Host-window Outside Fatal", " selftest=hostwin-fatal vcpus=2 hwb_fatal_outside=1"),
    ]:
        add_entry(title, DIAGNOSTIC_CMDLINE + extra, depth=3)
    return "\n".join(config)


def options_file(settings):
    """Bytes of vmm-options.conf: VMM_OPTIONS_FILE (a path) seeds it, else
    comments only. The file is user data from then on: vmmcfg.efi rewrites it."""
    seed = settings.get("VMM_OPTIONS_FILE", "")
    if not seed:
        return DEFAULT_OPTIONS.encode()
    path = Path(seed)
    if not path.is_file():
        raise RuntimeError(f"VMM_OPTIONS_FILE is not a file: {seed}")
    data = path.read_bytes()
    check_options(data.decode("ascii", errors="replace"), seed)
    return data


def copy_notices(tree):
    # Keep the paths cited by NOTICE on both the ESP tree and the disk image.
    for name in [
        "NOTICE",
        "third_party/limine/LICENSE",
        "third_party/limine/LICENSE.protocol",
        "docs/limine-dependencies.md",
        "third_party/limine/NOTICE.dependencies",
        "third_party/gnu-efi/LICENSE.sources",
        "third_party/gnu-efi/LICENSE.efilib",
        "third_party/gnu-efi/LICENSE.BSD-3-Clause",
        "third_party/edk2-ipf/License.txt",
    ]:
        target = tree / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, target)
    for name in ["LICENSES", "third_party/gnu-efi/licenses"]:
        shutil.copytree(ROOT / name, tree / name, dirs_exist_ok=True)


def disk_image(tree, dest, pairs=NVRAM_PAIRS):
    prepare_tree(tree, pairs)
    used = sum(p.stat().st_size for p in tree.rglob("*") if p.is_file())
    part_sectors = max(64 * 2048, ((used * 2 + 1048575) // 1048576) * 2048)
    first, total = 2048, 2048 + part_sectors
    if total > 0xFFFFFFFF:
        raise RuntimeError("image exceeds MBR sector range")
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".tmp")
    try:
        with tmp.open("wb") as image:
            image.truncate(total * 512)
            mbr = bytearray(512)
            mbr[446:462] = struct.pack(
                "<B3sB3sII", 0, b"\x20\x21\0", 0xEF, b"\xfe\xff\xff", first, part_sectors
            )
            mbr[510:512] = b"\x55\xaa"
            image.write(mbr)
        volume = f"{tmp}@@{first * 512}"
        subprocess.run(
            ["mformat", "-i", volume, "-T", str(part_sectors), "-H", str(first), "-F", "::"],
            check=True,
        )
        subprocess.run(
            ["mcopy", "-i", volume, "-s", *map(str, sorted(tree.iterdir())), "::/"], check=True
        )
        tmp.replace(dest)
    finally:
        tmp.unlink(missing_ok=True)


def main():
    load_local_env()
    settings = parse_config(required("USB_STICK_CONFIG"))
    # NVRAM_PAIRS: EFI variable slot pairs per firmware format, one per
    # machine the stick serves.
    pairs = settings.get("NVRAM_PAIRS", str(NVRAM_PAIRS))
    if not re.fullmatch(r"[1-9][0-9]?", pairs) or int(pairs) > 32:
        raise RuntimeError("NVRAM_PAIRS must be 1..32")
    pairs = int(pairs)
    config = entries(settings)
    tree = Path(required("USB_STICK_DIR"))
    # Never remove an arbitrary destination or touch a physical disk.
    tree.mkdir(parents=True, exist_ok=True)
    (tree / "firmware.elf").unlink(missing_ok=True)
    stage_bootmedia(ROOT, tree)
    for src, name in [
        ("build/vmm.elf", "vmm.elf"),
        ("build/firmware-new/release/ia64-firmware-new.elf", "firmware-new.elf"),
        ("build/efi/hello.efi", "hello.efi"),
        ("build/vmmcfg/vmmcfg.efi", CONFIGURE_PATH),
    ]:
        shutil.copyfile(ROOT / src, tree / name)
    (tree / OPTIONS_PATH).write_bytes(options_file(settings))
    (tree / "limine.conf").write_text(config)
    copy_notices(tree)
    if os.environ.get("USB_STICK_IMAGE"):
        dest = Path(os.environ["USB_STICK_IMAGE"])
        disk_image(tree, dest, pairs=pairs)
        print(f"MBR/FAT32 image: {dest}")
    else:
        prepare_tree(tree, pairs)
    print(f"USB ESP tree: {tree}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(f"usb-stick: {error}")
