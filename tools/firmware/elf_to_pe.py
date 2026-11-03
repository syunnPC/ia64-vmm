#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
# Copyright 2026 syunnPC
# Origin: Asteria Tools/Firmware/NormalizeIa64Pe.cpp
"""Static IA-64 ELF -> relocatable UEFI PE32+ (no objcopy/GenFw dependency).

Algorithmic port: bounded ELF section/RELA parsing, instruction slot validation, descriptor
validation, deduplication and page-grouped PE fixups. Extensions:
write PE headers, retain separate GOT, synthesize all OPD/GOT fixups, support
the invariant relocations emitted by EDK2 and EFI subsystems 10/11/12.
"""

import argparse
from dataclasses import dataclass
from pathlib import Path
import struct
import sys


class ConversionError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ConversionError(message)


def align(value, boundary):
    return (value + boundary - 1) & -boundary


def bounded(data, offset, size):
    require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset, "range outside file")
    return data[offset : offset + size]


def unpack(fmt, data, offset=0):
    return struct.unpack(fmt, bounded(data, offset, struct.calcsize(fmt)))


def string(data, offset):
    require(0 <= offset < len(data), "invalid string offset")
    end = data.find(b"\0", offset)
    require(end >= offset, "unterminated string")
    return data[offset:end].decode("ascii", errors="strict")


@dataclass
class Section:
    name_offset: int
    kind: int
    flags: int
    address: int
    offset: int
    size: int
    link: int
    info: int
    alignment: int
    entry_size: int
    name: str = ""
    data: bytes = b""

    def contains(self, address, size=1):
        return self.address <= address and size <= self.size - (address - self.address)


class Elf:
    def __init__(self, data):
        self.data = data
        h = unpack("<16sHHIQQQIHHHHHH", data)
        ident, kind, machine, version, self.entry, phoff, shoff, flags, hs, ps, pn, ss, sn, ni = h
        require(
            ident[:7] == b"\x7fELF\x02\x01\x01" and ident[7] in (0, 3),
            "expected ELF64 little-endian System V/Linux",
        )
        require(
            kind == 2 and machine == 50 and version == 1 and flags & 0x10,
            "expected static IA-64 ABI64 ET_EXEC",
        )
        require(
            hs == 64 and ss == 64 and shoff and sn and 0 < ni < sn,
            "missing/extended ELF section metadata",
        )
        bounded(data, shoff, sn * ss)
        self.sections = [Section(*unpack("<IIQQQQIIQQ", data, shoff + i * ss)) for i in range(sn)]
        require(self.sections[0].kind == 0, "invalid null section")
        names = self.sections[ni]
        require(names.kind == 3, "invalid section string table")
        names = bounded(data, names.offset, names.size)
        require(names[:1] == b"\0" and names[-1:] == b"\0", "invalid section names")
        for s in self.sections[1:]:
            s.name = string(names, s.name_offset)
            require(
                not s.alignment or s.alignment & (s.alignment - 1) == 0,
                "invalid ELF section alignment",
            )
            require(not s.flags & 0x400, "TLS unsupported")
            require(
                s.kind not in (5, 6, 11, 19, 0x6FFFFFF6, 0x6FFFFFFF),
                "dynamic ELF section unsupported",
            )
            require(
                not s.name.startswith((".plt", ".iplt", ".dynamic", ".dyn")),
                "dynamic/PLT artifact unsupported",
            )
            if s.kind != 8:
                s.data = bounded(data, s.offset, s.size)
            if s.flags & 2:
                require(s.kind in (1, 8, 0x70000001), "unsupported allocated section")
                require(
                    s.address < 2**64 and s.size <= 2**64 - s.address, "allocated range overflow"
                )
                require(s.alignment <= 1 or s.address % s.alignment == 0, "misaligned ELF section")
        self.loaded = sorted(
            (s for s in self.sections if s.flags & 2 and s.size), key=lambda s: s.address
        )
        require(self.loaded, "no loaded sections")
        for a, b in zip(self.loaded, self.loaded[1:]):
            require(a.address + a.size <= b.address, "overlapping allocated sections")
        require(pn and ps == 56 and pn != 0xFFFF, "missing/extended program metadata")
        bounded(data, phoff, pn * ps)
        self.segments = []
        for i in range(pn):
            pt, pf, off, va, pa, fs, ms, al = unpack("<IIQQQQQQ", data, phoff + i * ps)
            require(pt not in (2, 3, 7), "dynamic/interpreter/TLS segment unsupported")
            require(fs <= ms and ms <= 2**64 - va, "invalid segment size")
            bounded(data, off, fs)
            require(al <= 1 or al & (al - 1) == 0, "invalid segment alignment")
            if pt == 1:
                require(al <= 1 or va % al == off % al, "misaligned load segment")
                self.segments.append((va, ms, off, fs))
        for s in self.loaded:
            require(
                any(
                    va <= s.address
                    and s.address + s.size <= va + ms
                    and (
                        s.kind == 8
                        or (off + s.address - va == s.offset and s.address + s.size <= va + fs)
                    )
                    for va, ms, off, fs in self.segments
                ),
                "allocated section not faithfully mapped by PT_LOAD",
            )

    def section_at(self, address, size=1):
        return next((s for s in self.loaded if s.contains(address, size)), None)

    def cell(self, section, address):
        require(section.kind != 8 and section.contains(address, 8), "invalid raw cell")
        return unpack("<Q", section.data, address - section.address)[0]

    def symbols(self, table):
        require(
            table.kind == 2 and table.entry_size == 24 and table.size % 24 == 0,
            "invalid static symbol table",
        )
        require(0 < table.link < len(self.sections), "invalid symbol names link")
        names = self.sections[table.link]
        require(names.kind == 3, "invalid symbol names")
        return [
            (string(names.data, name), info, idx, value, size)
            for name, info, other, idx, value, size in struct.iter_unpack("<IBBHQQ", table.data)
        ]

    def named_symbol(self, name):
        matches = [
            sym
            for t in self.sections
            if t.kind == 2
            for sym in self.symbols(t)
            if sym[0] == name and sym[2]
        ]
        require(len(matches) == 1, "missing/ambiguous symbol " + name)
        return matches[0][3]


# IA-64 ELF ABI relocation codes (also /usr/include/elf.h). These have already
# been applied by the static linker. Common movement preserves their differences.
INSTRUCTION_INVARIANTS = {0x2A, 0x2B, 0x32, 0x33, 0x49, 0x4A, 0x4B, 0x52, 0x53, 0x86, 0x87}
CELL_INVARIANTS = {0x2F, 0x4F, 0x5F, 0x67}
LONG_INSTRUCTIONS = {0x23, 0x2B, 0x33, 0x53}


def imm64(bundle):
    # IA-64 movl decoder, SDM Vol.3 movl encoding.
    word = int.from_bytes(bundle, "little")
    require(word & 0x1E == 4, "IMM64 site is not an MLX bundle")
    l = (word >> 46) & ((1 << 41) - 1)
    x = word >> 87
    require(x >> 37 == 6, "IMM64 site is not movl")
    return (
        ((x >> 36) & 1) << 63
        | (l & ((1 << 41) - 1)) << 22
        | ((x >> 27) & 0x1FF) << 7
        | ((x >> 22) & 0x1F) << 16
        | ((x >> 21) & 1) << 21
        | (x >> 13) & 0x7F
    )


def collect_relocations(elf, base):
    fixups = {}
    footprints = {}
    source_sites = {}
    cell_sites = {}

    def add(address, kind, width):
        require(
            elf.section_at(address & -16 if kind == 9 else address, width), "fixup outside image"
        )
        rva = address - base
        require(0 <= rva < 2**32, "fixup RVA overflow")
        require(rva not in fixups or fixups[rva] == kind, "conflicting PE fixups")
        fixups[rva] = kind

    def relative(sym):
        name, info, idx, value, size = sym
        require(idx not in (0, 0xFFF2, 0xFFFF), "undefined/common symbol " + name)
        require(idx == 0xFFF1 or idx < len(elf.sections), "invalid symbol section")
        if idx == 0xFFF1:
            # Linker-defined __gp and boundaries are ABS but move with the image.
            return name in {"__image_base", "__gp", "__entry_descriptor", "__image_end"}
        require(elf.sections[idx].flags & 2, "relocation against nonallocated symbol")
        return True

    opd = next((s for s in elf.loaded if s.name == ".opd"), None)
    for r in elf.sections:
        if r.kind not in (4, 9):
            continue
        require(r.info < len(elf.sections), "invalid relocation target")
        target = elf.sections[r.info]
        if not target.flags & 2:
            continue
        require(
            r.kind == 4 and r.entry_size == 24 and r.size % 24 == 0, "expected allocated RELA table"
        )
        require(r.link < len(elf.sections), "invalid relocation symbol link")
        symbols = elf.symbols(elf.sections[r.link])
        for address, info, addend in struct.iter_unpack("<QQq", r.data):
            kind, si = info & 0xFFFFFFFF, info >> 32
            if kind == 0:
                continue
            require(si < len(symbols), "invalid relocation symbol index")
            moves = relative(symbols[si])
            require(
                address not in source_sites or source_sites[address] == kind,
                "contradictory source relocations",
            )
            source_sites[address] = kind
            if kind in INSTRUCTION_INVARIANTS | {0x23}:
                slot = address & 15
                bundle = address & -16
                require(
                    slot <= 2
                    and target.flags & 4
                    and target.kind != 8
                    and target.contains(bundle, 16),
                    "invalid instruction relocation site",
                )
                require(kind not in LONG_INSTRUCTIONS or slot == 1, "invalid long instruction slot")
                mask = 6 if kind in LONG_INSTRUCTIONS else 1 << slot
                for prev_kind, prev_addr, prev_mask in footprints.get(bundle, []):
                    require(
                        not prev_mask & mask or (prev_kind, prev_addr) == (kind, address),
                        "overlapping instruction relocation footprints",
                    )
                footprints.setdefault(bundle, []).append((kind, address, mask))
                if kind == 0x23:
                    value = imm64(
                        target.data[bundle - target.address : bundle - target.address + 16]
                    )
                    require(
                        value == (symbols[si][3] + addend) % 2**64,
                        "IMM64 does not match static symbol value",
                    )
                    if moves:
                        add(address, 9, 16)
                else:
                    require(moves, "invariant instruction references external absolute symbol")
            elif kind in CELL_INVARIANTS | {0x27, 0x47, 0x81}:
                width = 16 if kind == 0x81 else 8
                require(
                    target.kind != 8 and target.contains(address, width),
                    "invalid cell relocation site",
                )
                for old_address, old_width in cell_sites.items():
                    require(
                        old_address == address
                        or address + width <= old_address
                        or old_address + old_width <= address,
                        "overlapping cell relocations",
                    )
                cell_sites[address] = width
                value = elf.cell(target, address)
                if kind == 0x27:
                    require(
                        value == (symbols[si][3] + addend) % 2**64,
                        "DIR64 does not match static symbol value",
                    )
                    if moves:
                        add(address, 10, 8)
                elif kind == 0x47:
                    require(
                        moves and opd and value % 16 == 0 and opd.contains(value, 16),
                        "invalid function descriptor reference",
                    )
                    require(
                        elf.cell(opd, value) == (symbols[si][3] + addend) % 2**64,
                        "function descriptor does not match static symbol",
                    )
                    add(address, 10, 8)
                elif kind == 0x81:
                    require(moves and address % 8 == 0, "invalid inline descriptor")
                    require(
                        value == (symbols[si][3] + addend) % 2**64
                        and elf.cell(target, address + 8) == elf.named_symbol("__gp"),
                        "inline descriptor does not match static symbol/GP",
                    )
                    add(address, 10, 8)
                    add(address + 8, 10, 8)
                else:
                    require(moves, "invariant cell references external absolute symbol")
            else:
                raise ConversionError(f"unsupported allocated IA-64 relocation 0x{kind:02x}")
    # The IA-64 linker creates these cells without emitting relocations for them.
    for s in elf.loaded:
        if s.name not in (".opd", ".got"):
            continue
        width = 16 if s.name == ".opd" else 8
        require(
            s.kind == 1 and s.address % width == 0 and s.size % width == 0, "invalid OPD/GOT layout"
        )
        for offset in range(0, s.size, width):
            for cell_offset in range(0, width, 8):
                address = s.address + offset + cell_offset
                value = elf.cell(s, address)
                # Refuse unknown absolute GOT values: silently rebasing constants
                # would corrupt them. Null GOT entries stay null.
                if value == 0 and s.name == ".got":
                    continue
                require(
                    base <= value <= max(x.address + x.size for x in elf.loaded),
                    "OPD/GOT cell is not an image address",
                )
                if s.name == ".opd" and cell_offset == 0:
                    code = elf.section_at(value, 16)
                    require(
                        value % 16 == 0 and code and code.flags & 4,
                        "descriptor code outside executable section",
                    )
                if s.name == ".opd" and cell_offset == 8:
                    require(value == elf.named_symbol("__gp"), "descriptor has inconsistent GP")
                add(address, 10, 8)
    return sorted(fixups.items())


def encode_relocations(fixups):
    pages = {}
    for rva, kind in fixups:
        pages.setdefault(rva & -4096, []).append(kind << 12 | rva & 4095)
    result = bytearray()
    for page, entries in sorted(pages.items()):
        if len(entries) % 2:
            entries.append(0)  # IMAGE_REL_BASED_ABSOLUTE padding.
        result += struct.pack("<II", page, 8 + 2 * len(entries))
        result += struct.pack("<" + "H" * len(entries), *entries)
    return bytes(result)


def convert(data, subsystem=10, entry_symbol="__entry_descriptor", base_symbol="__image_base"):
    require(subsystem in (10, 11, 12), "unsupported EFI subsystem")
    elf = Elf(data)
    base = elf.named_symbol(base_symbol)
    require(base % 65536 == 0, "image base must be 64 KiB aligned")
    require(len(elf.loaded) < 95, "too many PE sections")
    entry = elf.named_symbol(entry_symbol)
    opd = elf.section_at(entry, 16)
    require(
        opd and opd.name == ".opd" and entry % 16 == 0, "entry must be a complete OPD descriptor"
    )
    code = elf.cell(opd, entry)
    require(elf.entry == entry or elf.entry == code, "ELF entry disagrees with PE descriptor")
    sections = []
    for s in elf.loaded:
        rva = s.address - base
        require(
            4096 <= rva < 2**32 and rva % 4096 == 0,
            "ELF layout must leave headers and align every allocated section to 4 KiB",
        )
        require(0 < s.size < 2**32 - rva, "PE section size overflow")
        require(len(s.name.encode("ascii")) <= 8, "PE section name exceeds 8 bytes")
        if sections:
            require(
                sections[-1][1] + align(sections[-1][2], 4096) <= rva,
                "PE section alignment causes overlap",
            )
        characteristics = 0x40000000 | (0x80000000 if s.flags & 1 else 0)
        characteristics |= 0x20000020 if s.flags & 4 else (0x80 if s.kind == 8 else 0x40)
        sections.append((s.name, rva, s.size, s.data, characteristics))
    fixups = collect_relocations(elf, base)
    require(fixups, "image has no relocations")
    reloc = encode_relocations(fixups)
    reloc_rva = align(sections[-1][1] + sections[-1][2], 4096)
    sections.append((".reloc", reloc_rva, len(reloc), reloc, 0x42000040))
    header_size = align(64 + 24 + 240 + 40 * len(sections), 512)
    require(header_size <= elf.loaded[0].address - base, "headers overlap image")
    image_size = align(reloc_rva + len(reloc), 4096)
    require(image_size < 2**32 and base + image_size < 2**64, "image size overflow")
    result = bytearray(header_size)
    result[:2] = b"MZ"
    struct.pack_into("<I", result, 60, 64)
    struct.pack_into("<IHHIIIHH", result, 64, 0x4550, 0x200, len(sections), 0, 0, 0, 240, 0x022E)
    opt = 88
    struct.pack_into("<H", result, opt, 0x20B)
    struct.pack_into("<II", result, opt + 16, entry - base, elf.loaded[0].address - base)
    struct.pack_into("<QII", result, opt + 24, base, 4096, 512)
    struct.pack_into("<II", result, opt + 56, image_size, header_size)
    struct.pack_into("<HH", result, opt + 68, subsystem, 0x160)
    struct.pack_into("<QQQQII", result, opt + 72, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    struct.pack_into("<II", result, opt + 112 + 5 * 8, reloc_rva, len(reloc))
    totals = [0, 0, 0]
    for i, (name, rva, size, raw, characteristics) in enumerate(sections):
        raw_size = align(len(raw), 512) if raw else 0
        offset = len(result) if raw else 0
        struct.pack_into(
            "<8sIIIIIIHHI",
            result,
            opt + 240 + i * 40,
            name.encode(),
            size,
            rva,
            raw_size,
            offset,
            0,
            0,
            0,
            0,
            characteristics,
        )
        result += raw + bytes(raw_size - len(raw))
        totals[0 if characteristics & 0x20 else 2 if characteristics & 0x80 else 1] += (
            raw_size if raw else size
        )
    struct.pack_into("<III", result, opt + 4, *totals)
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("pe", type=Path)
    parser.add_argument("--subsystem", type=int, default=10)
    parser.add_argument("--entry-symbol", default="__entry_descriptor")
    args = parser.parse_args()
    try:
        output = convert(args.elf.read_bytes(), args.subsystem, args.entry_symbol)
        temporary = args.pe.with_name(args.pe.name + ".tmp")
        temporary.write_bytes(output)
        temporary.replace(args.pe)
    except (ConversionError, OSError, UnicodeError, struct.error) as error:
        print("elf_to_pe: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
