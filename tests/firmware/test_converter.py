# /* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
# Copyright 2026 syunnPC
# Origin: Asteria Tests/Platform/Ia64/Firmware/Ia64ImageRelocationTests.cpp
"""Hand-made static ELFs; no compiler/objcopy is the oracle for conversion."""

import importlib.util
from pathlib import Path
import struct
import sys
import unittest

MODULE = Path(__file__).resolve().parents[2] / "tools/firmware/elf_to_pe.py"
spec = importlib.util.spec_from_file_location("elf_to_pe", MODULE)
converter = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = converter
spec.loader.exec_module(converter)

BASE = 0x4000000


def immediate_positions():
    # Independent bit-coordinate model for movl immediates.
    return [
        100 + b
        if b < 7
        else 114 + b - 7
        if b < 16
        else 109 + b - 16
        if b < 21
        else 108
        if b == 21
        else 46 + b - 22
        if b < 63
        else 123
        for b in range(64)
    ]


def movl(value):
    # SDM Vol.3: MLX, long immediate slot 1 and movl opcode 6 in slot 2.
    x = (
        6 << 37
        | (value >> 63) << 36
        | ((value >> 7) & 511) << 27
        | ((value >> 16) & 31) << 22
        | ((value >> 21) & 1) << 21
        | (value & 127) << 13
    )
    return (4 | ((value >> 22) & ((1 << 41) - 1)) << 46 | x << 87).to_bytes(16, "little")


def fixture(relocations=None, got=None, descriptor=None):
    if relocations is None:
        relocations = [
            (BASE + 0x1001, 3, 0x23, 0),
            (BASE + 0x4000, 3, 0x27, 0),
            (BASE + 0x4008, 4, 0x47, 0),
        ]
    names = [
        "",
        ".text",
        ".opd",
        ".got",
        ".data",
        ".bss",
        ".strtab",
        ".symtab",
        ".rela.text",
        ".rela.data",
        ".shstrtab",
    ]
    string_names = b"\0__image_base\0__entry_descriptor\0value\0function\0absolute\0__gp\0"
    symbols = [
        (0, 0, 0, 0, 0, 0),
        (string_names.index(b"__image_base"), 0x10, 0, 0xFFF1, BASE, 0),
        (string_names.index(b"__entry_descriptor"), 0x10, 0, 2, BASE + 0x2000, 16),
        (string_names.index(b"value"), 0x10, 0, 4, BASE + 0x4010, 8),
        (string_names.index(b"function"), 0x12, 0, 1, BASE + 0x1000, 16),
        (string_names.index(b"absolute"), 0x10, 0, 0xFFF1, 0x1234, 0),
        (string_names.index(b"__gp"), 0x10, 0, 0xFFF1, BASE + 0x3000, 0),
    ]
    contents = [
        b"",
        movl(BASE + 0x4010),
        descriptor or struct.pack("<QQ", BASE + 0x1000, BASE + 0x3000),
        got or struct.pack("<QQ", BASE + 0x4010, BASE + 0x2000),
        struct.pack("<QQQ", BASE + 0x4010, BASE + 0x2000, 42),
        b"",
        string_names,
        b"".join(struct.pack("<IBBHQQ", *sym) for sym in symbols),
    ]
    for target in (1, 4):
        contents.append(
            b"".join(
                struct.pack("<QQq", address, symbol << 32 | kind, addend)
                for address, symbol, kind, addend in relocations
                if (address - BASE) >> 12 == target
            )
        )
    section_names = b"\0" + b"\0".join(n.encode() for n in names[1:]) + b"\0"
    contents.append(section_names)
    result = bytearray(0x5020)
    sections = [(0,) * 10]
    for i in range(1, len(names)):
        kind = 8 if i == 5 else 3 if i in (6, 10) else 2 if i == 7 else 4 if i in (8, 9) else 1
        address = BASE + i * 4096 if i <= 5 else 0
        offset = i * 4096 if i <= 4 else len(result)
        if contents[i]:
            if i <= 4:
                result[offset : offset + len(contents[i])] = contents[i]
            else:
                result += contents[i]
        size = 32 if i == 5 else len(contents[i])
        flags = 6 if i == 1 else 3 if i in (4, 5) else 2 if i in (2, 3) else 0
        link = 6 if i == 7 else 7 if i in (8, 9) else 0
        info = 1 if i in (7, 8) else 4 if i == 9 else 0
        sections.append(
            (
                section_names.index(names[i].encode()),
                kind,
                flags,
                address,
                offset,
                size,
                link,
                info,
                16 if i == 2 else 8,
                24 if i in (7, 8, 9) else 0,
            )
        )
    shoff = len(result)
    result += b"".join(struct.pack("<IIQQQQIIQQ", *s) for s in sections)
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into(
        "<16sHHIQQQIHHHHHH",
        result,
        0,
        ident,
        2,
        50,
        1,
        BASE + 0x2000,
        64,
        shoff,
        0x10,
        64,
        56,
        1,
        64,
        len(sections),
        10,
    )
    struct.pack_into(
        "<IIQQQQQQ", result, 64, 1, 7, 4096, BASE + 4096, BASE + 4096, 0x4000, 0x4020, 4096
    )
    return result


def load_pe(pe, delta=0):
    opt = 88
    count = struct.unpack_from("<H", pe, 70)[0]
    memory = bytearray(struct.unpack_from("<I", pe, opt + 56)[0])
    for i in range(count):
        size, rva, raw_size, raw = struct.unpack_from("<IIII", pe, opt + 240 + i * 40 + 8)
        memory[rva : rva + min(size, raw_size)] = pe[raw : raw + min(size, raw_size)]
    rva, size = struct.unpack_from("<II", pe, opt + 112 + 40)
    fixups = []
    end = rva + size
    while rva < end:
        page, block = struct.unpack_from("<II", memory, rva)
        for offset in range(rva + 8, rva + block, 2):
            entry = struct.unpack_from("<H", memory, offset)[0]
            kind, address = entry >> 12, page + (entry & 4095)
            if not kind:
                continue
            fixups.append((address, kind))
            if kind == 10:
                value = struct.unpack_from("<Q", memory, address)[0]
                struct.pack_into("<Q", memory, address, (value + delta) % 2**64)
            elif kind == 9:
                address &= -16
                word = int.from_bytes(memory[address : address + 16], "little")
                positions = immediate_positions()
                value = sum((word >> position & 1) << bit for bit, position in enumerate(positions))
                value = (value + delta) % 2**64
                for bit, position in enumerate(positions):
                    word = (word & ~(1 << position)) | (value >> bit & 1) << position
                memory[address : address + 16] = word.to_bytes(16, "little")
            else:
                raise AssertionError("unsupported PE fixup")
        rva += block
    return memory, fixups


class ConverterTests(unittest.TestCase):
    def test_headers_and_relocations(self):
        for subsystem in (10, 11, 12):
            pe = converter.convert(fixture(), subsystem)
            self.assertEqual(pe[:2], b"MZ")
            self.assertEqual(struct.unpack_from("<H", pe, 68)[0], 0x200)
            self.assertEqual(struct.unpack_from("<H", pe, 156)[0], subsystem)
            self.assertEqual(struct.unpack_from("<I", pe, 104)[0], 0x2000)
            for delta in (0, 0x1800000, -0x3000000):
                memory, fixups = load_pe(pe, delta)
                self.assertEqual(
                    fixups,
                    [
                        (0x1001, 9),
                        (0x2000, 10),
                        (0x2008, 10),
                        (0x3000, 10),
                        (0x3008, 10),
                        (0x4000, 10),
                        (0x4008, 10),
                    ],
                )
                self.assertEqual(memory[0x1000:0x1010], movl(BASE + 0x4010 + delta))
                self.assertEqual(
                    struct.unpack_from("<QQ", memory, 0x2000),
                    (BASE + 0x1000 + delta, BASE + 0x3000 + delta),
                )
                self.assertEqual(
                    struct.unpack_from("<QQQ", memory, 0x4000),
                    (BASE + 0x4010 + delta, BASE + 0x2000 + delta, 42),
                )
                self.assertEqual(memory[0x5000:0x5020], bytes(32))

    def test_deterministic(self):
        self.assertEqual(converter.convert(fixture()), converter.convert(fixture()))

    def test_null_got(self):
        pe = converter.convert(fixture(got=bytes(16)))
        memory, fixups = load_pe(pe, 0x100000)
        self.assertEqual(memory[0x3000:0x3010], bytes(16))
        self.assertFalse(any(0x3000 <= site < 0x3010 for site, _ in fixups))

    def test_unknown_relocation(self):
        with self.assertRaisesRegex(converter.ConversionError, "unsupported allocated"):
            converter.convert(fixture([(BASE + 0x1001, 3, 0x91, 0)]))

    def test_instruction_overlap(self):
        with self.assertRaisesRegex(converter.ConversionError, "overlapping instruction"):
            converter.convert(fixture([(BASE + 0x1001, 3, 0x23, 0), (BASE + 0x1002, 3, 0x2A, 0)]))

    def test_duplicate_fixup_deduplicated(self):
        one = (BASE + 0x4000, 3, 0x27, 0)
        self.assertEqual(converter.convert(fixture([one])), converter.convert(fixture([one, one])))

    def test_external_absolute_stays_constant(self):
        elf = fixture([(BASE + 0x4000, 5, 0x27, 0)])
        struct.pack_into("<Q", elf, 0x4000, 0x1234)
        memory, _ = load_pe(converter.convert(elf), 0x100000)
        self.assertEqual(struct.unpack_from("<Q", memory, 0x4000)[0], 0x1234)

    def test_invariant_external_absolute_rejected(self):
        with self.assertRaisesRegex(converter.ConversionError, "external absolute"):
            converter.convert(fixture([(BASE + 0x1000, 5, 0x2A, 0)]))

    def test_invariant_relocations(self):
        for kind in sorted(converter.INSTRUCTION_INVARIANTS | converter.CELL_INVARIANTS):
            site = BASE + (
                0x4000
                if kind in converter.CELL_INVARIANTS
                else 0x1001
                if kind in converter.LONG_INSTRUCTIONS
                else 0x1000
            )
            converter.convert(fixture([(site, 3, kind, 0)]))

    def test_bad_descriptor(self):
        with self.assertRaisesRegex(converter.ConversionError, "descriptor"):
            converter.convert(fixture(descriptor=struct.pack("<QQ", BASE + 0x4000, BASE + 0x3000)))

    def test_unknown_got_constant(self):
        with self.assertRaisesRegex(converter.ConversionError, "OPD/GOT cell"):
            converter.convert(fixture(got=struct.pack("<QQ", 7, 42)))

    def test_corrupt_or_dynamic_elf(self):
        for offset, fmt, value in [
            (16, "<H", 3),
            (18, "<H", 62),
            (5, "<B", 2),
            (40, "<Q", 2**63),
            (60, "<H", 0),
            (64, "<I", 2),
        ]:
            with self.subTest(offset=offset):
                elf = fixture()
                struct.pack_into(fmt, elf, offset, value)
                with self.assertRaises(converter.ConversionError):
                    converter.convert(elf)

    def test_truncation(self):
        for size in (0, 16, 63, 120, len(fixture()) - 1):
            with self.assertRaises(converter.ConversionError):
                converter.convert(fixture()[:size])

    def test_relocation_outside_section(self):
        with self.assertRaisesRegex(converter.ConversionError, "invalid cell"):
            converter.convert(fixture([(BASE + 0x4020, 3, 0x27, 0)]))

    def test_bad_movl(self):
        elf = fixture()
        elf[0x1000] = 0
        with self.assertRaisesRegex(converter.ConversionError, "MLX"):
            converter.convert(elf)

    def test_source_conflict(self):
        with self.assertRaisesRegex(converter.ConversionError, "contradictory source"):
            converter.convert(fixture([(BASE + 0x4000, 3, 0x27, 0), (BASE + 0x4000, 3, 0x4F, 0)]))

    def test_cell_overlap(self):
        with self.assertRaisesRegex(converter.ConversionError, "overlapping cell"):
            converter.convert(fixture([(BASE + 0x4000, 3, 0x4F, 0), (BASE + 0x4004, 3, 0x4F, 0)]))

    def test_bad_gp(self):
        with self.assertRaisesRegex(converter.ConversionError, "inconsistent GP"):
            converter.convert(fixture(descriptor=struct.pack("<QQ", BASE + 0x1000, BASE + 0x4000)))

    def test_scattered_immediate_reference(self):
        for value in (0, 1, 1 << 63, 2**64 - 1, 0xABCDEF0123456789):
            word = 4 | 6 << 124
            for bit, position in enumerate(immediate_positions()):
                word |= (value >> bit & 1) << position
            self.assertEqual(converter.imm64(word.to_bytes(16, "little")), value)

    def test_imm64_preserves_other_bundle_bits(self):
        elf = fixture()
        untouched = 8 << 93 | 3 << 87  # destination register and predicate.
        word = int.from_bytes(elf[0x1000:0x1010], "little") | untouched
        elf[0x1000:0x1010] = word.to_bytes(16, "little")
        memory, _ = load_pe(converter.convert(elf), 0x100000)
        result = int.from_bytes(memory[0x1000:0x1010], "little")
        immediate_mask = sum(1 << bit for bit in immediate_positions())
        self.assertEqual(result & ~immediate_mask, word & ~immediate_mask)
        self.assertEqual(converter.imm64(memory[0x1000:0x1010]), BASE + 0x4010 + 0x100000)


if __name__ == "__main__":
    unittest.main()
