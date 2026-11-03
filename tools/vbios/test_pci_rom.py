#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for tools/vbios/pci_rom.py on synthetic ROM images.

Every image is built here from the specifications (PCI Firmware 3.0 §5.1,
UEFI 2.x "PCI Option ROMs", VBE 3.0 PMID) and from the NVIDIA layout the
parser documents. No vendor ROM bytes are used or needed.
"""

import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pci_rom  # noqa: E402


def pcir(vendor, device, cls, length, code_type, last, revision=3, sig=b"PCIR"):
    ds_len = 0x1C if revision >= 3 else 0x18
    body = struct.pack("<4sHHHHB", sig, vendor, device, 0, ds_len, revision)
    body += bytes([cls & 0xFF, cls >> 8 & 0xFF, cls >> 16 & 0xFF])
    body += struct.pack("<HHBB", length // 512, 1, code_type, 0x80 if last else 0)
    if revision >= 3:
        body += struct.pack("<HHH", length // 512, 0, 0)
    return body


def npde(length, last, revision=0x101):
    return struct.pack(
        "<4sHHHBB", b"NPDE", revision, 0x14, length // 512, 0x80 if last else 0, 0
    ) + bytes(8)


def fix_checksum(img, size, at):
    img[at] = (-sum(img[:size]) + img[at]) & 0xFF


def legacy_image(
    length=0x1000,
    last=False,
    with_npde=False,
    npde_last=False,
    pnp=False,
    vendor=0x10DE,
    device=0x2F04,
):
    img = bytearray(length)
    img[0:3] = b"\x55\xaa" + bytes([length // 512])
    img[3:5] = b"\xeb\x4b"  # jmp short 0x50
    img[0x50:0x53] = b"\xe9" + struct.pack("<h", 0x200 - 0x53)  # jmp near 0x200
    img[0x200] = 0xCB  # retf: the "init routine"
    img[0x1E:0x30] = b"IBM VGA Compatible"
    ds = pcir(vendor, device, 0x030000, length, 0x00, last, revision=0)
    img[0x18:0x1A] = struct.pack("<H", 0x100)
    img[0x100 : 0x100 + len(ds)] = ds
    if with_npde:
        n = npde(length, npde_last)
        img[0x120 : 0x120 + len(n)] = n
    if pnp:
        hdr = bytearray(0x20)
        hdr[0:4] = b"$PnP"
        hdr[4] = 1  # structure revision
        hdr[5] = 2  # length in 16-byte units
        hdr[0x0E:0x10] = struct.pack("<H", 0x300)  # manufacturer string
        hdr[0x10:0x12] = struct.pack("<H", 0x310)  # product string
        hdr[0x12:0x15] = bytes([0x00, 0x00, 0x03])  # device type: display
        hdr[9] = (-sum(hdr)) & 0xFF
        img[0x180:0x1A0] = hdr
        img[0x1A:0x1C] = struct.pack("<H", 0x180)
        img[0x300:0x308] = b"Example\0"
        img[0x310:0x318] = b"Display\0"
    else:
        img[0x1A:0x1C] = struct.pack("<H", 0x200)  # points at code, not $PnP
    pm = bytearray(
        b"PMID"
        + struct.pack("<HHHHHHHB", 0x220, 0x220, 0, 0xA000, 0xB000, 0xB800, 0xC000, 0)
        + b"\0"
    )
    pm[19] = (-sum(pm)) & 0xFF
    img[0x60:0x74] = pm
    img[0x400:0x40C] = b"\xff\xb8BIT\x00" + struct.pack("<HBBBB", 0x0100, 12, 6, 3, 0)[:6]
    img[0x40C] = 0
    fix_checksum(img, length, length - 1)
    return img


def efi_image(length=0x800, last=True, with_npde=False, npde_last=False, machine=0x8664):
    img = bytearray(length)
    img[0:4] = b"\x55\xaa" + struct.pack("<H", length // 512)
    img[4:8] = struct.pack("<I", 0x0EF1)
    img[8:0x0E] = struct.pack("<HHH", 11, machine, 0)
    img[0x16:0x18] = struct.pack("<H", 0x80)
    img[0x18:0x1A] = struct.pack("<H", 0x40)
    img[0x80:0x82] = b"MZ"
    ds = pcir(0x10DE, 0x2F04, 0x030000, length, 0x03, last)
    img[0x40 : 0x40 + len(ds)] = ds
    if with_npde:
        n = npde(length, npde_last)
        img[0x60 : 0x60 + len(n)] = n
    return img


def nvidia_image(
    length=0x400,
    last=False,
    with_npde=True,
    npde_last=False,
    sig=b"\x55\xaa",
    ds_sig=b"PCIR",
    device=0x2F04,
):
    img = bytearray(length)
    img[0:2] = sig
    img[0x18:0x1A] = struct.pack("<H", 0x20)
    ds = pcir(0x10DE, device, 0x030000, length, 0xE0, last, sig=ds_sig)
    img[0x20 : 0x20 + len(ds)] = ds
    if with_npde:
        n = npde(length, npde_last)
        img[0x40 : 0x40 + len(n)] = n
    return img


def nvidia_style_dump():
    """Vendor header, PCI chain ending at the EFI image, NPDE continuation."""
    header = bytearray(b"\x4c\xc0" + b"\xff" * 0x3FE)
    chain = (
        nvidia_image(0x400, with_npde=False)
        + legacy_image(0x1000, with_npde=True)
        + efi_image(0x800, last=True, with_npde=True, npde_last=False)
        + nvidia_image(0x600, sig=b"VN", ds_sig=b"NPDS", last=True, npde_last=True, device=0x2F00)
    )
    trailer = nvidia_image(0x400, with_npde=False, device=0x2B80)
    return bytes(header + chain + bytearray(0x200) + trailer)


class PciRomTests(unittest.TestCase):
    def test_nvidia_dump_chain_and_npde_continuation(self):
        rep = pci_rom.parse(nvidia_style_dump())
        self.assertEqual(len(rep["chains"]), 2)
        chain = rep["chains"][0]
        self.assertEqual(chain["start"], 0x400)
        offs = [i["offset"] for i in chain["images"]]
        self.assertEqual(offs, [0x400, 0x800, 0x1800, 0x2000])
        self.assertEqual([i["pcir"]["code_type"] for i in chain["images"]], [0xE0, 0, 3, 0xE0])
        # The PCI indicator ends the option ROM at the EFI image; NPDE goes on.
        self.assertEqual(chain["pci_rom_end"], 0x2000)
        self.assertEqual(chain["end"], 0x2600)
        self.assertEqual(chain["images"][3]["signature"], "VN")
        self.assertEqual(chain["images"][3]["pcir"]["signature"], "NPDS")
        self.assertTrue(chain["images"][3]["pcir"]["npde"]["last"])
        self.assertEqual(rep["chains"][1]["start"], 0x2800)
        self.assertEqual(rep["legacy_images"], [0x800])

    def test_legacy_details(self):
        rep = pci_rom.parse(nvidia_style_dump())
        lg = rep["chains"][0]["images"][1]
        self.assertEqual(lg["pcir"]["revision"], 0)
        self.assertNotIn("max_runtime_length", lg["pcir"])
        d = lg["legacy"]
        self.assertEqual(d["init_size"], 0x1000)
        self.assertTrue(d["checksum_ok"])
        self.assertEqual(d["entry_chain"], [3, 0x50, 0x200])
        self.assertFalse(d["pnp"]["present"])
        self.assertIn("note", d["pnp"])
        self.assertEqual(d["ibm_string"], "IBM VGA Compatible")
        self.assertEqual(len(d["vbe_pmid"]), 1)
        self.assertEqual(d["vbe_pmid"][0]["segments"], [0, 0xA000, 0xB000, 0xB800, 0xC000])
        self.assertEqual(lg["markers"]["nvidia_bit"]["offset"], 0x400)
        self.assertEqual(lg["pcir"]["npde"]["subimage_length"], 0x1000)
        self.assertEqual(lg["warnings"], [])

    def test_efi_details(self):
        img = pci_rom.parse(nvidia_style_dump())["chains"][0]["images"][2]
        e = img["efi"]
        self.assertTrue(e["efi_signature_ok"])
        self.assertEqual((e["subsystem_name"], e["machine_name"]), ("boot service driver", "X64"))
        self.assertTrue(e["pe_header"])
        self.assertTrue(img["pcir"]["last"])
        self.assertEqual(img["pcir"]["max_runtime_length"], 0x800)

    def test_standard_rom_with_pnp(self):
        rom = legacy_image(0x1000, pnp=True, vendor=0x1234, device=0x1111) + efi_image(
            machine=0x200
        )
        rep = pci_rom.parse(rom)
        self.assertEqual(len(rep["chains"]), 1)
        imgs = rep["chains"][0]["images"]
        self.assertEqual(len(imgs), 2)
        pnp = imgs[0]["legacy"]["pnp"]
        self.assertTrue(pnp["present"])
        self.assertTrue(pnp["checksum_ok"])
        self.assertEqual((pnp["manufacturer"], pnp["product"]), ("Example", "Display"))
        self.assertEqual(pnp["device_type"], 0x030000)
        self.assertEqual(imgs[1]["efi"]["machine_name"], "IA64")

    def test_pci_last_stops_without_npde(self):
        rom = legacy_image(0x1000, last=True) + efi_image(last=True)
        chains = pci_rom.parse(rom)["chains"]
        self.assertEqual([len(c["images"]) for c in chains], [1, 1])
        self.assertEqual(chains[0]["pci_rom_end"], 0x1000)

    def test_bad_checksum_is_reported(self):
        img = legacy_image(0x1000, last=True)
        img[0x500] ^= 0x5A
        rep = pci_rom.parse(bytes(img))
        legacy = rep["chains"][0]["images"][0]
        self.assertFalse(legacy["legacy"]["checksum_ok"])
        self.assertIn("legacy initialisation area checksum is not zero", legacy["warnings"])

    def test_truncated_and_malformed_images(self):
        img = legacy_image(0x1000, last=False)
        trunc = pci_rom.parse(bytes(img[:0x800]))
        legacy = trunc["chains"][0]["images"][0]
        self.assertTrue(any("past the end" in w for w in legacy["warnings"]))
        self.assertFalse(legacy["legacy"]["checksum_ok"])
        bad_ptr = bytearray(img)
        bad_ptr[0x18:0x1A] = struct.pack("<H", 0xFFF0)
        self.assertEqual(pci_rom.parse(bytes(bad_ptr))["chains"], [])
        zero = bytearray(img)
        zero[0x110:0x112] = b"\0\0"
        rep = pci_rom.parse(bytes(zero))
        self.assertEqual(rep["chains"][0]["images"][0]["warnings"], ["zero image length"])
        self.assertEqual(pci_rom.parse(b"\xff" * 4096)["chains"], [])
        self.assertEqual(pci_rom.parse(b"\x55\xaa")["chains"], [])

    def test_entry_jump_outside_image(self):
        img = legacy_image(0x1000, last=True)
        img[3:6] = b"\xe9\xf0\x7f"  # jmp near far beyond the image
        chain = pci_rom.jump_chain(bytes(img), 0, 0x1000)
        self.assertEqual(chain[0], 3)
        self.assertTrue(chain[1].startswith("outside image"))

    def test_vendor_markers(self):
        img = legacy_image(0x1000, last=True)
        img[0x30:0x3A] = b" 761295520"
        img[0x48:0x4A] = struct.pack("<H", 0x600)
        img[0x604:0x608] = b"ATOM"
        img[0x700:0x704] = b"$VBT"
        m = pci_rom.parse(bytes(img))["chains"][0]["images"][0]["markers"]
        self.assertTrue(m["amd_atom"]["atom_signature"])
        self.assertEqual(m["intel_vbt"], 0x700)

    def test_cli_json_and_extract(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "dump.rom"
            out = Path(tmp) / "legacy.bin"
            dump = nvidia_style_dump()
            src.write_bytes(dump)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = pci_rom.main([str(src), "--json", "--extract-legacy", str(out)])
            self.assertEqual(rc, 0)
            self.assertEqual(json.loads(buf.getvalue())["legacy_images"], [0x800])
            self.assertEqual(out.read_bytes(), dump[0x800:0x1800])
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = pci_rom.main([str(src)])
            text = buf.getvalue()
            self.assertIn("PCI last-image indicator ends at 0x2000", text)
            self.assertIn("entry 0x3 -> 0x50 -> 0x200", text)
            empty = Path(tmp) / "empty.rom"
            empty.write_bytes(b"\0" * 1024)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(pci_rom.main([str(empty)]), 2)


if __name__ == "__main__":
    unittest.main()
