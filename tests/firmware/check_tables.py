#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Disassemble all generated ACPI tables and compile the emitted AML again."""

from pathlib import Path
import os
import shutil
import subprocess
from aml20 import check_dumps

root = Path(__file__).resolve().parents[2]
output = root / "build/firmware-new/acpi-dumps"
output.mkdir(parents=True, exist_ok=True)
local = root / "local.env"
if local.exists():
    for line in local.read_text().splitlines():
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            os.environ.setdefault(key, value)
iasl = os.environ.get("IASL", "iasl")
if not shutil.which(iasl):
    raise SystemExit("iasl is required (install acpica-tools, or set IASL)")
subprocess.run([str(root / "build/tests/firmware-tables"), str(output)], check=True)
check_dumps(output, compact=True)
for name in ("DSDT", "FACP", "APIC", "MCFG", "SRAT", "SLIT", "XSDT", "FACS"):
    (output / (name + ".dsl")).unlink(missing_ok=True)
    command = [iasl, "-d", name + ".dat"]
    result = subprocess.run(command, cwd=output, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (output / (name + ".log")).write_bytes(result.stdout)
    if result.returncode or not (output / (name + ".dsl")).is_file():
        raise SystemExit(result.stdout.decode(errors="replace"))
result = subprocess.run(
    [iasl, "-p", "DSDT-roundtrip", "DSDT.dsl"],
    cwd=output,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
)
(output / "DSDT-roundtrip.log").write_bytes(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout.decode(errors="replace"))
text = (output / "DSDT.dsl").read_text()
for term in (
    "PNP0A08",
    "PNP0A03",
    "_CRS",
    "_PRT",
    "0x0000000080000000",
    "0x0000000120000000",
    "0x0003FFFFFC000000",
):
    if term not in text:
        raise SystemExit("Missing handoff resource in iasl output: " + term)
# RSDP has no SDT header and iasl -d does not accept it; the native test
# checks both root checksums, length and the XSDT address instead.
print("ACPI: eight ACPI dumps disassembled; DSDT round-trip compiled; RSDP checked natively")
