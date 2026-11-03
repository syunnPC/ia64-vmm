#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""FAT32 file persistence, portable images and record corruption tests."""

import importlib.util
from pathlib import Path
import sys
import struct
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/usb-stick"))
import persistence as p

spec = importlib.util.spec_from_file_location(
    "builder", Path(__file__).resolve().parents[2] / "tools/usb-stick/build.py"
)
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


class Media(unittest.TestCase):
    def test_records(self):
        ident = bytes(range(16))
        a = p.record(ident, 5, b"old\n")
        z = p.record(ident, 6, b"new\n", crash=True)
        self.assertEqual(p.newest([a, z], ident), (6, True, b"new\n"))
        for at in [0, 8, 16, 24, 32, 64, 512]:
            broken = bytearray(z)
            broken[at] ^= 1
            self.assertEqual(p.newest([a, broken], ident), (5, False, b"old\n"))
        self.assertIsNone(p.newest([z[:-1]], ident))
        self.assertIsNone(p.newest([a], bytes(16)))
        # Uncommitted payload or changed length must never supersede the old copy.
        broken = bytearray(z)
        broken[:512] = bytes(512)
        self.assertEqual(p.newest([a, broken], ident)[0], 5)

    def test_circular_record(self):
        ident = bytes(range(16))
        text = b"wrapped text\n"
        header = bytearray(p.record(ident, 7, text)[:512])
        start = p.LOG_BYTES - 512 - 5
        struct.pack_into("<I", header, 8, 2)
        struct.pack_into("<I", header, 52, start)
        struct.pack_into("<I", header, 508, p.zlib.crc32(header[:508]))
        payload = bytearray(p.LOG_BYTES - 512)
        payload[start:] = text[:5]
        payload[: len(text) - 5] = text[5:]
        self.assertEqual(p.newest([header + payload], ident), (7, False, text))

    def build_image(self, pairs=3):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        tree = Path(tmp.name) / "tree"
        tree.mkdir()
        (tree / "hello.efi").write_bytes(b"hello")
        image = Path(tmp.name) / "stick.img"
        b.disk_image(tree, image, pairs=pairs)
        return tree, image

    def test_larger_media_and_file_records(self):
        tree, image = self.build_image()
        with image.open("r+b") as f:
            f.seek(454)
            first = struct.unpack("<I", f.read(4))[0]
            f.seek(first * 512 + 28)
            self.assertEqual(struct.unpack("<I", f.read(4))[0], first)
            m = p.read_manifest(f)
            identity = m["disk_id"]
            self.assertNotIn("capacity", m)
            p.write_file(f, m["slots"][1], p.record(identity, 2, b"round trip\n"))
            p.write_file(f, m["crash"], p.record(identity, 1, b"crash survives\n", crash=True))
            for capacity in [256 * 1024 * 1024, 2 * 1024 * 1024 * 1024]:
                f.truncate(capacity)
                self.assertEqual(p.read_manifest(f)["disk_id"], identity)
                self.assertEqual(p.read_logs(f), (2, False, b"round trip\n"))
                self.assertEqual(p.read_logs(f, True), (1, True, b"crash survives\n"))
        self.assertEqual((tree / "persistence-manifest.bin").stat().st_size, 512)

    def test_tree_manifests(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            p.prepare_tree(tree, pairs=32)
            raw = (tree / "persistence-manifest.bin").read_bytes()
            identity = p.decode_manifest(raw)["disk_id"]
            self.assertEqual(struct.unpack_from("<II", raw, 8), (3, 512))
            self.assertEqual(raw[32:508], bytes(476))
            for fmt in p.NVRAM_FORMATS:
                raw = (tree / p.nvram_manifest_file(fmt)).read_bytes()
                m = p.decode_nvram_manifest(raw)
                self.assertEqual((m["disk_id"], m["pairs"]), (identity, 32))
                self.assertEqual(struct.unpack_from("<II", raw, 8), (2, 512))
                self.assertEqual(raw[40:508], bytes(468))
            p.prepare_tree(tree, pairs=1)
            self.assertNotEqual(
                p.decode_manifest((tree / "persistence-manifest.bin").read_bytes())["disk_id"],
                identity,
            )
            self.assertFalse((tree / "nvram-new-31a.bin").exists())

    def test_fragmented_log_and_nvram(self):
        _, image = self.build_image()
        with image.open("r+b") as f:
            m = p.read_manifest(f)
            n, _ = p.read_nvram(f, "new")
            p.write_file(f, m["slots"][0], p.record(m["disk_id"], 9, bytes(range(256)) * 50))
            window = bytes(range(256)) * 256
            p.write_file(
                f, n["slots"][1][1], p.nvram_record(n["disk_id"], 1, 1, 5, 7, 2, b"Maker\0", window)
            )
            metadata = {}
            p.fat_files(f, metadata)
            geometry = metadata["geometry"]
            # Move each file's second cluster to a free cluster and relink both FATs.
            for item in [m["slots"][0], n["slots"][1][1]]:
                old = item.clusters[1]
                fat_lba, fat_size, data_lba, spc, fats = geometry
                f.seek(fat_lba * 512)
                fat = f.read(fat_size * 512)
                free = next(
                    i
                    for i in range(2, len(fat) // 4)
                    if struct.unpack_from("<I", fat, i * 4)[0] == 0
                )
                f.seek((data_lba + (old - 2) * spc) * 512)
                data = f.read(spc * 512)
                f.seek((data_lba + (free - 2) * spc) * 512)
                f.write(data)
                for copy in range(fats):
                    for cluster, value in [
                        (item.clusters[0], free),
                        (free, item.clusters[2]),
                        (old, 0),
                    ]:
                        f.seek((fat_lba + copy * fat_size) * 512 + cluster * 4)
                        f.write(struct.pack("<I", value))
            fragmented = p.read_manifest(f)["slots"][0]
            self.assertGreater(len(fragmented.extents), 1)
            p.write_file(f, fragmented, p.record(m["disk_id"], 9, bytes(range(256)) * 50))
            self.assertEqual(p.read_logs(f), (9, False, bytes(range(256)) * 50))
            records = p.read_nvram(f, "new")[1]
            self.assertEqual(
                [(q, s, r["sequence"], r["window"]) for q, s, r in records], [(1, 1, 7, window)]
            )

    def test_malformed_fat_metadata(self):
        _, image = self.build_image()
        with image.open("r+b") as f:
            metadata = {}
            files = p.fat_files(f, metadata)
            fat_lba, fat_size, _, _, _ = metadata["geometry"]
            first = metadata["first"] * 512
            log = files["log-a.bin"]
            other = files["log-b.bin"]
            directory = lambda item: item.directory[0] * 512 + item.directory[1]
            cases = [
                (first + 44, struct.pack("<I", 0xFFFFFFF)),
                (first + 19, b"\x01\0"),
                (first + 42, b"\x01\0"),
                (first + 40, b"\x10\0"),
                ((fat_lba + fat_size) * 512 + log.clusters[0] * 4, bytes(4)),
                (directory(log), b"\xe5"),
                (directory(log) + 28, struct.pack("<I", 512)),
                (directory(other) + 20, log.directory[2][20:22]),
                (directory(files["hello.efi"]), log.directory[2]),
                (directory(files["nvram-new-00a.bin"]) + 28, struct.pack("<I", 512)),
            ]
            # Crosslink log-b to log-a; its high cluster word can already match.
            cases[7] = (directory(other) + 26, log.directory[2][26:28])
            for offset, replacement in cases:
                f.seek(offset)
                original = f.read(len(replacement))
                f.seek(offset)
                f.write(replacement)
                with self.subTest(offset=offset), self.assertRaises(ValueError):
                    p.read_nvram(f, "new") if offset == cases[-1][0] else p.read_manifest(f)
                f.seek(offset)
                f.write(original)
            # FAT32 reserves the upper four bits of each FAT entry.
            offset = (fat_lba + fat_size) * 512 + log.clusters[0] * 4
            f.seek(offset)
            original = f.read(4)
            value = struct.unpack("<I", original)[0] ^ 0xF0000000
            f.seek(offset)
            f.write(struct.pack("<I", value))
            p.read_manifest(f)

    def test_invalid_long_name_fields(self):
        _, image = self.build_image()
        with image.open("r+b") as f:
            item = p.fat_files(f)["persistence-manifest.bin"]
            entry = item.directory[0] * 512 + item.directory[1]
            # The LFN immediately before its short entry has sequence 1.
            for within, value in [(0, 0x21), (12, 1), (26, 1)]:
                offset = entry - 32 + within
                f.seek(offset)
                original = f.read(1)
                f.seek(offset)
                f.write(bytes([value]))
                with self.subTest(field=within), self.assertRaises(ValueError):
                    p.read_manifest(f)
                f.seek(offset)
                f.write(original)

    def test_nvram_records(self):
        # Header sector, then the 64 KiB window.
        disk = bytes(range(16))
        old = bytes([1]) * p.NVRAM_WINDOW
        a = p.nvram_record(disk, 1, 3, 4, 9, 1, b"uuid of machine!", old)
        self.assertEqual(len(a), 512 + p.NVRAM_WINDOW)
        r = p.nvram_decode_record(a, disk, 1, 3)
        self.assertEqual(
            (r["generation"], r["sequence"], r["kind"], r["identity"], r["window"]),
            (4, 9, 1, b"uuid of machine!", old),
        )
        for at in [0, 8, 12, 16, 24, 32, 48, 56, 64, 68, 72, 508, 512, 512 + 4000]:
            broken = bytearray(a)
            broken[at] ^= 1
            self.assertIsNone(p.nvram_decode_record(bytes(broken), disk, 1, 3), at)
        self.assertIsNone(p.nvram_decode_record(a, disk, 0, 3))
        self.assertIsNone(p.nvram_decode_record(a, disk, 1, 2))
        self.assertIsNone(p.nvram_decode_record(a, bytes(16), 1, 3))
        with self.assertRaises(ValueError):
            p.nvram_record(disk, 1, 0, 0, 1, 1, b"", old)
        with self.assertRaises(ValueError):
            p.nvram_record(disk, 1, 0, 1, 1, 1, b"", old[:-1])
        with self.assertRaises(ValueError):
            p.nvram_record(disk, 1, 0, 1, 1, 2, bytes(385), old)

    def test_nvram_image(self):
        for pairs in (1, 3, 32):
            with self.subTest(pairs=pairs):
                tree, image = self.build_image(pairs)
                with image.open("r+b") as f:
                    identity = p.read_manifest(f)["disk_id"]
                    for fmt in p.NVRAM_FORMATS:
                        m, records = p.read_nvram(f, fmt)
                        self.assertEqual(
                            (m["format"], m["pairs"], records), (p.NVRAM_FORMAT_ID[fmt], pairs, [])
                        )
                        self.assertEqual(m["disk_id"], identity)
                        self.assertTrue(
                            all(file.size == p.NVRAM_SLOT for pair in m["slots"] for file in pair)
                        )
                    m, _ = p.read_nvram(f, "new")
                    item = m["slots"][0][1]
                    data = p.nvram_record(
                        identity, 1, 0, 5, 7, 2, b"Maker\0", bytes(p.NVRAM_WINDOW)
                    )
                    p.write_file(f, item, data)
                    self.assertEqual(
                        [(q, s, r["sequence"]) for q, s, r in p.read_nvram(f, "new")[1]],
                        [(0, 1, 7)],
                    )
                    p.write_file(f, item, b"x", offset=600)
                    self.assertEqual(p.read_nvram(f, "new")[1], [])

    def test_manifest_validation(self):
        tree, image = self.build_image()
        raw = (tree / "persistence-manifest.bin").read_bytes()
        nvram = (tree / "nvram-new.bin").read_bytes()
        for data, decode, reserved in [
            (raw, p.decode_manifest, 32),
            (nvram, p.decode_nvram_manifest, 40),
        ]:
            for at in [0, 8, 12, 16, 508]:
                bad = bytearray(data)
                bad[at] ^= 1
                with self.assertRaises(ValueError):
                    decode(bad)
            bad = bytearray(data)
            bad[reserved] = 1
            struct.pack_into("<I", bad, 508, p.zlib.crc32(bad[:508]))
            with self.assertRaises(ValueError):
                decode(bad)
        with image.open("r+b") as f:
            files = p.fat_files(f)
            bad = bytearray(nvram)
            bad[16] ^= 1
            struct.pack_into("<I", bad, 508, p.zlib.crc32(bad[:508]))
            p.write_file(f, files["nvram-new.bin"], bad)
            with self.assertRaisesRegex(ValueError, "identity"):
                p.read_nvram(f, "new")


if __name__ == "__main__":
    unittest.main()
