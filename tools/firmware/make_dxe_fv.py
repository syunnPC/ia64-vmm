#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Pack uncompressed PI DXE files, with actual INF dependency expressions."""

import json
from pathlib import Path
import re
import struct
import sys
import uuid
from make_fv import make_fv

ROOT = Path(__file__).resolve().parents[2]
NAMES = [
    "VmmFvb",
    "SecurityStubDxe",
    "RuntimeDxe",
    "VmmArch",
    "FaultTolerantWriteDxe",
    "VariableRuntimeDxe",
    "VmmFpswa",
    "VmmRuntime",
    "WatchdogTimer",
    "DevicePathDxe",
    "VmmConsole",
    "HiiDatabase",
    "GraphicsConsoleDxe",
    "ConSplitterDxe",
    "VmmTables",
    "VmmPci",
    "PciBusDxe",
    "VmmKeyboard",
    "Ps2KeyboardDxe",
    "SataController",
    "AtaAtapiPassThruDxe",
    "AtaBusDxe",
    "ScsiBus",
    "ScsiDisk",
    "NvmExpressDxe",
    "XhciDxe",
    "EhciDxe",
    "UhciDxe",
    "UsbBusDxe",
    "UsbMassStorageDxe",
    "UsbKbDxe",
    "PartitionDxe",
    "DiskIoDxe",
    "EnglishDxe",
    "Fat",
    "BdsDxe",
]


def section(kind, data):
    size = 4 + len(data)
    if size >= 0xFFFFFF:
        raise ValueError("section exceeds small PI header")
    result = size.to_bytes(3, "little") + bytes([kind]) + data
    return result + b"\0" * ((-size) % 4)


def depex(expression, guids):
    tokens = re.findall(r"\w+|[()]", expression)
    output = bytearray()
    operators = []
    codes = {"AND": 3, "OR": 4, "NOT": 5, "TRUE": 6, "FALSE": 7}
    precedence = {"OR": 1, "AND": 2, "NOT": 3}
    for token in tokens:
        if token == "(":
            operators.append(token)
        elif token == ")":
            while operators and operators[-1] != "(":
                output.append(codes[operators.pop()])
            if not operators:
                raise ValueError("unmatched dependency parenthesis")
            operators.pop()
        elif token in precedence:
            while (
                operators
                and operators[-1] != "("
                and (
                    precedence[operators[-1]] > precedence[token]
                    or (token != "NOT" and precedence[operators[-1]] == precedence[token])
                )
            ):
                output.append(codes[operators.pop()])
            operators.append(token)
        elif token in ("TRUE", "FALSE"):
            output.append(codes[token])
        else:
            output += b"\x02" + guids[token]
    while operators:
        op = operators.pop()
        if op == "(":
            raise ValueError("unmatched dependency parenthesis")
        output.append(codes[op])
    depth = 0
    at = 0
    while at < len(output):
        opcode = output[at]
        at += 1
        if opcode == 2:
            at += 16
            depth += 1
        elif opcode in (6, 7):
            depth += 1
        elif opcode == 5:
            if not depth:
                raise ValueError("NOT without dependency operand")
        else:
            if depth < 2:
                raise ValueError("binary dependency without two operands")
            depth -= 1
    if depth != 1:
        raise ValueError("dependency expression must produce one value")
    return bytes(output) + b"\x08"


def build(release=False):
    out = ROOT / "build/firmware-new" / ("release" if release else "")
    manifest = json.loads((ROOT / "firmware/edk2/manifest.json").read_text())
    recipes = {r["name"]: (k, r) for k, r in manifest["recipes"].items()}
    files = bytearray()
    for name in NAMES:
        key, r = recipes[name]
        inf = ROOT / ("" if r["inf"].startswith("firmware/") else "third_party/edk2-ipf") / r["inf"]
        text = inf.read_text()
        guid = re.search(r"FILE_GUID\s*=\s*([\w-]+)", text)[1]
        autogen = (
            ROOT
            / (
                "firmware/new/fpswa/metadata.c"
                if name == "VmmFpswa"
                else "firmware/edk2/generated/" + key + "/AutoGen.c"
            )
        ).read_text()
        guids = {}
        for dec in (ROOT / "third_party/edk2-ipf").glob("*/*.dec"):
            for symbol, values in re.findall(
                r"^\s*(\w+)\s*=\s*(\{[^\n]+\})", dec.read_text(), re.M
            ):
                numbers = [int(n, 0) for n in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", values)]
                if len(numbers) == 11:
                    guids[symbol] = struct.pack("<IHH8B", *numbers)
        for symbol, values in re.findall(r"\b(?:EFI_GUID|GUID) (\w+) = (\{[^;]+\});", autogen):
            numbers = [int(n, 0) for n in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", values)]
            guids[symbol] = struct.pack("<IHH8B", *numbers)
        dependency = re.search(r"\[Depex(?:\.IPF)?\]\s*([^\[]*)", text)
        expr = (
            " ".join(line.split("#")[0] for line in dependency[1].splitlines())
            if dependency
            else "TRUE"
        )
        # This platform's variable FV must exist before VariableRuntimeDxe's
        # entry copies its PCD-selected physical store. Upstream's TRUE is
        # intended for platforms that formatted the store during PEI.
        if name == "VariableRuntimeDxe":
            expr = "gEfiFirmwareVolumeBlockProtocolGuid AND gEfiFaultTolerantWriteProtocolGuid"
        payload = (
            section(0x13, depex(expr, guids))
            + section(0x15, (name + "\0").encode("utf-16-le"))
            + section(0x10, (out / f"{name}.efi").read_bytes())
        )
        length = 24 + len(payload)
        header = bytearray(
            uuid.UUID(guid).bytes_le + b"\0\0\x07\0" + length.to_bytes(3, "little") + b"\0"
        )
        header[16] = -sum(header) & 255
        header[17] = 0xAA
        header[23] = 0xF8
        files += header + payload + b"\xff" * ((-length) % 8)
    fv = bytearray(make_fv((out / "DxeCore.efi").read_bytes()))
    core_length = int.from_bytes(fv[92:95], "little")
    start = (72 + core_length + 7) & -8
    fv = fv[:start] + files
    fv += b"\xff" * ((-len(fv)) % 4096)
    struct.pack_into("<Q", fv, 32, len(fv))
    struct.pack_into("<I", fv, 56, len(fv) // 4096)
    struct.pack_into("<H", fv, 50, 0)
    struct.pack_into("<H", fv, 50, -sum(struct.unpack("<36H", fv[:72])) & 65535)
    (out / "dxe.fv").write_bytes(fv)
    print(f"DXE FV: DxeCore + {len(NAMES)} drivers, {len(fv)} bytes")


if __name__ == "__main__":
    build("--release" in sys.argv[1:])
