#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Import pinned source and freeze the EDK2 workspace's generated module compilation recipes.

Only reads the external workspace. Normal builds consume the resulting manifest
and AutoGen sources, never the workspace or its Python 2 tools.
"""

import argparse
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile

from edk2_workspace import environment, REVISION, REPO

AUTOGEN_NOTICE = b"""/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2007 - 2017, Intel Corporation. All rights reserved.
 * EDK2 BaseTools/Source/Python/AutoGen templates; terms in third_party/edk2-ipf/License.txt.
 */
"""

SELECTED = {
    "DxeCore",
    "RuntimeDxe",
    "HiiDatabase",
    "PartitionDxe",
    "DiskIoDxe",
    "AtaAtapiPassThruDxe",
    "AtaBusDxe",
    "NvmExpressDxe",
    "UsbBusDxe",
    "UsbMassStorageDxe",
    "UsbKbDxe",
    "UhciDxe",
    "EhciDxe",
    "XhciDxe",
    "TerminalDxe",
    "ConSplitterDxe",
    "GraphicsConsoleDxe",
    "BdsDxe",
    "PciBusDxe",
    "DevicePathDxe",
    "SecurityStubDxe",
    "WatchdogTimer",
    "Metronome",
    "Fat",
    "EnglishDxe",
    "SataController",
    "ScsiBus",
    "ScsiDisk",
    "Ps2KeyboardDxe",
    "VariableRuntimeDxe",
    "FaultTolerantWriteDxe",
}

EXTRA_INFS = [
    "MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf",
    "MdeModulePkg/Bus/Pci/SataControllerDxe/SataControllerDxe.inf",
    "MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf",
    "MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf",
    "MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf",
    "MdeModulePkg/Bus/Isa/Ps2KeyboardDxe/Ps2KeyboardDxe.inf",
    "MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf",
    "MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf",
]


def variable(text, name):
    match = re.search(r"^" + name + r" = (.*)", text.replace("\\\n", " "), re.M)
    return match[1].split() if match else []


def snapshot(root, gate=None):
    previous_file = REPO / "firmware/edk2/manifest.json"
    previous = (
        json.loads(previous_file.read_text()).get("recipes", {}) if previous_file.exists() else {}
    )
    recipes = {}
    roots = []
    import_dirs = {"MdePkg/Include", "MdeModulePkg/Include"}
    selection = REPO / "third_party/edk2-ipf/selection.txt"
    if selection.exists():
        import_dirs.update(
            line for line in selection.read_text().splitlines() if line and not line.startswith("#")
        )
    metadata = REPO / "firmware/edk2/generated"
    metadata.mkdir(parents=True, exist_ok=True)

    def visit(path):
        workspace = next(p for p in path.parents if p.name == "Build").parent
        key = str(path.parent.relative_to(workspace / "Build"))
        if key in recipes:
            return key
        text = path.read_text()
        name = variable(text, "MODULE_NAME")[0]
        module_dir = variable(text, "MODULE_RELATIVE_DIR")[0]
        if not module_dir.startswith("firmware/"):
            import_dirs.add(module_dir)
        inc = []
        for token in variable(text, "INC"):
            if token.startswith("-I$(WORKSPACE)/"):
                relative = token[len("-I$(WORKSPACE)/") :]
                inc.append(
                    relative
                    if relative.startswith("firmware/")
                    else "third_party/edk2-ipf/" + relative
                )
                if "/Include" in relative:
                    import_dirs.add(relative.split("/Include")[0] + "/Include")
            elif token == "-I$(DEBUG_DIR)":
                inc.append("firmware/edk2/generated/" + key)
            else:
                raise ValueError("Unknown include: " + token)
        sources = []
        for command in text.splitlines():
            if not command.startswith(('\t"$(CC)"', '\t"$(PP)"')):
                continue
            source = command.split(">")[0].split()[-1]
            if not source.endswith((".c", ".S", ".s")):
                raise ValueError("Unknown compilation: " + command)
            source = Path(source).relative_to(workspace)
            if str(source).startswith("Build/"):
                source = "firmware/edk2/generated/" + key + "/" + source.name
            elif not str(source).startswith("firmware/"):
                source = "third_party/edk2-ipf/" + str(source)
            else:
                source = str(source)
            sources.append(source)
        if not sources:
            raise ValueError("No compilation recipes: " + key)
        recipe = dict(
            name=name,
            inf=module_dir + "/" + variable(text, "MODULE_FILE")[0],
            kind=variable(text, "MODULE_TYPE")[0],
            includes=inc,
            sources=sources,
            libraries=[],
        )
        recipes[key] = recipe
        generated = metadata / key
        generated.mkdir(parents=True, exist_ok=True)
        for source in (path.parent / "DEBUG").iterdir():
            if source.suffix in (".h", ".c"):
                contents = source.read_bytes()
                if str(workspace).encode() in contents:
                    raise ValueError("Machine path in generated file: " + str(source))
                target = generated / source.name
                contents = AUTOGEN_NOTICE + contents
                if not target.exists() or target.read_bytes() != contents:
                    target.write_bytes(contents)
        for token in variable(text, "STATIC_LIBRARY_FILES"):
            if token.startswith("$(OUTPUT_DIR)"):
                continue
            if not token.startswith("$(BIN_DIR)/"):
                raise ValueError("Unknown library: " + token)
            # BIN_DIR is the IPF directory for this platform's configuration.
            bin_dir = next(p for p in path.parents if p.name == "IPF")
            dep = bin_dir / token[len("$(BIN_DIR)/") :]
            recipe["libraries"].append(visit(dep.parent.parent / "GNUmakefile"))
        return key

    paths = list(((gate or root) / "Build").rglob("GNUmakefile"))
    for path in sorted(paths):
        if "DEBUG_ELFGCC/IPF" not in str(path):
            continue
        text = path.read_text()
        names = variable(text, "MODULE_NAME")
        if names and names[0] in SELECTED | (
            {
                "GateHello",
                "GateDriver",
                "VmmArch",
                "VmmRuntime",
                "VmmFvb",
                "VmmConsole",
                "VmmPci",
                "VmmKeyboard",
                "VmmTables",
            }
            if gate
            else set()
        ):
            roots.append(visit(path))
    # FPSWA uses its own AutoGen.h and metadata.c rather than BaseTools output.
    # Keep its hand-maintained recipe when replacing the generated recipes.
    for key, recipe in previous.items():
        if gate and recipe["name"] == "VmmFpswa":
            recipes[key] = recipe
            roots.append(key)
    require_names = {recipes[k]["name"] for k in roots}
    if SELECTED - require_names:
        raise ValueError("Missing generated modules: " + repr(SELECTED - require_names))
    # Source is obtained from Git objects, so local workspace plumbing edits cannot
    # enter the unchanged import. Includes retain cross-architecture definitions.
    tracked = subprocess.check_output(
        ["git", "ls-tree", "-r", "--name-only", REVISION], cwd=root, text=True
    ).splitlines()
    packages = {directory.split("/")[0] for directory in import_dirs}
    selected = [
        p
        for p in tracked
        if any(p.startswith(d + "/") for d in import_dirs)
        or p == "License.txt"
        or (
            p.split("/")[0] in packages
            and (p.endswith((".dec", ".dsc")) or "license" in Path(p).name.lower())
        )
    ]
    archive = subprocess.check_output(["git", "archive", REVISION, "--", *selected], cwd=root)
    vendor = REPO / "third_party/edk2-ipf"
    vendor.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        for member in stream:
            if member.isfile():
                target = vendor / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                contents = stream.extractfile(member).read()
                if not target.exists() or target.read_bytes() != contents:
                    target.write_bytes(contents)
    # Python 2's hash iteration can permute otherwise identical source lists.
    # Retain frozen order to keep concurrent manifest edits additive.
    for key, recipe in recipes.items():
        if key not in previous:
            continue
        for field in ("sources", "includes", "libraries"):
            before, current = previous[key][field], recipe[field]
            recipe[field] = [item for item in before if item in current] + [
                item for item in current if item not in before
            ]
    (metadata.parent / "manifest.json").write_text(
        json.dumps(
            dict(
                spdx="/* SPDX-License-Identifier: GPL-2.0-or-later */",
                revision=REVISION,
                roots=roots,
                recipes=recipes,
            ),
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path)
    args = parser.parse_args()
    root, env = environment()
    snapshot(root, args.workspace.resolve() if args.workspace else None)
