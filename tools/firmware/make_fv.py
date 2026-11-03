#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""PI firmware volume: one uncompressed DXE Core PE32 section."""

from pathlib import Path
import struct
import sys
import uuid

CORE_GUID = "d6a2cb7f-6a18-4e2f-b43b-9920a733700a"


def make_fv(pe):
    section_size = len(pe) + 4
    file_size = section_size + 24
    if file_size >= 0xFFFFFF:
        raise ValueError("FFS file too large")
    section = section_size.to_bytes(3, "little") + b"\x10" + pe
    file_header = bytearray(
        uuid.UUID(CORE_GUID).bytes_le + b"\0\0\x05\0" + file_size.to_bytes(3, "little") + b"\0"
    )
    file_header[16] = -sum(file_header) & 255
    file_header[17] = 0xAA  # No file checksum attribute, PI's fixed checksum.
    file_header[23] = 0xF8  # Erase polarity 1; header constructed/valid, data valid.
    fv_size = (72 + file_size + 4095) & -4096
    header = bytearray(
        struct.pack(
            "<16s16sQIIHHHBBIIII",
            bytes(16),
            uuid.UUID("8c8ce578-8a3d-4f1c-9935-896185c32dd3").bytes_le,
            fv_size,
            0x4856465F,
            0x000C0C06,
            72,
            0,
            0,
            0,
            2,
            fv_size // 4096,
            4096,
            0,
            0,
        )
    )
    struct.pack_into("<H", header, 50, -sum(struct.unpack("<36H", header)) & 65535)
    return bytes(header + file_header + section) + b"\xff" * (fv_size - 72 - file_size)


if __name__ == "__main__":
    Path(sys.argv[2]).write_bytes(make_fv(Path(sys.argv[1]).read_bytes()))
