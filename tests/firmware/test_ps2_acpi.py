# SPDX-License-Identifier: GPL-2.0-or-later
"""Native PS/2 resources, PCI0 I/O window and ACPI 2.0 FADT flags."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from aml20 import Grammar

ROOT = Path(__file__).resolve().parents[2]


class Namespace(Grammar):
    def __init__(self, data):
        super().__init__(data)
        self.values = {}
        self.current = ()

    def name(self, limit, scope):
        self.current = super().name(limit, scope)
        return self.current

    def data_object(self, limit, scope, depth=0):
        path = self.current
        value = super().data_object(limit, scope, depth)
        self.values[path] = value
        return value

    def resource(self, raw, origin):
        if raw[0] in (0x47, 0x22):
            # ACPI 2.0 small IO/IRQ resource descriptors, EndTag.
            at = 0
            while raw[at] != 0x79:
                tag = raw[at]
                assert tag in (0x47, 0x22)
                at += 1 + (tag & 7)
            assert at + 2 == len(raw)
        else:
            super().resource(raw, origin)


def check(data, enabled):
    # ACPI 2.0 FADT IAPC_BOOT_ARCH: legacy devices and 8042 present.
    assert int.from_bytes(data[:2], "little") == (3 if enabled else 0)
    data = data[2:]
    ns = Namespace(data)
    ns.terms(len(data))
    base = ("_SB_", "PCI0")
    if enabled:
        raw, _ = ns.values[base + ("_CRS",)]
        at, io = 0, []
        while raw[at] != 0x79:
            width = 2 if raw[at] == 0x88 else 8
            if raw[at + 3] == 1:
                lo = int.from_bytes(raw[at + 6 + width : at + 6 + 2 * width], "little")
                hi = int.from_bytes(raw[at + 6 + 2 * width : at + 6 + 3 * width], "little")
                io.append((lo, hi))
            at += 3 + int.from_bytes(raw[at + 1 : at + 3], "little")
        assert all(any(lo <= port <= hi for lo, hi in io) for port in (0x60, 0x64))
    for device, hid, ports, irq in (
        ("PS2K", 0x0303D041, [0x60, 0x64], 1),
        ("PS2M", 0x130FD041, [], 12),
    ):
        path = base + (device,)
        if not enabled:
            assert path not in ns.names
            continue
        assert ns.values[path + ("_HID",)] == hid
        assert ns.values[path + ("_STA",)] == 15
        raw, _ = ns.values[path + ("_CRS",)]
        at = 0
        for port in ports:
            assert raw[at] == 0x47 and raw[at + 1] == 1
            assert int.from_bytes(raw[at + 2 : at + 4], "little") == port
            assert int.from_bytes(raw[at + 4 : at + 6], "little") == port
            assert raw[at + 6 : at + 8] == bytes([1, 1])
            at += 8
        assert raw[at] == 0x22  # IRQNoFlags: edge, active high, exclusive.
        assert int.from_bytes(raw[at + 1 : at + 3], "little") == 1 << irq
        assert raw[at + 3 :] == bytes([0x79, 0])


class Ps2Acpi(unittest.TestCase):
    def test_presence_and_resources(self):
        with tempfile.TemporaryDirectory(prefix="ps2-acpi-") as directory:
            temp = Path(directory)
            (temp / "fixture.c").write_text("""
#include <stdio.h>
#include "tests/firmware/acpi-map.h"
int main(int argc, char **argv) {
    (void)argv;
    struct ia64vmm_handoff h = {.cpu_count=1, .ecam_bus_last=255,
        .ram_range_count=1, .ram={{0, 0x100000}},
        .ecam_base=IA64VMM_ECAM_BASE, .ecam_size=IA64VMM_ECAM_SIZE,
        .iosapic_base=IA64VMM_IOSAPIC_BASE, .iosapic_gsi_count=24,
        .pib_base=IA64VMM_PIB_BASE, .acpi_pm_port_base=IA64VMM_ACPI_PM_PORT_BASE,
        .acpi_pm_port_size=IA64VMM_ACPI_PM_PORT_SIZE,
        .legacy_io_base=IA64VMM_LEGACY_IO_BASE, .legacy_io_size=IA64VMM_LEGACY_IO_SIZE,
        .pci_io_base=0x60, .pci_io_size=0x3a0};
    if (argc == 2) h.flags = IA64VMM_HF_I8042_ENABLED;
    static struct acpi_image a; unsigned char facs[64];
    if (!test_acpi_build(&h, &a, 0x40000000, facs, 0x50000000)) return 1;
    size_t n = table_get(a.bytes + a.dsdt + 4, 4) - 36;
    if (fwrite(a.bytes + a.fadt + 109, 1, 2, stdout) != 2) return 1;
    return fwrite(a.bytes + a.dsdt + 36, 1, n, stdout) != n;
}
""")
            subprocess.run(
                [
                    os.environ.get("HOSTCC", "cc"),
                    "-std=c11",
                    "-O2",
                    "-I" + str(ROOT),
                    "-I" + str(ROOT / "include"),
                    str(temp / "fixture.c"),
                    str(ROOT / "firmware/new/acpi/aml.c"),
                    str(ROOT / "firmware/new/acpi/tables.c"),
                    "-o",
                    str(temp / "fixture"),
                ],
                check=True,
            )
            check(subprocess.check_output([temp / "fixture"]), False)
            data = subprocess.check_output([temp / "fixture", "enabled"])
            check(data, True)
            with self.assertRaises(AssertionError):
                check(bytes([0, 0]) + data[2:], True)
            ns = Namespace(data[2:])
            ns.terms(len(data) - 2)
            raw, _ = ns.values[("_SB_", "PCI0", "_CRS")]
            # Make the root window start above both child ports.
            bad = bytearray(raw)
            at = 0
            while raw[at] != 0x79:
                width = 2 if raw[at] == 0x88 else 8
                if raw[at + 3] == 1:
                    bad[at + 6 + width : at + 6 + 2 * width] = (0x65).to_bytes(width, "little")
                    high = int.from_bytes(raw[at + 6 + 2 * width : at + 6 + 3 * width], "little")
                    bad[at + 6 + 4 * width : at + 6 + 5 * width] = (high - 0x65 + 1).to_bytes(
                        width, "little"
                    )
                at += 3 + int.from_bytes(raw[at + 1 : at + 3], "little")
            with self.assertRaises(AssertionError):
                check(data.replace(raw, bad), True)
            # Reject semantically wrong resources/IDs and missing presence.
            for before, after in (
                (b"PS2K", b"NONE"),
                (bytes.fromhex("41d00303"), bytes.fromhex("41d00304")),
                (bytes.fromhex("4701600060000101"), bytes.fromhex("4701610061000101")),
                (bytes.fromhex("220200"), bytes.fromhex("220400")),
                (bytes.fromhex("220010"), bytes.fromhex("220020")),
            ):
                self.assertEqual(data.count(before), 1)
                with self.assertRaises((AssertionError, KeyError)):
                    check(data.replace(before, after), True)


if __name__ == "__main__":
    unittest.main()
