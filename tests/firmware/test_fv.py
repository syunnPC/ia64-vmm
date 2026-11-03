# /* SPDX-License-Identifier: GPL-2.0-or-later */
import importlib.util
from pathlib import Path
import struct
import unittest
import uuid

spec = importlib.util.spec_from_file_location(
    "make_fv", Path(__file__).resolve().parents[2] / "tools/firmware/make_fv.py"
)
fv_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fv_module)


class FirmwareVolumeTests(unittest.TestCase):
    def test_pi_checksums_and_sections(self):
        payload = b"MZ" + bytes(range(256)) * 3
        fv = fv_module.make_fv(payload)
        self.assertEqual(len(fv) % 4096, 0)
        self.assertEqual(fv[16:32], uuid.UUID("8c8ce578-8a3d-4f1c-9935-896185c32dd3").bytes_le)
        self.assertEqual(struct.unpack_from("<Q", fv, 32)[0], len(fv))
        self.assertEqual(fv[40:44], b"_FVH")
        self.assertEqual(sum(struct.unpack("<36H", fv[:72])) & 65535, 0)
        file = fv[72:96]
        self.assertEqual(file[:16], uuid.UUID(fv_module.CORE_GUID).bytes_le)
        self.assertEqual(file[18], 5)
        self.assertEqual((sum(file) - file[17] - file[23]) & 255, 0)
        self.assertEqual(file[17], 0xAA)
        self.assertEqual(file[23], 0xF8)
        self.assertEqual(int.from_bytes(file[20:23], "little"), 24 + 4 + len(payload))
        self.assertEqual(int.from_bytes(fv[96:99], "little"), 4 + len(payload))
        self.assertEqual(fv[99], 0x10)
        self.assertEqual(fv[100 : 100 + len(payload)], payload)
        self.assertEqual(set(fv[100 + len(payload) :]), {255})

    def test_large_ffs_rejected(self):
        with self.assertRaises(ValueError):
            fv_module.make_fv(bytes(0x1000000))
