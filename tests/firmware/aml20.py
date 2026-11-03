# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Strict ACPI 2.0 checker for the firmware's emitted AML subset.

ACPI 2.0 §§17.2.1-17.2.4 and §6.4.3.5. Unknown constructs fail closed;
this is not an interpreter or a validator for arbitrary vendor AML.
Lengths delimit recursive reads, never scans for opcodes inside raw buffers.
"""

from pathlib import Path
import re


class Invalid(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise Invalid(message)


class Grammar:
    def __init__(self, data, revision=2):
        self.data = data
        self.at = 0
        self.revision = revision
        self.names = set()
        self.processors = []
        self.packages = []
        self.constants = []
        self.resources = []

    def take(self, count, limit):
        require(count <= limit - self.at, "truncated operand")
        result = self.data[self.at : self.at + count]
        self.at += count
        return result

    def byte(self, limit):
        return self.take(1, limit)[0]

    def package(self, limit):
        start = self.at
        lead = self.byte(limit)
        follow = lead >> 6
        require(not follow or not lead & 0x30, "reserved PkgLength bits")
        length = lead & (0x0F if follow else 0x3F)
        for i, value in enumerate(self.take(follow, limit)):
            length |= value << (4 + 8 * i)
        require(length >= follow + 1 and length <= limit - start, "package boundary")
        self.packages.append((start, follow + 1, length))
        return start + length

    def name(self, limit, scope):
        absolute = False
        if self.data[self.at : self.at + 1] == b"\\":
            self.at += 1
            absolute = True
        while self.data[self.at : self.at + 1] == b"^":
            require(not absolute and scope, "invalid parent prefix")
            self.at += 1
            scope = scope[:-1]
        count = 1
        if self.data[self.at : self.at + 1] == b".":
            self.at += 1
            count = 2
        elif self.data[self.at : self.at + 1] == b"/":
            self.at += 1
            count = self.byte(limit)
            require(count, "empty MultiNamePath")
        segments = []
        for _ in range(count):
            segment = self.take(4, limit)
            require(re.fullmatch(rb"[A-Z_][A-Z_0-9]{3}", segment), "invalid NameSeg")
            segments.append(segment.decode("ascii"))
        return (() if absolute else scope) + tuple(segments)

    def integer(self, limit):
        start = self.at
        op = self.byte(limit)
        if op in (0, 1, 0xFF):
            value = {0: 0, 1: 1, 0xFF: (1 << (64 if self.revision == 2 else 32)) - 1}[op]
        else:
            require(op in (0x0A, 0x0B, 0x0C, 0x0E), "integer opcode")
            require(op != 0x0E or self.revision == 2, "QWordConst in revision 1")
            width = {0x0A: 1, 0x0B: 2, 0x0C: 4, 0x0E: 8}[op]
            value = int.from_bytes(self.take(width, limit), "little")
        self.constants.append((start, self.at - start, value))
        return value

    def resource(self, raw, origin):
        at = 0
        while at < len(raw):
            start = at
            tag = raw[at]
            at += 1
            if tag == 0x79:
                require(at + 1 == len(raw), "missing or misplaced EndTag")
                require(raw[at] == 0 or sum(raw) % 256 == 0, "resource checksum")
                return
            require(tag in (0x88, 0x8A) and at + 2 <= len(raw), "resource tag")
            length = int.from_bytes(raw[at : at + 2], "little")
            at += 2
            width = 2 if tag == 0x88 else 8
            require(length == 3 + 5 * width and length <= len(raw) - at, "resource length")
            kind, general, flags = raw[at : at + 3]
            require(kind <= 2 and general & 0xF0 == 0, "resource general flags")
            mask = (0x0F, 0x33, 0)[kind]
            require(flags & ~mask == 0, "resource specific flags")
            require(kind != 1 or flags & 3 != 0, "reserved IO range type")
            fields = [
                int.from_bytes(raw[at + 3 + i * width : at + 3 + (i + 1) * width], "little")
                for i in range(5)
            ]
            granularity, low, high, translation, size = fields
            require(
                low <= high and size and granularity & (granularity + 1) == 0, "resource bounds"
            )
            if general & 0x0C == 0x0C:
                require(granularity == 0 and size == high - low + 1, "fixed resource bounds")
            self.resources.append((origin + start, kind, general, flags, fields))
            at += length
        raise Invalid("missing EndTag")

    def data_object(self, limit, scope, depth=0):
        require(depth < 64, "data nesting")
        require(self.at < limit, "missing data object")
        op = self.data[self.at]
        if op == 0x12:
            self.at += 1
            end = self.package(limit)
            count = self.byte(end)
            values = [self.data_object(end, scope, depth + 1) for _ in range(count)]
            require(self.at == end, "package element count")
            return values
        if op == 0x11:
            self.at += 1
            end = self.package(limit)
            size = self.integer(end)
            origin = self.at
            raw = self.take(end - self.at, end)
            require(size == len(raw), "buffer size")
            return (raw, origin)
        if op == 0x0D:
            self.at += 1
            raw = bytearray()
            while True:
                value = self.byte(limit)
                if not value:
                    return raw.decode("ascii")
                require(value < 128, "non-ASCII string")
                raw.append(value)
        return self.integer(limit)

    def terms(self, limit, scope=(), depth=0):
        require(depth < 64, "namespace nesting")
        while self.at < limit:
            op = self.byte(limit)
            if op == 8:
                path = self.name(limit, scope)
                require(path not in self.names, "duplicate namespace object")
                self.names.add(path)
                value = self.data_object(limit, scope)
                leaf = path[-1]
                if leaf == "_CRS":
                    require(isinstance(value, tuple), "_CRS type")
                    self.resource(*value)
                if leaf in ("_HID", "_CID", "_UID"):
                    require(isinstance(value, (str, int)), "device ID type")
                    require(
                        not isinstance(value, int) or leaf == "_UID" or value <= 0xFFFFFFFF,
                        "EISA ID width",
                    )
                if leaf == "_PRT":
                    require(isinstance(value, list), "_PRT type")
                    for row in value:
                        require(
                            isinstance(row, list)
                            and len(row) == 4
                            and all(isinstance(v, int) for v in row)
                            and row[0] <= 0xFFFFFFFF
                            and row[1] <= 3
                            and row[2] == 0
                            and row[3] <= 0xFFFFFFFF,
                            "_PRT row",
                        )
                continue
            if op == 0x5B:
                op = 0x5B00 | self.byte(limit)
            require(op in (0x10, 0x5B82, 0x5B83), "unsupported ACPI 2.0 term")
            end = self.package(limit)
            path = self.name(end, scope)
            if op != 0x10:
                require(path not in self.names, "duplicate namespace object")
                self.names.add(path)
            if op == 0x5B83:
                processor = self.byte(end)
                block = self.take(5, end)
                require(block == bytes(5), "unexpected P_BLK")
                require(processor not in self.processors, "duplicate ProcID")
                self.processors.append(processor)
            self.terms(end, path, depth + 1)
            require(self.at == end, "term boundary")


def check_table(table):
    require(len(table) >= 36 and table[:4] in (b"DSDT", b"SSDT"), "definition block header")
    require(int.from_bytes(table[4:8], "little") == len(table), "table length")
    require(table[8] in (1, 2) and sum(table) % 256 == 0, "table revision/checksum")
    grammar = Grammar(table[36:], table[8])
    grammar.terms(len(grammar.data))
    return grammar


def checksum(table):
    table[9] = 0
    table[9] = -sum(table) % 256
    return bytes(table)


def regressions(table):
    grammar = check_table(table)
    variants = []

    def broken(label, at, value):
        data = bytearray(table)
        data[36 + at] = value
        variants.append((label, checksum(data)))

    at, width, _ = next(p for p in grammar.packages if p[1] > 1)
    broken("PkgLength reserved bits", at, table[36 + at] | 0x10)
    broken("PkgLength overruns parent", at, 0xCF)
    broken("invalid NameSeg", table[36:].index(b"PCI0"), ord("9"))
    broken("newer opcode", 0, 0x15)  # ExternalOp arrived after ACPI 2.0.
    resource = next(r for r in grammar.resources if r[1] == 0)
    broken("resource length", resource[0] + 1, 42)
    broken("resource reserved flags", resource[0] + 4, 0x8C)
    broken("resource EndTag", len(table) - 36 - 2, 0x78)
    broken("package element count", grammar.packages[0][0] + grammar.packages[0][1], 5)
    # A revision-1 block cannot contain the ACPI 2.0 QWordConst. Use a
    # self-contained fixture so changing production integer widths is safe.
    body = b"\x08TEST\x0e" + bytes(8)
    header = bytearray(table[:36])
    header[4:8] = (36 + len(body)).to_bytes(4, "little")
    header[8] = 2
    valid = checksum(header + body)
    check_table(valid)
    header[8] = 1
    variants.append(("QWordConst in revision 1", checksum(header + body)))
    for label, variant in variants:
        try:
            check_table(variant)
        except Invalid:
            continue
        raise AssertionError("accepted broken variant: " + label)
    for end in range(36, len(table)):
        data = bytearray(table[:end])
        data[4:8] = end.to_bytes(4, "little")
        try:
            check_table(checksum(data))
        except Invalid:
            continue
        # A prefix ending at a complete top-level object is legal AML.
        require(
            end == 36 or end == 36 + grammar.packages[0][0] + grammar.packages[0][2],
            "accepted truncated object",
        )
    # ACPI 2.0 permits nonminimal widths: never mistake our format choice
    # for a rule of the specification or reject the previous valid emitter.
    g = Grammar(b"\x12\xc8\x00\x00\x00\x01\x0b\x02\x00")
    require(g.data_object(len(g.data), ()) == [2], "legal nonminimal encoding")
    require(g.at == len(g.data), "nonminimal boundary")
    return len(variants)


def check_dumps(directory, compact=False):
    paths = sorted(Path(directory).glob("DSDT*.dat")) + sorted(Path(directory).glob("SSDT*.dat"))
    require(paths, "no definition blocks")
    for path in paths:
        table = path.read_bytes()
        grammar = check_table(table)
        match = re.fullmatch(r"DSDT-(\d+)\.dat", path.name)
        if match:
            require(grammar.processors == list(range(int(match[1]))), "processor topology")
        if compact and path.name.startswith("DSDT"):
            # This is the emitter's format choice, separate from legality.
            require(table[8] == 2, "expected 64-bit definition block revision")
            for _, width, value in grammar.constants:
                expected = (
                    1
                    if value <= 1
                    else 2
                    if value <= 255
                    else 3
                    if value <= 65535
                    else 5
                    if value <= 0xFFFFFFFF
                    else 9
                )
                require(width == expected, "noncompact integer emitted")
            for _, width, length in grammar.packages:
                body = length - width
                expected = next(
                    n
                    for n, maximum in enumerate((0x3F, 0xFFF, 0xFFFFF, 0xFFFFFFF), 1)
                    if body + n <= maximum
                )
                require(width == expected, "noncompact PkgLength emitted")
    # PkgLength can legally use any of its four widths. Check the boundary
    # values even above the production builder's 32 KiB capacity.
    for length, width in ((63, 1), (64, 2), (4095, 2), (4096, 3), (0xFFFFF, 3), (0x100000, 4)):
        encoded = (
            bytes([length])
            if width == 1
            else bytes([((width - 1) << 6) | (length & 15)])
            + bytes((length >> (4 + 8 * i)) & 255 for i in range(width - 1))
        )
        g = Grammar(encoded + bytes(length - width))
        require(g.package(length) == length and g.at == width, "PkgLength boundary")
    count = regressions((Path(directory) / "DSDT.dat").read_bytes())
    print(f"ACPI 2.0 grammar: {len(paths)} definition blocks; {count} broken variants rejected")
