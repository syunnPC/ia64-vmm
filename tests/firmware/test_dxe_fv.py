# /* SPDX-License-Identifier: GPL-2.0-or-later */
from pathlib import Path
import sys
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/firmware"))
import make_dxe_fv as pack


class DxeVolumeTests(unittest.TestCase):
    def test_postfix_precedence_and_unary(self):
        guids = {"A": uuid.UUID(int=1).bytes_le, "B": uuid.UUID(int=2).bytes_le}
        a = b"\x02" + guids["A"]
        b = b"\x02" + guids["B"]
        self.assertEqual(pack.depex("A OR B AND NOT A", guids), a + b + a + b"\x05\x03\x04\x08")
        self.assertEqual(pack.depex("NOT NOT (A OR B)", guids), a + b + b"\x04\x05\x05\x08")
        self.assertEqual(pack.depex("TRUE", guids), b"\x06\x08")
        for bad in ("", "A B", "AND A", "A AND", "(A", "A)"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                pack.depex(bad, guids)

    def test_section_header_and_alignment(self):
        for length in range(9):
            data = b"x" * length
            section = pack.section(0x13, data)
            self.assertEqual(int.from_bytes(section[:3], "little"), 4 + length)
            self.assertEqual(section[3], 0x13)
            self.assertEqual(section[4 : 4 + length], data)
            self.assertEqual(len(section) % 4, 0)
        with self.assertRaises(ValueError):
            pack.section(0x10, bytes(0xFFFFFF))
