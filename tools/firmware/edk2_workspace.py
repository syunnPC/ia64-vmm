#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Configure the pinned EDK2 workspace for build-file regeneration."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

REVISION = "3e72ffe8afdd03f1f89eba65c921cbdcb004cfee"
REPO = Path(__file__).resolve().parents[2]


def find_python2(env):
    """Python 2.7 for EDK2 BaseTools: EDK2_PYTHON2 (a path, or a command
    name looked up on PATH), else python2 on PATH."""
    configured = env.get("EDK2_PYTHON2") or "python2"
    if os.sep not in configured:
        found = shutil.which(configured, path=env["PATH"])
        return Path(found) if found else None
    path = REPO / configured
    return path if path.is_file() else None


def environment():
    env = os.environ.copy()
    local = REPO / "local.env"
    if local.exists():
        for line in local.read_text().splitlines():
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                env.setdefault(key, value)
    for key in ("EDK2_IPF_DIR", "CROSS_IA64"):
        if not env.get(key):
            sys.exit("Required variable is unset: " + key)
    root = Path(env["EDK2_IPF_DIR"]).resolve()
    if root == REPO or REPO in root.parents:
        sys.exit("EDK2_IPF_DIR must be outside the repository")
    if (
        subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
        != REVISION
    ):
        sys.exit("Expected vUDK2018 checkout at " + REVISION)
    python2 = find_python2(env)
    if python2:
        env["PATH"] = str(python2.parent) + os.pathsep + env["PATH"]
    env.update(
        WORKSPACE=str(root),
        EDK_TOOLS_PATH=str(root / "BaseTools"),
        CONF_PATH=str(root / "Conf"),
        PYTHONPATH=str(root / "BaseTools/Source/Python"),
    )
    env["PATH"] = os.pathsep.join(
        [
            str(root / "BaseTools/BinWrappers/PosixLike"),
            str(root / "BaseTools/Source/C/bin"),
            env["PATH"],
        ]
    )
    return root, env


def configure(root, env, relax, link_plumbing):
    conf = root / "Conf"
    conf.mkdir(exist_ok=True)
    for name in ("target", "build_rule"):
        shutil.copyfile(root / ("BaseTools/Conf/" + name + ".template"), conf / (name + ".txt"))
    definitions = (root / "BaseTools/Conf/tools_def.template").read_text()
    # ELFGCC has an IPF target and ELF->PE conversion. GCC44 only has IA32/X64.
    definitions = re.sub(
        r"(\*_ELFGCC_IPF_\w+_PATH\s*=).*",
        lambda m: (
            m[1]
            + " ENV(CROSS_IA64)"
            + (
                "ar"
                if "SLINK" in m[0] and "ASLDLINK" not in m[0]
                else "ld"
                if "DLINK" in m[0]
                else "objcopy"
                if "RC_PATH" in m[0]
                else "gcc"
            )
        ),
        definitions,
    )
    if relax:
        definitions = re.sub(
            r"(\*_ELFGCC_IPF_CC_FLAGS\s*=.*)",
            lambda m: m[1].replace("-Werror", "-Wno-error"),
            definitions,
        )
    if link_plumbing:
        definitions = re.sub(
            r"(\*_ELFGCC_IPF_CC_FLAGS\s*=.*)",
            r"\1 -fno-strict-aliasing -minline-int-divide-min-latency"
            r" -ffreestanding -fno-builtin",
            definitions,
        )
        definitions = re.sub(
            r"(\*_ELFGCC_IPF_DLINK_FLAGS\s*=.*)", r"\1 -Bsymbolic --no-undefined", definitions
        )
        definitions += (
            "\n*_ELFGCC_IPF_SYMRENAME_PATH = ENV(CROSS_IA64)objcopy\n"
            "*_ELFGCC_IPF_SYMRENAME_FLAGS = --redefine-sym memcpy=CopyMem\n"
        )
    (conf / "tools_def.txt").write_text(definitions)
