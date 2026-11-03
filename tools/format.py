#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Format project C sources, excluding imported and generated files."""

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from source_files import source_files

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true")
parser.add_argument("--clang-format", default="clang-format")
args = parser.parse_args()
names = sorted(
    name
    for name in source_files(ROOT, untracked=True)
    if (ROOT / name).is_file()
    and Path(name).suffix in {".c", ".h"}
    and not name.startswith(("third_party/", "firmware/edk2/generated/", "firmware/include/"))
    and name != "host/fb/font.h"
)
for start in range(0, len(names), 100):
    subprocess.run(
        [
            args.clang_format,
            *(["--dry-run", "--Werror"] if args.check else ["-i"]),
            *names[start : start + 100],
        ],
        cwd=ROOT,
        check=True,
    )
