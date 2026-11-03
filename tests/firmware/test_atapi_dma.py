#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the EDK2 overlays against a PIIX bus-master/device fixture."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AtapiDma(unittest.TestCase):
    def test_dma_protocol(self):
        recipes = json.loads((ROOT / "firmware/edk2/manifest.json").read_text())["recipes"]
        includes = {"include", "."}
        for recipe in recipes.values():
            if recipe["name"] in ("AtaAtapiPassThruDxe", "SataController"):
                includes.update(recipe["includes"])
        flags = [
            os.environ.get("HOSTCC", "gcc"),
            "-std=c11",
            "-O2",
            "-D_EFI_P64",
            "-D_PCD_GET_MODE_BOOL_PcdAtaSmartEnable=0",
            "-ffunction-sections",
            "-fdata-sections",
        ]
        flags += ["-I" + str(ROOT / p) for p in sorted(includes)]
        originals = {
            name: (ROOT / "firmware/new/pci" / name).read_text()
            for name in ("atapi-dma.h", "piix-ide.h")
        }
        with tempfile.TemporaryDirectory(prefix="atapi-dma-") as directory:
            temp = Path(directory)
            sources = {}
            bus = ROOT / "third_party/edk2-ipf/MdeModulePkg/Bus"
            for name, source, patch in [
                ("IdeMode.c", bus / "Ata/AtaAtapiPassThru/IdeMode.c", "atapi-dma.patch"),
                (
                    "SataController.c",
                    bus / "Pci/SataControllerDxe/SataController.c",
                    "piix-ide.patch",
                ),
            ]:
                upstream = temp / ("upstream-" + name)
                upstream.write_text(source.read_text())
                dest = temp / name
                subprocess.run(
                    [
                        "patch",
                        "--batch",
                        "--silent",
                        "--output",
                        str(dest),
                        str(upstream),
                        str(ROOT / "firmware/new/pci" / patch),
                    ],
                    check=True,
                )
                sources[name] = dest.read_text()
            for header, text in originals.items():
                (temp / header).write_text(text)
            ide = sources["IdeMode.c"]
            (temp / "IdeMode.c").write_text(ide)
            (temp / "SataController.c").write_text(sources["SataController.c"])
            start = ide.index("EFI_STATUS\nEFIAPI\nDetectAndConfigIdeDevice (")
            end = ide.index("\n/**", start)
            (temp / "enumeration.inc").write_text(ide[start:end])
            enumeration = temp / "enumeration"
            subprocess.run(
                [
                    *flags,
                    "-I" + str(temp),
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(ROOT / "tests/firmware/ide-enumeration.c"),
                    "-Wl,--gc-sections",
                    "-o",
                    str(enumeration),
                ],
                check=True,
            )
            result = subprocess.run([str(enumeration)], capture_output=True, cwd=temp)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            objects = []
            for source in ("IdeMode.c", "SataController.c"):
                obj = temp / (source + ".o")
                subprocess.run(
                    [*flags, "-I" + str(temp), "-w", "-c", str(temp / source), "-o", str(obj)],
                    cwd=ROOT,
                    check=True,
                )
                objects.append(str(obj))
            binary = temp / "fixture"
            subprocess.run(
                [
                    *flags,
                    "-I" + str(temp),
                    "-DVMM_TEST_OVERLAY",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(ROOT / "tests/firmware/atapi-dma.c"),
                    *objects,
                    "-Wl,--gc-sections",
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                check=True,
            )
            result = subprocess.run([str(binary)], capture_output=True, cwd=temp, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == "__main__":
    unittest.main()
