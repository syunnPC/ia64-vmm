#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Compile frozen EDK2 recipes without BaseTools or Python 2."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from make_dxe_fv import NAMES

REPO = Path(__file__).resolve().parents[2]
FLAGS = [
    "-std=gnu11",
    "-Os",
    "-fshort-wchar",
    "-Wall",
    "-Wno-error",
    "-D_EFI_P64",
    "-DDISABLE_NEW_DEPRECATED_INTERFACES",
    "-fno-strict-aliasing",
    "-minline-int-divide-min-latency",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-common",
]
# Keep the pinned vendor tree intact; architecture and device fixes use overlays.
PATCHES = {
    "third_party/edk2-ipf/MdeModulePkg/Bus/Ata/AtaAtapiPassThru/IdeMode.c": "firmware/new/pci/atapi-dma.patch",
    "third_party/edk2-ipf/MdeModulePkg/Bus/Pci/SataControllerDxe/SataController.c": "firmware/new/pci/piix-ide.patch",
    "third_party/edk2-ipf/MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressHci.c": "firmware/new/pci/nvme-ipf-unaligned.patch",
    "third_party/edk2-ipf/MdeModulePkg/Library/UefiBootManagerLib/BmBootDescription.c": "firmware/new/pci/nvme-ipf-devicepath.patch",
    "third_party/edk2-ipf/MdeModulePkg/Core/Dxe/Mem/Page.c": "firmware/new/dxe/gcd-uc-mmio.patch",
}


def environment():
    env = os.environ.copy()
    local = REPO / "local.env"
    if local.exists():
        for line in local.read_text().splitlines():
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                env.setdefault(k, v)
    if not env.get("CROSS_IA64"):
        sys.exit("Required variable is unset: CROSS_IA64")
    return env


def run(command):
    subprocess.run(command, cwd=REPO, check=True)


def apply_overlay(source, patch, output):
    result = subprocess.run(
        [
            "patch",
            "--batch",
            "--forward",
            "--fuzz=0",
            "--output",
            str(output),
            str(source),
            str(patch),
        ],
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env={**os.environ, "LC_ALL": "C"},
    )
    if result.returncode or re.search(r"\b(?:offset|fuzz)\b", result.stdout):
        output.unlink(missing_ok=True)
        raise RuntimeError(f"Overlay must apply without fuzz or offsets: {patch}\n{result.stdout}")


def build(names, jobs, release=False):
    env = environment()
    cross = env["CROSS_IA64"]
    manifest = json.loads((REPO / "firmware/edk2/manifest.json").read_text())
    recipes = manifest["recipes"]
    # The release build uses EDK2's release semantics (ASSERT and DEBUG() compile out).
    base = REPO / "build/firmware-new" / ("release" if release else "")
    output = base / "edk2"
    done = {}

    def compile_source(recipe, key, source):
        directory = output / key
        obj = directory / (source.removeprefix("third_party/edk2-ipf/").replace("/", "_") + ".o")
        directory.mkdir(parents=True, exist_ok=True)
        command = [
            cross + "gcc",
            *FLAGS,
            *(["-DMDEPKG_NDEBUG"] if release else []),
            "-ffile-prefix-map=" + str(REPO) + "=.",
        ]
        command += [
            "-I" + str(REPO / inc) for inc in ["include", "firmware/new/pci", *recipe["includes"]]
        ]
        autogen = (
            REPO / "firmware/new/fpswa/AutoGen.h"
            if recipe["name"] == "VmmFpswa"
            else REPO / "firmware/edk2/generated" / key / "AutoGen.h"
        )
        command += ["-include", str(autogen)]
        if source.startswith("firmware/new/"):
            command += ["-std=c11", "-Wextra", "-Werror"]
        signature = hashlib.sha256(json.dumps(command + [source]).encode()).hexdigest()
        patch = REPO / PATCHES[source] if source in PATCHES else None
        if patch:
            signature = hashlib.sha256(
                (signature + hashlib.sha256(patch.read_bytes()).hexdigest()).encode()
            ).hexdigest()
        stamp = obj.with_suffix(".command")
        headers = [p for inc in recipe["includes"] for p in (REPO / inc).rglob("*.h")]
        if source.startswith("firmware/new/") or source in PATCHES:
            headers += list((REPO / "firmware/new").rglob("*.h"))
        if recipe["name"] == "VmmFpswa":
            headers += [
                REPO / ("ia64/" + name + ".c")
                for name in ("fp", "fp_arith", "fp_round", "fp_parallel", "decoder")
            ]
            headers.append(REPO / "ia64/decode/decode.c")
        newest = max(
            p.stat().st_mtime_ns for p in [REPO / source, Path(__file__), autogen, *headers]
        )
        if (
            obj.exists()
            and stamp.exists()
            and stamp.read_text() == signature
            and obj.stat().st_mtime_ns >= newest
        ):
            return str(obj)
        stamp.unlink(missing_ok=True)
        compile_input = REPO / source
        if patch:
            compile_input = directory / Path(source).name
            # Vendored sources use CRLF; normalize only the build copy so the
            # reviewable patch stays LF and the import audit stays byte exact.
            upstream_copy = compile_input.with_suffix(".upstream.c")
            upstream_copy.write_text((REPO / source).read_text())
            apply_overlay(upstream_copy, patch, compile_input)
        if source.endswith((".S", ".s")):
            # The EDK2 build preprocesses MASM-style IPF macros into GNU assembly first.
            assembly = obj.with_suffix(".s")
            run(
                command
                + ["-E", "-x", "assembler-with-cpp", str(REPO / source), "-o", str(assembly)]
            )
            # Equivalent to BaseTools Trim --source-code --trim-long: retain
            # the original assembly, discard C declarations from AutoGen.h.
            retained = []
            original = False
            for line in assembly.read_text().splitlines(keepends=True):
                marker = re.match(r'^\s*#\s*\d+\s+"([^"]+)"', line)
                if marker:
                    original = marker[1] == str(REPO / source)
                elif original:
                    retained.append(re.sub(r"(?<=\w)U?LL\b", "", line))
            assembly.write_text("".join(retained))
            run([cross + "gcc", "-c", "-x", "assembler", str(assembly), "-o", str(obj)])
        else:
            run(command + ["-c", str(compile_input), "-o", str(obj)])
        run([cross + "objcopy", "--redefine-sym", "memcpy=CopyMem", str(obj)])
        stamp.write_text(signature)
        return str(obj)

    def visit(key):
        if key in done:
            return done[key]
        r = recipes[key]
        libs = [visit(dep) for dep in r["libraries"]]
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            objects = list(pool.map(lambda source: compile_source(r, key, source), r["sources"]))
        if r["kind"] == "BASE" or not r["libraries"]:
            target = output / key / (r["name"] + ".a")
            target.unlink(missing_ok=True)
            run([cross + "ar", "rcsD", str(target), *objects])
        else:
            target = base / (r["name"] + ".elf")
            extra = []
            if r["name"] == "VmmFpswa":
                libs.append(
                    subprocess.check_output(
                        [cross + "gcc", "-print-libgcc-file-name"], text=True
                    ).strip()
                )
            # Firmware drivers must not claim an OS loader's fixed address
            # (Linux IA-64 PT_LOAD starts at 0x04000000). A zero preferred
            # base selects DXE's ordinary AllocateAnyPages + PE relocation.
            placement = (
                ["--defsym=__configured_image_base=0"]
                if r["kind"] in ("DXE_DRIVER", "DXE_RUNTIME_DRIVER", "UEFI_DRIVER")
                else []
            )
            if r["name"] == "DxeCore":
                extra = []
            if r["name"] == "GateHello":
                blob = base / "driver-blob.o"
                run([cross + "gcc", "-c", "firmware/new/gate/driver_blob.S", "-o", str(blob)])
                extra = [str(blob)]
            run(
                [
                    cross + "ld",
                    "-static",
                    "--emit-relocs",
                    "--no-undefined",
                    "--build-id=none",
                    *placement,
                    "-T",
                    str(REPO / "firmware/edk2/module.lds"),
                    "--entry=__entry_descriptor",
                    "-u",
                    "_ModuleEntryPoint",
                    "-Map=" + str(target.with_suffix(".map")),
                    "-o",
                    str(target),
                    *extra,
                    "--start-group",
                    *objects,
                    *libs,
                    "--end-group",
                ]
            )
            run(
                [
                    sys.executable,
                    str(REPO / "tools/firmware/elf_to_pe.py"),
                    str(target),
                    str(target.with_suffix(".efi")),
                    "--subsystem",
                    "10"
                    if r["kind"] == "UEFI_APPLICATION"
                    else "12"
                    if r["kind"] == "DXE_RUNTIME_DRIVER"
                    else "11",
                ]
            )
        done[key] = str(target)
        print("EDK2: " + r["name"], flush=True)
        if r["name"] == "GateHello":
            launcher = compile_source(r, key, "firmware/new/gate/launcher.c")
            blob = base / "hello-blob.o"
            run([cross + "gcc", "-c", "firmware/new/gate/hello_blob.S", "-o", str(blob)])
            launcher_elf = base / "GateLauncher.elf"
            autogen_object = next(
                obj for source, obj in zip(r["sources"], objects) if source.endswith("/AutoGen.c")
            )
            run(
                [
                    cross + "ld",
                    "-static",
                    "--emit-relocs",
                    "--no-undefined",
                    "--build-id=none",
                    "--defsym=__configured_image_base=0x08000000",
                    "-u",
                    "_ModuleEntryPoint",
                    "-T",
                    "firmware/edk2/module.lds",
                    "-o",
                    str(launcher_elf),
                    "--start-group",
                    launcher,
                    autogen_object,
                    str(blob),
                    *libs,
                    "--end-group",
                ]
            )
            run(
                [
                    sys.executable,
                    "tools/firmware/elf_to_pe.py",
                    str(launcher_elf),
                    str(launcher_elf.with_suffix(".efi")),
                ]
            )
            behaviour = compile_source(r, key, "firmware/new/gate/behaviour.c")
            virtual = base / "behaviour-virtual.o"
            run([cross + "gcc", "-c", "firmware/new/gate/virtual.S", "-o", str(virtual)])
            behaviour_elf = base / "UefiBehaviour.elf"
            run(
                [
                    cross + "ld",
                    "-static",
                    "--emit-relocs",
                    "--no-undefined",
                    "--build-id=none",
                    "-u",
                    "_ModuleEntryPoint",
                    "-T",
                    "firmware/edk2/module.lds",
                    "-o",
                    str(behaviour_elf),
                    "--start-group",
                    behaviour,
                    str(virtual),
                    autogen_object,
                    *libs,
                    "--end-group",
                ]
            )
            run(
                [
                    sys.executable,
                    "tools/firmware/elf_to_pe.py",
                    str(behaviour_elf),
                    str(behaviour_elf.with_suffix(".efi")),
                ]
            )
            probe = compile_source(r, key, "firmware/new/pal/probe.c")
            unaligned = base / "unaligned-probe.o"
            run([cross + "gcc", "-c", "firmware/new/pal/unaligned-probe.S", "-o", str(unaligned)])
            probe_elf = base / "PalProbe.elf"
            run(
                [
                    cross + "ld",
                    "-static",
                    "--emit-relocs",
                    "--no-undefined",
                    "--build-id=none",
                    "-u",
                    "_ModuleEntryPoint",
                    "-T",
                    "firmware/edk2/module.lds",
                    "-o",
                    str(probe_elf),
                    "--start-group",
                    probe,
                    str(unaligned),
                    str(virtual),
                    autogen_object,
                    *libs,
                    "--end-group",
                ]
            )
            run(
                [
                    sys.executable,
                    "tools/firmware/elf_to_pe.py",
                    str(probe_elf),
                    str(probe_elf.with_suffix(".efi")),
                ]
            )
        return str(target)

    if release:
        # Only the firmware volume's modules; the test applications stay debug-only.
        names = names or {"DxeCore", *NAMES}
    roots = [k for k in manifest["roots"] if not names or recipes[k]["name"] in names]
    if names - {recipes[k]["name"] for k in roots}:
        sys.exit("Unknown module name")
    roots.sort(key=lambda k: (recipes[k]["name"] == "GateHello", k))
    for key in roots:
        visit(key)
    if not release and (not names or "VmmFpswa" in names):
        run([sys.executable, "tests/firmware/build_fpswa.py"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="*", default=[])
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument(
        "--release",
        action="store_true",
        help="build into build/firmware-new/release with MDEPKG_NDEBUG",
    )
    args = parser.parse_args()
    build(set(args.only), args.jobs, args.release)
