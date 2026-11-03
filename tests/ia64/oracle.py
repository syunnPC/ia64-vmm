#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare the declared guest outputs with the fork and the interpreter.

The raw image loader starts at 0x100000 (hw/ia64/ia64_common.c).
Stop collecting at the first state following the terminal break; otherwise
the empty IVT creates a fault loop and unbounded CPU logs. No assists are
used by these physical-mode tests.
"""

import os
from pathlib import Path
import re
import select
import subprocess
import sys
import time


def run(qemu, image):
    source = Path("tests/ia64") / (image.stem + ".S")
    expected = re.findall(
        r"EXPECT (r|p|nat)(\d+) = (0x[0-9a-fA-F]+|[0-9a-fA-F]+)", source.read_text()
    )
    if not expected:
        raise RuntimeError(f"{source}: no expectations")
    subprocess.run(["build/ia64-test", str(image)], check=True)
    errors = image.with_suffix(".oracle.stderr")
    command = [
        qemu,
        "-machine",
        "itanium2-vpc,nvram=none",
        "-cpu",
        "montecito-9050",
        "-m",
        "128M",
        "-bios",
        str(image),
        "-display",
        "none",
        "-serial",
        "none",
        "-monitor",
        "none",
        "-d",
        "cpu,nochain",
        "-accel",
        "tcg,one-insn-per-tb=on",
        "-D",
        "/dev/stdout",
    ]
    with errors.open("wb") as err:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=err)
        output = bytearray()
        deadline = time.monotonic() + 10
        state = None
        try:
            while time.monotonic() < deadline:
                if not select.select([process.stdout], [], [], 0.1)[0]:
                    if process.poll() is not None:
                        break
                    continue
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    break
                output.extend(chunk)
                # Machine-readable PR follows all GR records, so this
                # proves that the terminal register block is complete.
                match = re.search(
                    rb"(?m)^exception: .*fault_imm: 0x00000000001ffffe[^\n]*\n", output
                )
                if match:
                    tail = output[match.end() :]
                    complete = re.search(rb"IA64STATE PR value=([0-9a-f]{16})\n", tail)
                    if complete:
                        state = bytes(tail[: complete.end()]).decode("ascii")
                        break
                if len(output) > 8 * 1024 * 1024:
                    raise RuntimeError("oracle log limit reached before terminal break")
        finally:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stdout.close()
        if state is None:
            raise RuntimeError(f"{image}: terminal break not reached; see {errors}")
    image.with_suffix(".oracle.log").write_bytes(output)
    registers = {
        int(i, 16): (int(v, 16), int(n))
        for i, v, n in re.findall(
            r"IA64STATE GR index=([0-9a-f]+) value=([0-9a-f]+) nat=(\d+)", state
        )
    }
    pr = int(re.search(r"IA64STATE PR value=([0-9a-f]+)", state)[1], 16)
    for kind, reg, wanted in expected:
        reg, wanted = int(reg), int(wanted, 16)
        actual = (pr >> reg) & 1 if kind == "p" else registers[reg][1 if kind == "nat" else 0]
        if actual != wanted:
            raise RuntimeError(f"{image}: fork {kind}{reg}={actual:#x}, expected {wanted:#x}")
    print(f"oracle: {image.name}: {len(expected)} guest outputs agree")


if __name__ == "__main__":
    images = [Path(p) for p in sys.argv[2:]] or [
        Path(f"build/tests/ia64/{n}.bin")
        for n in (
            "arithmetic",
            "compares",
            "multimedia",
            "memory",
            "registers",
            "rse",
            "system-memory",
            "tlb",
            "interruption",
            "fp",
            "fp-extra",
            "fp-gcc",
            "endian",
            "endian-sem",
            "endian-fp",
            "endian-rse",
        )
    ]
    for path in images:
        run(sys.argv[1], path)
