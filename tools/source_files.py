# SPDX-License-Identifier: GPL-2.0-or-later
"""List project sources in a Git checkout or a source archive."""

import os
from pathlib import Path
import subprocess


def source_files(root, *, untracked=False):
    if (root / ".git").exists():
        args = ["git", "ls-files", "-z"]
        if untracked:
            args += ["--cached", "--others", "--exclude-standard"]
        return set(subprocess.check_output(args, cwd=root).decode().split("\0")) - {""}
    files = set()
    for directory, dirs, names in os.walk(root):
        dirs[:] = [
            d
            for d in dirs
            if not d.startswith(".")
            and d != "__pycache__"
            and not (Path(directory) == root and d == "build")
        ]
        for name in names:
            path = (Path(directory) / name).relative_to(root)
            if str(path) not in {"local.env", "usb-stick.local.env"}:
                files.add(str(path))
    return files
