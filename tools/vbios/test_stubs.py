#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode the stub segment F0000-FFFFFh with the host objdump (i8086)
and compare it with the intended instructions (the per-vector table at
F000:D000h), once as the SVM backend builds it (VMMCALL) and once as the VMX
backend does (VMCALL). A copy with one changed byte must fail the same
comparison."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# (offset in segment F000h, bytes, instructions as objdump -m i8086 prints
# them); HYPERCALL is the backend's: vmmcall (SVM) or vmcall (VMX).
STUBS = [
    (0xD000, 0x400, ["HYPERCALL", "iret"] * 256),  # vector n at D000h + 4n
    (0xE000, 8, ["lcall $0xc000,$0x3", "HYPERCALL"]),
    (0xE010, 8, ["mov $0x3,%ax", "int $0x10", "HYPERCALL"]),
    (0xE020, 3, ["cli", "jmp 0xe021"]),
    (0xE028, 5, ["int $0x10", "HYPERCALL"]),
    (0xF065, 4, ["HYPERCALL", "iret"]),
    (0xFE6E, 4, ["HYPERCALL", "iret"]),
    (0xFF53, 1, ["iret"]),
]


def decode(path, start, length):
    objdump = os.environ.get("OBJDUMP") or "objdump"  # make exports it empty when unset
    text = subprocess.run(
        [
            objdump,
            "-D",
            "-b",
            "binary",
            "-m",
            "i8086",
            f"--start-address={start:#x}",
            f"--stop-address={start + length:#x}",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    found = []
    for line in text.splitlines():
        match = re.match(r"\s*[0-9a-f]+:\t(?:[0-9a-f]{2} )+\s*\t(.*)$", line)
        if match:
            found.append(" ".join(match.group(1).split()))
    return found


def problems(path, hypercall):
    data = Path(path).read_bytes()
    result = []
    if len(data) != 0x10000:
        return [f"segment is {len(data)} bytes, not 64 KiB"]
    covered = set()
    for start, length, template in STUBS:
        expected = [hypercall if i == "HYPERCALL" else i for i in template]
        got = decode(path, start, length)
        if got != expected:
            result.append(f"F000:{start:04X}: {got} != {expected}")
        covered.update(range(start, start + length))
    stray = [i for i in range(len(data)) if i not in covered and data[i]]
    if stray:
        result.append(f"{len(stray)} nonzero bytes outside the stubs, first at F000:{stray[0]:04X}")
    return result


def main():
    variants = [
        (
            Path(sys.argv[1]),
            "vmmcall",
            [(0xE001, 0x04), (0xE011, 0x13), (0x0000, 0x24), (0xD057, 0x90), (0xE029, 0x15)],
        ),
        (
            Path(sys.argv[2]),
            "vmcall",
            [
                (0xE007, 0xD9),
                (0xFE70, 0xD9),
                (0xE017, 0xD9),
                (0xD3FE, 0xD9),
                (0xF067, 0xD9),
                (0xE02C, 0xD9),
            ],
        ),
    ]
    caught = 0
    for segment, hypercall, broken_bytes in variants:
        found = problems(segment, hypercall)
        for line in found:
            print(f"stub mismatch ({hypercall}): {line}", file=sys.stderr)
        if found:
            return 1
        # Broken variants: SVM: the far call's offset, the int 10h function,
        # a stray byte in the zero area, a table entry without its iret
        # (int 15h's) and the live-state call's vector; VMX: a VMMCALL byte
        # (D9h) left in each stub of the VMCALL segment, the last table entry
        # and F065h among them. Each must be caught.
        data = bytearray(segment.read_bytes())
        for offset, value in broken_bytes:
            broken = bytearray(data)
            broken[offset] = value
            with tempfile.NamedTemporaryFile(suffix=".bin") as handle:
                handle.write(broken)
                handle.flush()
                if not problems(handle.name, hypercall):
                    print(
                        f"broken {hypercall} variant at F000:{offset:04X} not caught",
                        file=sys.stderr,
                    )
                    return 1
            caught += 1
    print(
        f"test-vbios-stubs: F0000 stub segment decodes to the intended instructions with vmmcall and with vmcall "
        f"({caught} broken variants caught)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
