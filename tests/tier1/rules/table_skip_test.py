#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A mask-0 native run must report every selected BMI rule as skipped."""

import os
from pathlib import Path
import subprocess
import sys

NAMES = {
    "bmi1_andcm",
    "bmi1_blsi",
    "bmi1_blsmsk",
    "bmi1_blsr",
    "bmi1_blsmsk_add",
    "bmi1_blsr_add",
    "bmi1_extru",
    "bmi2_ror_depz",
    "bmi2_ror",
    "bmi2_shl",
    "bmi2_shru",
    "bmi2_shr",
}
result = subprocess.run(
    [
        sys.executable,
        "-I",
        str(Path(__file__).with_name("table_conformance.py")),
        "--runner",
        sys.argv[1],
        "--only",
        "^bmi",
        "--cases",
        "1",
        "--jobs",
        "1",
    ],
    env=dict(os.environ, IA64VMM_HOST_ISA_MASK="0"),
    capture_output=True,
    text=True,
    check=True,
)
skipped = {
    line.split()[2]
    for line in result.stdout.splitlines()
    if line.startswith("table-conformance: isel ") and "SKIPPED" in line
}
if skipped != NAMES or "12 isel rules skipped" not in result.stdout or "passed" in result.stdout:
    raise SystemExit("BMI skip reporting failed:\n" + result.stdout)
print("table-conformance: mask-0 regression: all 12 BMI rules explicitly skipped")
