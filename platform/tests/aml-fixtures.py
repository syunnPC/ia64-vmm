#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Evaluate immutable recorded host ACPI, paths from environment/local.env.

The QEMU q35 tables of the reference fork (IA64_FORK_DIR) are required.
ACPI_TABLE_DIRS optionally adds directories of tables recorded on other
hosts, separated by ':'; each holds DSDT.AML and any SSDTn.AML."""

import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu.config import load_local_env, required

load_local_env()
paths = [
    (name, Path(required("IA64_FORK_DIR")) / "tests/data/acpi/x86" / name, "DSDT")
    for name in ["q35", "pc"]
]
recorded = [d for d in os.environ.get("ACPI_TABLE_DIRS", "").split(":") if d]
if not recorded:
    print("AML recorded-table test unavailable: ACPI_TABLE_DIRS unset", flush=True)
for directory in recorded:
    paths.append((directory, Path(directory), "DSDT.AML"))
for label, directory, name in paths:
    dsdt = directory / name
    if not dsdt.is_file():
        raise SystemExit(f"AML {label} DSDT missing: {dsdt}")
    ssdts = sorted(
        p
        for p in directory.iterdir()
        if p.name.startswith("SSDT")
        and (p.name[4:].isdigit() or (p.suffix == ".AML" and p.stem[4:].isdigit()))
    )
    subprocess.run(
        [str(ROOT / "build/tests/aml-routing"), str(dsdt), *map(str, ssdts)], check=True, cwd=ROOT
    )
