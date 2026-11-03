# SPDX-License-Identifier: GPL-2.0-or-later
"""FAT32 files and capacity-independent log and NVRAM persistence records."""

from dataclasses import dataclass
import secrets
import struct
import zlib

SECTOR = 512
LOG_BYTES = 2 * 1024 * 1024
MANIFEST = b"PLOGMAP1"
RECORD = b"PLOGREC1"
NVRAM_WINDOW = 64 * 1024
NVRAM_SLOT = 128 * 1024
NVRAM_FORMATS = ["new"]
NVRAM_FORMAT_ID = {"new": 1}
NVRAM_PAIRS = 8
NVRAM_MANIFEST = b"NVRAMMAP"
NVRAM_RECORD = b"NVRAMRC1"


def nvram_manifest_file(fmt):
    return f"nvram-{fmt}.bin"


def nvram_slot_file(fmt, pair, slot):
    return f"nvram-{fmt}-{pair:02}{'ab'[slot]}.bin"


def nvram_files(pairs):
    return [
        nvram_slot_file(fmt, p, s) for fmt in NVRAM_FORMATS for p in range(pairs) for s in (0, 1)
    ]


@dataclass(frozen=True)
class FatFile:
    size: int
    clusters: tuple
    extents: tuple  # (LBA, sector count), in file order
    directory: tuple  # directory-entry LBA, byte offset, raw entry


def fat_files(f, metadata=None):
    """Discover an MBR FAT32 ESP and its root files, following every FAT chain."""

    def read(at, size):
        f.seek(at)
        data = f.read(size)
        if len(data) != size:
            raise ValueError("truncated FAT32 media")
        return data

    mbr = read(0, SECTOR)
    if mbr[510:] != b"\x55\xaa":
        raise ValueError("MBR signature missing")
    partition = next((mbr[at : at + 16] for at in range(446, 510, 16) if mbr[at + 4] == 0xEF), None)
    if partition is None:
        raise ValueError("MBR ESP missing")
    first, count = struct.unpack_from("<II", partition, 8)
    f.seek(0, 2)
    if not first or not count or (first + count) * SECTOR > f.tell():
        raise ValueError("ESP range")
    boot = read(first * SECTOR, SECTOR)
    bps, spc, reserved, fats = struct.unpack_from("<HBHB", boot, 11)
    fat_size = struct.unpack_from("<I", boot, 36)[0]
    total = struct.unpack_from("<I", boot, 32)[0]
    root = struct.unpack_from("<I", boot, 44)[0]
    flags, version = struct.unpack_from("<HH", boot, 40)
    fsinfo = struct.unpack_from("<H", boot, 48)[0]
    if (
        boot[510:] != b"\x55\xaa"
        or bps != SECTOR
        or not spc
        or spc > 128
        or spc & (spc - 1)
        or not reserved
        or not fats
        or not fat_size
        or not total
        or total > count
        or version
        or flags & 0xFF70
        or struct.unpack_from("<H", boot, 17)[0]
        or struct.unpack_from("<H", boot, 19)[0]
        or struct.unpack_from("<H", boot, 22)[0]
        or (fsinfo not in (0, 0xFFFF) and fsinfo >= reserved)
    ):
        raise ValueError("unsupported FAT32 geometry")
    data_lba = first + reserved + fats * fat_size
    clusters = (total - reserved - fats * fat_size) // spc
    if not 65525 <= clusters <= 0x0FFFFFEE or clusters + 2 > fat_size * SECTOR // 4:
        raise ValueError("invalid FAT32 geometry")
    active = flags & 15 if flags & 0x80 else 0
    if active >= fats:
        raise ValueError("invalid active FAT")
    fat_lba = first + reserved
    fat = read((fat_lba + active * fat_size) * SECTOR, fat_size * SECTOR)
    if not flags & 0x80:
        for copy in range(1, fats):
            mirror = read((fat_lba + copy * fat_size) * SECTOR, fat_size * SECTOR)
            if any(
                (a[0] ^ b[0]) & 0x0FFFFFFF
                for a, b in zip(struct.iter_unpack("<I", fat), struct.iter_unpack("<I", mirror))
            ):
                raise ValueError("FAT mirrors disagree")

    def chain(cluster):
        seen = set()
        result = []
        while True:
            if cluster < 2 or cluster >= clusters + 2 or cluster in seen:
                raise ValueError("bad FAT chain")
            seen.add(cluster)
            result.append(cluster)
            cluster = struct.unpack_from("<I", fat, cluster * 4)[0] & 0xFFFFFFF
            if cluster >= 0x0FFFFFF8:
                break
        return tuple(result)

    def extents(blocks):
        result = []
        for block in blocks:
            lba = data_lba + (block - 2) * spc
            if result and sum(result[-1]) == lba:
                result[-1] = (result[-1][0], result[-1][1] + spc)
            else:
                result.append((lba, spc))
        return tuple(result)

    root_chain = chain(root)
    occupied = set(root_chain)
    aliases = set()
    if metadata is not None:
        metadata.update(
            geometry=(fat_lba, fat_size, data_lba, spc, fats),
            first=first,
            last=first + count - 1,
            root=root,
            root_chain=root_chain,
        )
    files = {}
    long = {}
    checksum = None
    expected = 0
    for cluster in root_chain:
        lba = data_lba + (cluster - 2) * spc
        directory = read(lba * SECTOR, spc * SECTOR)
        for at in range(0, len(directory), 32):
            e = directory[at : at + 32]
            if e[0] == 0:
                return files
            if e[0] == 0xE5:
                long = {}
                expected = 0
                continue
            if e[11] == 15:
                index = e[0] & 31
                if e[0] & 0x40:
                    long = {}
                    expected = index
                    checksum = e[13]
                if (
                    not 1 <= index <= 20
                    or e[0] & 0xA0
                    or index != expected
                    or e[13] != checksum
                    or e[12]
                    or e[26:28] != bytes(2)
                ):
                    long = {}
                    expected = 0
                    continue
                long[index] = e[1:11] + e[14:26] + e[28:32]
                expected -= 1
                continue
            value = 0
            for byte in e[:11]:
                value = ((value & 1) << 7) + (value >> 1) + byte & 255
            stem, suffix = (
                e[:8].decode("ascii", errors="replace").rstrip(),
                e[8:11].decode("ascii", errors="replace").rstrip(),
            )
            short = stem + ("." + suffix if suffix else "")
            name = short
            if long and not expected and value == checksum:
                units = b"".join(long[k] for k in sorted(long)).decode("utf-16le", errors="replace")
                candidate, marker, padding = units.partition("\0")
                if (
                    0 < len(candidate) <= 255
                    and all(32 <= ord(c) < 127 and c not in '/\\:"*?<>|' for c in candidate)
                    and not candidate.endswith((" ", "."))
                    and (not marker or all(c == "\uffff" for c in padding))
                ):
                    name = candidate
            long = {}
            expected = 0
            if e[11] & 0xD8:
                continue
            if not short or any(not 32 <= ord(c) < 127 for c in short):
                continue
            keys = {short.lower(), name.lower()}
            if keys & aliases:
                raise ValueError("duplicate FAT filename")
            aliases.update(keys)
            size = struct.unpack_from("<I", e, 28)[0]
            start = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
            blocks = chain(start) if start else ()
            if len(blocks) * spc * SECTOR < size:
                raise ValueError("short FAT chain")
            if occupied.intersection(blocks):
                raise ValueError("overlapping FAT chains")
            occupied.update(blocks)
            files[name.lower()] = FatFile(
                size, blocks, extents(blocks), (lba + at // SECTOR, at % SECTOR, bytes(e))
            )
    return files


def read_file(f, item):
    if not isinstance(item, FatFile):
        raise ValueError("missing FAT32 file")
    data = bytearray()
    for lba, count in item.extents:
        f.seek(lba * SECTOR)
        chunk = f.read(min(count * SECTOR, item.size - len(data)))
        data.extend(chunk)
        if len(data) == item.size:
            return bytes(data)
    if len(data) != item.size:
        raise ValueError("truncated FAT32 file")
    return bytes(data)


def write_file(f, item, data, offset=0):
    """Overwrite existing file bytes without changing its allocation or length."""
    if not isinstance(item, FatFile):
        raise ValueError("missing FAT32 file")
    if offset < 0 or offset + len(data) > item.size:
        raise ValueError("file write range")
    for lba, count in item.extents:
        length = count * SECTOR
        if offset >= length:
            offset -= length
            continue
        chunk = data[: length - offset]
        f.seek(lba * SECTOR + offset)
        f.write(chunk)
        data = data[len(chunk) :]
        offset = 0
        if not data:
            return
    if data:
        raise ValueError("short FAT chain")


def prepare_tree(tree, pairs=NVRAM_PAIRS):
    if not 1 <= pairs <= 32:
        raise ValueError("NVRAM_PAIRS must be 1..32")
    identity = secrets.token_bytes(16)
    slots = set(nvram_files(pairs))
    for path in tree.glob("nvram-pad-*.bin"):
        path.unlink()
    for name, size in [(n, LOG_BYTES) for n in ["log-a.bin", "log-b.bin", "crash.bin"]] + [
        (n, NVRAM_SLOT) for n in nvram_files(pairs)
    ]:
        with (tree / name).open("wb") as f:
            f.truncate(size)
    for name in nvram_files(32):
        if name not in slots:
            (tree / name).unlink(missing_ok=True)
    for magic, version, name, fmt in [(MANIFEST, 3, "persistence-manifest.bin", None)] + [
        (NVRAM_MANIFEST, 2, nvram_manifest_file(fmt), index)
        for fmt in NVRAM_FORMATS
        for index in [NVRAM_FORMAT_ID[fmt]]
    ]:
        m = bytearray(SECTOR)
        struct.pack_into("<8sII16s", m, 0, magic, version, SECTOR, identity)
        if fmt is not None:
            struct.pack_into("<II", m, 32, fmt, pairs)
        struct.pack_into("<I", m, 508, zlib.crc32(m[:508]))
        (tree / name).write_bytes(m)


def _manifest(m, magic, version, reserved):
    if len(m) != SECTOR or m[:8] != magic or struct.unpack_from("<II", m, 8) != (version, SECTOR):
        raise ValueError("invalid persistence manifest")
    if zlib.crc32(m[:508]) != struct.unpack_from("<I", m, 508)[0]:
        raise ValueError("manifest checksum")
    if not any(m[16:32]) or any(m[reserved:508]):
        raise ValueError("manifest reserved fields")
    return dict(disk_id=m[16:32])


def decode_manifest(m):
    return _manifest(m, MANIFEST, 3, 32)


def decode_nvram_manifest(m):
    result = _manifest(m, NVRAM_MANIFEST, 2, 40)
    fmt, pairs = struct.unpack_from("<II", m, 32)
    if fmt not in (0, 1) or not 1 <= pairs <= 32:
        raise ValueError("NVRAM manifest fields")
    return dict(result, format=fmt, pairs=pairs)


def _required(files, name, size):
    item = files.get(name)
    if item is None or item.size != size:
        raise ValueError(f"missing or invalid file: {name}")
    return item


def read_manifest(f):
    files = fat_files(f)
    m = decode_manifest(read_file(f, _required(files, "persistence-manifest.bin", SECTOR)))
    return dict(
        m,
        slots=[_required(files, name, LOG_BYTES) for name in ["log-a.bin", "log-b.bin"]],
        crash=_required(files, "crash.bin", LOG_BYTES),
    )


def nvram_record(disk_id, window_format, pair, generation, sequence, kind, identity, window):
    """Header sector: magic, version 1, format, the pair's generation, window
    length, image token, window CRC32, identity kind, stick-wide sequence,
    identity length, pair index, identity bytes; header CRC32 at 508. Then
    the window."""
    if not generation or not sequence or len(window) != NVRAM_WINDOW or len(identity) > 384:
        raise ValueError("NVRAM record fields")
    header = bytearray(SECTOR)
    struct.pack_into(
        "<8sIIQQ16sIIQII",
        header,
        0,
        NVRAM_RECORD,
        1,
        window_format,
        generation,
        len(window),
        disk_id,
        zlib.crc32(window),
        kind,
        sequence,
        len(identity),
        pair,
    )
    header[72 : 72 + len(identity)] = identity
    struct.pack_into("<I", header, 508, zlib.crc32(header[:508]))
    return bytes(header) + window


def nvram_decode_record(data, disk_id, window_format, pair):
    """dict(generation, sequence, kind, identity, window) of a valid record, or None."""
    if len(data) < SECTOR + NVRAM_WINDOW or data[:8] != NVRAM_RECORD:
        return None
    version, fmt, gen, length, media, checksum, kind, seq, idlen, index = struct.unpack_from(
        "<IIQQ16sIIQII", data, 8
    )
    if (
        version != 1
        or fmt != window_format
        or not gen
        or length != NVRAM_WINDOW
        or media != disk_id
        or not seq
        or idlen > 384
        or index != pair
        or zlib.crc32(data[:508]) != struct.unpack_from("<I", data, 508)[0]
    ):
        return None
    window = data[SECTOR : SECTOR + NVRAM_WINDOW]
    if zlib.crc32(window) != checksum:
        return None
    return dict(
        generation=gen, sequence=seq, kind=kind, identity=data[72 : 72 + idlen], window=window
    )


def read_nvram(f, fmt):
    """The format manifest and every valid record: [(pair, slot, record)]."""
    if fmt not in NVRAM_FORMATS:
        raise ValueError("unknown NVRAM format")
    files = fat_files(f)
    m = decode_nvram_manifest(read_file(f, _required(files, nvram_manifest_file(fmt), SECTOR)))
    log = decode_manifest(read_file(f, _required(files, "persistence-manifest.bin", SECTOR)))
    if m["disk_id"] != log["disk_id"] or m["format"] != NVRAM_FORMAT_ID[fmt]:
        raise ValueError("manifest identity mismatch")
    slots = [
        [_required(files, nvram_slot_file(fmt, p, s), NVRAM_SLOT) for s in (0, 1)]
        for p in range(m["pairs"])
    ]
    records = []
    for pair, copies in enumerate(slots):
        for slot, item in enumerate(copies):
            r = nvram_decode_record(read_file(f, item), m["disk_id"], m["format"], pair)
            if r:
                records.append((pair, slot, r))
    return dict(m, slots=slots), records


def record(identity, generation, text, crash=False):
    if not generation or len(text) > LOG_BYTES - SECTOR:
        raise ValueError("record length/generation")
    header = bytearray(SECTOR)
    struct.pack_into(
        "<8sIIQQ16sI",
        header,
        0,
        RECORD,
        1,
        int(crash),
        generation,
        len(text),
        identity,
        zlib.crc32(text),
    )
    struct.pack_into("<I", header, 508, zlib.crc32(header[:508]))
    return bytes(header) + text


def newest(copies, identity):
    valid = []
    for data in copies:
        if len(data) < SECTOR or data[:8] != RECORD:
            continue
        version, flags, gen, length, media, checksum = struct.unpack_from("<IIQQ16sI", data, 8)
        if (
            version not in (1, 2)
            or flags > 1
            or not gen
            or media != identity
            or length > LOG_BYTES - SECTOR
        ):
            continue
        if zlib.crc32(data[:508]) != struct.unpack_from("<I", data, 508)[0]:
            continue
        start = struct.unpack_from("<I", data, 52)[0] if version == 2 else 0
        if start >= LOG_BYTES - SECTOR:
            continue
        first = min(length, LOG_BYTES - SECTOR - start)
        text = (
            data[SECTOR + start : SECTOR + start + first] + data[SECTOR : SECTOR + length - first]
        )
        if len(text) != length or zlib.crc32(text) != checksum:
            continue
        valid.append((gen, bool(flags), text))
    return max(valid, key=lambda v: v[0]) if valid else None


def read_logs(f, crash_only=False):
    m = read_manifest(f)
    copies = []
    for item in [m["crash"]] if crash_only else m["slots"]:
        data = read_file(f, item)
        if len(data) >= 16 and struct.unpack_from("<I", data, 12)[0] == int(crash_only):
            copies.append(data)
    return newest(copies, m["disk_id"])
