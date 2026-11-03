# SPDX-License-Identifier: GPL-2.0-or-later
"""Install the UEFI boot-media shim and its Limine child on an ESP."""

import shutil


BOOT_FILES = [
    ("build/bootmedia/bootmedia.efi", "EFI/BOOT/BOOTX64.EFI"),
    ("third_party/limine/BOOTX64.EFI", "EFI/vmm/limine.efi"),
]


def stage_bootmedia(root, tree):
    for source, name in BOOT_FILES:
        destination = tree / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / source, destination)
