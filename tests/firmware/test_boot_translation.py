#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot ITR bounds, linker contracts and the DXE SAL producer."""

import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu.config import load_local_env, required


class BootTranslation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        load_local_env()
        cls.cross = required("CROSS_IA64")
        cls.output = Path("build/tests/boot-translation")
        cls.output.mkdir(parents=True, exist_ok=True)

    def run_tool(self, args, success=True):
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_sal_producer(self):
        original = (ROOT / "firmware/new/acpi/driver.c").read_text()
        for name, text, success in (("normal", original, True),):
            with self.subTest(name=name):
                source = self.output / (name + "-driver.c")
                source.write_text(text)
                binary = self.output / (name + "-driver")
                self.run_tool(
                    [
                        *shlex.split(os.environ.get("HOSTCC", "gcc")),
                        "-std=c11",
                        "-O2",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-D_EFI_P64",
                        "-DMDEPKG_NDEBUG",
                        "-ffunction-sections",
                        "-fdata-sections",
                        "-I.",
                        "-Iinclude",
                        "-Ifirmware/new/acpi",
                        "-Ithird_party/edk2-ipf/MdePkg/Include",
                        "-Ithird_party/edk2-ipf/MdePkg/Include/Ipf",
                        f'-DSAL_DRIVER_SOURCE="{source}"',
                        "tests/firmware/sal-driver.c",
                        "firmware/new/sal/table.c",
                        "firmware/new/sal/sal.c",
                        "-Wl,--gc-sections",
                        "-o",
                        str(binary),
                    ]
                )
                result = self.run_tool([str(binary)], success=success)
                if not success:
                    self.assertIn("table_get(tr+8,8)==addresses[n]", result.stderr)

    def test_linker_bounds(self):
        fixture = (
            '.section .pal,"ax"\n.global __firmware_native_pal\n'
            "__firmware_native_pal:\n.space 32768\n"
            ".text\n.global _start,ivt_base\n_start:\n"
            ".p2align 15\nivt_base:\n.space 32768\n"
            ".section .rodata\n.space 0x100000\n"
        )
        for name, text, error in (
            ("normal", fixture, None),
            (
                "oversize",
                fixture.replace(".text\n", ".text\n.space 0x100000\n"),
                "PAL and native handlers exceed a 1 MiB ITR",
            ),
            (
                "ivt-outside",
                fixture.replace("ivt_base:\n", ".section .rodata\nivt_base:\n"),
                "IVT must be inside the firmware code span",
            ),
            (
                "pal-outside",
                fixture.replace("__firmware_native_pal:\n", "") + "__firmware_native_pal:\n",
                "PAL must start the firmware code page",
            ),
        ):
            with self.subTest(name=name):
                source = self.output / (name + ".S")
                obj, elf = source.with_suffix(".o"), source.with_suffix(".elf")
                source.write_text(text)
                self.run_tool([self.cross + "gcc", "-c", str(source), "-o", str(obj)])
                result = self.run_tool(
                    [
                        self.cross + "ld",
                        "-T",
                        "firmware/new/firmware.lds",
                        str(obj),
                        "-o",
                        str(elf),
                    ],
                    success=error is None,
                )
                if error:
                    self.assertIn(error, result.stderr)
                else:
                    data = elf.read_bytes()
                    (phoff,) = struct.unpack_from("<Q", data, 32)
                    (alignment,) = struct.unpack_from("<Q", data, phoff + 48)
                    self.assertGreaterEqual(alignment, 0x100000)


if __name__ == "__main__":
    unittest.main()
