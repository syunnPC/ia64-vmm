#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Regenerate selected AutoGen files with Python 2.7 (EDK2_PYTHON2), entirely in /tmp.

Normal builds do not run this tool. The external pinned checkout and its build
outputs are read only; scratch source, configuration and build logs use /tmp.
"""

import os
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from edk2_workspace import environment, configure, find_python2, REVISION, REPO
from snapshot_edk2 import snapshot, EXTRA_INFS


def main():
    root, env = environment()
    python2 = find_python2(env)
    if not python2:
        raise SystemExit("Regeneration requires Python 2.7 (EDK2_PYTHON2 or python2 on PATH)")
    with tempfile.TemporaryDirectory(prefix="edk2-regenerate-", dir="/tmp") as directory:
        work = Path(directory)
        archive = work / "edk2.tar"
        subprocess.run(["git", "archive", "-o", str(archive), REVISION], cwd=root, check=True)
        subprocess.run(["tar", "-xf", str(archive), "-C", str(work)], check=True)
        archive.unlink()
        shutil.copytree(REPO / "firmware/new", work / "firmware/new")
        (work / "firmware/edk2").mkdir(parents=True)
        shutil.copyfile(REPO / "firmware/edk2/Gate.dsc", work / "firmware/edk2/Gate.dsc")
        shutil.copyfile(REPO / "firmware/edk2/VmmDxe.dsc", work / "firmware/edk2/VmmDxe.dsc")
        env.update(
            WORKSPACE=str(work),
            EDK_TOOLS_PATH=str(work / "BaseTools"),
            CONF_PATH=str(work / "Conf"),
            PYTHONPATH=str(work / "BaseTools/Source/Python"),
        )
        env["PATH"] = os.pathsep.join(
            [str(python2.parent), str(work / "BaseTools/Source/C/bin"), env["PATH"]]
        )
        configure(work, env, True, True)
        # The generic VariableRuntimeDxe sources remain IPF-capable, but the
        # DSC limits its component block to x86. This temporary DSC
        # overlay selects it for our platform; imported upstream stays intact.
        dsc = work / "MdeModulePkg/MdeModulePkg.dsc"
        dsc.write_bytes(
            dsc.read_bytes() + b"\n" + (REPO / "firmware/edk2/IpfExtra.dsc.inc").read_bytes()
        )
        manifest = json.loads((REPO / "firmware/edk2/manifest.json").read_text())
        modules = [
            ("firmware/edk2/Gate.dsc", f"firmware/new/gate/{module}.inf")
            for module in ("Driver", "Hello")
        ]
        for key in manifest["roots"]:
            recipe = manifest["recipes"][key]
            if recipe["inf"].startswith("firmware/"):
                continue
            package = recipe["inf"].split("/")[0]
            modules.append(
                (
                    "firmware/edk2/VmmDxe.dsc"
                    if package == "MdeModulePkg"
                    else package + "/" + package + ".dsc",
                    recipe["inf"],
                )
            )
        present = {module for _, module in modules}
        modules += [("firmware/edk2/VmmDxe.dsc", inf) for inf in EXTRA_INFS if inf not in present]
        modules += [
            ("firmware/edk2/VmmDxe.dsc", "firmware/new/dxe/" + name + ".inf")
            for name in ("Arch", "Runtime", "Fvb", "Console")
        ]
        modules.append(("firmware/edk2/VmmDxe.dsc", "firmware/new/acpi/Platform.inf"))
        modules += [
            ("firmware/edk2/VmmDxe.dsc", "firmware/new/pci/" + name + ".inf")
            for name in ("Root", "Keyboard")
        ]
        for platform, module in modules:
            log = work / (Path(module).stem + ".log")
            with log.open("w") as stream:
                result = subprocess.run(
                    [
                        str(python2),
                        str(work / "BaseTools/Source/Python/build/build.py"),
                        "genmake",
                        "-a",
                        "IPF",
                        "-t",
                        "ELFGCC",
                        "-b",
                        "DEBUG",
                        "-p",
                        platform,
                        "-m",
                        module,
                        "-n",
                        "1",
                    ],
                    cwd=work,
                    env=env,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                )
            if result.returncode:
                print(log.read_text())
                raise SystemExit("EDK2 did not generate metadata for " + module)
        snapshot(root, work)


if __name__ == "__main__":
    main()
