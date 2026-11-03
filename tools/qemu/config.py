# SPDX-License-Identifier: GPL-2.0-or-later
"""Load the repository's literal KEY=value settings without executing shell code."""

import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def load_local_env():
    source = ROOT / "local.env"
    if not source.is_file():
        return
    for number, line in enumerate(source.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        key = key.strip()
        if not separator or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
            raise RuntimeError(f"{source}:{number}: expected KEY=value")
        # Make includes this same file: no shell quotes or expansion syntax.
        os.environ.setdefault(key, value.strip())


def required(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} is unset; set it in the environment or local.env")
    return value
