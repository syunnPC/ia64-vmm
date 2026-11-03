#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a PE32+ x86-64 EFI application (subsystem 10)."""

import struct
import sys

IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_RELOCS_STRIPPED = 0x0001
IMAGE_FILE_EXECUTABLE_IMAGE = 0x0002
PE32_PLUS = 0x20B
SUBSYSTEM_EFI_APPLICATION = 10


def check(path, require_relocations=True):
    data = open(path, "rb").read()
    assert data[:2] == b"MZ", "no MZ header"
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe : pe + 4] == b"PE\0\0", "no PE signature"
    machine, sections, _, _, _, opt_size, characteristics = struct.unpack_from(
        "<HHIIIHH", data, pe + 4
    )
    assert machine == IMAGE_FILE_MACHINE_AMD64, f"machine {machine:#x}, not x86-64"
    assert characteristics & IMAGE_FILE_EXECUTABLE_IMAGE, "not an executable image"
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    assert magic == PE32_PLUS, f"optional header magic {magic:#x}, not PE32+"
    entry = struct.unpack_from("<I", data, opt + 16)[0]
    subsystem = struct.unpack_from("<H", data, opt + 68)[0]
    assert subsystem == SUBSYSTEM_EFI_APPLICATION, (
        f"subsystem {subsystem}, not 10 (EFI application)"
    )
    assert entry, "no entry point"
    # EDK II BasePeCoffLib permits an image with no fixups when relocations
    # are not stripped. Fully position-independent applications need none.
    table = opt + 112
    reloc_rva, reloc_size = struct.unpack_from("<II", data, table + 5 * 8)
    assert not characteristics & IMAGE_FILE_RELOCS_STRIPPED, "base relocations stripped"
    assert bool(reloc_rva) == bool(reloc_size), "incomplete base relocation directory"
    if require_relocations:
        assert reloc_size, "no base relocation table"
    first = opt + opt_size
    inside = False
    for i in range(sections):
        _, vsize, rva, rawsize = struct.unpack_from("<8sIII", data, first + 40 * i)
        inside |= rva <= entry < rva + max(vsize, rawsize)
    assert inside, "entry point outside every section"
    return sections


if __name__ == "__main__":
    allow_pic = "--allow-no-relocations" in sys.argv[2:]
    try:
        count = check(sys.argv[1], require_relocations=not allow_pic)
    except AssertionError as error:
        sys.exit(f"test-vmmcfg: {sys.argv[1]}: {error}")
    print(
        f"test-vmmcfg: {sys.argv[1]} is a PE32+ x86-64 EFI application (subsystem 10, {count} sections)"
    )
