#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check profile PAL/CPUID vectors against the configured IA-64 fork.

The fork retains physical-alias clocks and optional performance controls;
those expectations deliberately differ from the VMM's virtual facilities.
PAL layouts and legal argument cases follow EDK2 IndustryStandard/Pal.h and
SDM Rev.2.3 Vol.2. Explicit fork differences below are checked, not used as
the specification for the VMM/native regression runner.
"""

import os
from pathlib import Path
import re
import select
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu.config import load_local_env, required


def run(qemu, cross, directory, model, index, a, b, expected, c=0, buffer=None, gr4=None):
    available = int(subprocess.check_output(["free", "-g"], text=True).splitlines()[1].split()[-1])
    if available < 6:
        raise RuntimeError("environment: less than 6 GiB available; retry later")
    seed = capture = ""
    if buffer is not None:
        seed = "    movl r16=0x180000\n    mov r17=0x1234\n    ;;\n"
        seed += "".join("    st8 [r16]=r17,8\n    ;;\n" for _ in range(8))
        capture = "    movl r16=0x180000\n    ;;\n"
        capture += "".join(f"    ld8 r{40 + i}=[r16],8\n    ;;\n" for i in range(8))
    branch = "br.call.sptk.many b0=b6" if index & 256 else "br.sptk.many b6"
    source = directory / "pal.S"
    source.write_text(f"""/* SPDX-License-Identifier: GPL-2.0-or-later */
.explicit
.text
.global _start
_start:
    br.sptk start
    ;;
    .org 0x60
    break.m 0x100000
    ;;
    br.sptk.many b0
    ;;
start:
    alloc r14=ar.pfs,0,16,4,0
    ;;
{seed}    mov r4=123
    mov r28={index}
    movl r29={a}
    movl r30={b}
    movl r31={c}
    ;;
    mov r48=r28
    mov r49=r29
    mov r50=r30
    mov r51=r31
    movl r16=0x100060
    movl r17=after_pal
    ;;
    mov b6=r16
    mov b0=r17
    ;;
    {branch}
    ;;
after_pal:
    mov r16=0
    ;;
    mov r20=cpuid[r16]
    add r16=1,r16
    ;;
    mov r21=cpuid[r16]
    add r16=1,r16
    ;;
    mov r22=cpuid[r16]
    add r16=1,r16
    ;;
    mov r23=cpuid[r16]
    add r16=1,r16
    ;;
    mov r24=cpuid[r16]
    ;;
{capture}    break.i 0x1ffffe
""")
    obj, elf, image = (directory / ("pal." + ext) for ext in ("o", "elf", "bin"))
    subprocess.run([cross + "as", str(source), "-o", str(obj)], check=True)
    subprocess.run(
        [cross + "ld", "-Ttext=0x100000", "-e", "_start", str(obj), "-o", str(elf)], check=True
    )
    subprocess.run([cross + "objcopy", "-O", "binary", str(elf), str(image)], check=True)
    command = [
        qemu,
        "-machine",
        "itanium2-vpc,nvram=none",
        "-cpu",
        model,
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
        "-D",
        "/dev/stdout",
        "-accel",
        "tcg,one-insn-per-tb=on",
    ]
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as process:
        data = bytearray()
        deadline = time.monotonic() + 30
        state = None
        try:
            while time.monotonic() < deadline:
                if not select.select([process.stdout], [], [], 0.1)[0]:
                    continue
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    break
                data.extend(chunk)
                terminal = re.search(rb"exception: .*fault_imm: 0x00000000001ffffe[^\n]*\n", data)
                if terminal:
                    tail = data[terminal.end() :]
                    end = re.search(rb"IA64STATE PR value=[0-9a-f]+\n", tail)
                    if end:
                        state = tail[: end.end()].decode("ascii")
                        break
                if len(data) > 8 * 1024 * 1024:
                    raise RuntimeError("PAL oracle did not reach its terminal break")
        finally:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    if state is None:
        raise RuntimeError(f"{model}: PAL {index} timed out or fork startup failed")
    registers = {
        int(i, 16): int(value, 16)
        for i, value in re.findall(r"IA64STATE GR index=([0-9a-f]+) value=([0-9a-f]+)", state)
    }
    actual = tuple(registers[i] for i in range(8, 12))
    assert actual == tuple(v & ((1 << 64) - 1) for v in expected), (
        model,
        index,
        a,
        b,
        actual,
        expected,
    )
    if buffer is not None:
        assert tuple(registers[i] for i in range(40, 48)) == buffer, (
            model,
            index,
            registers,
            buffer,
        )
    if gr4 is not None:
        assert registers[4] == (gr4 & ((1 << 64) - 1)), (model, index, registers[4], gr4)
    assert tuple(registers[i] for i in range(20, 25)) == (
        0x49656E69756E6547,
        0x6C65746E,
        0,
        0x1F010504 if model == "madison" else 0x20000704,
        1 if model == "madison" else 5,
    )


def main():
    load_local_env()
    qemu, cross = required("QEMU_IA64"), required("CROSS_IA64")
    with tempfile.TemporaryDirectory(prefix="pal-oracle-") as directory:
        for model in ("montecito", "madison"):
            madison = model == "madison"
            version = 0x57301000573 if madison else 0x96801000968
            vectors = [
                (4, 0, 0, (0, 3, 4 if madison else 5, 0)),
                (5, 0, 0, (0, 0x71, 0, 0)),
                (6, 0, 0, (0, 0, 0x100000001, 0)),
                (11, 0, 0, (0, 4, 4, 0)),
                (19, 0, 0, (0, 96, 0, 0)),
                (20, 0, 0, (0, version, version, 0)),
                (8, 0, 0, (0, 0x02043F3F080F1865 if madison else 0x02041F1F080F1865, 0x183C, 0)),
                (7, 1, 1, (0, 0x500808001, 0x155557000, 0)),
                (7, 1, 2, (0, 0x500808001, 0x155557000, 0)),
                (34, 0, 0, (0, 0x155557000, 0x155557000, 0)),
                (2, 0, 1, (0, 0x01FF06060400, 0x310C0C00004000, 0)),
                (2, 0, 2, (0, 0x010106060400, 0x310C0C00004000, 0)),
                (2, 1, 1, (-2, 0, 0, 0) if madison else (0, 0x07FF07070800, 0x31110C00100000, 0)),
                (2, 1, 2, (0, 0x050107070803 if madison else 0x050107070802, 0x310F0C00040000, 0)),
                (
                    2,
                    2,
                    2,
                    (0, 0x0E0107070C03, 0x31120C00300000 if madison else 0x31140C00C00000, 0),
                ),
                (38, 2, 2, (0, 0x400C520000000040 if madison else 0x400C540000000040, 0, 0)),
                (42, 0, 0, (-1, 0, 0, 0) if madison else (0, 0x100010001, 0, 0)),
                (43, 0, 1, (-1, 0, 0, 0) if madison else (0, 1, 0, 0)),
                (13, 0, 0, (0, 100000000, 0, 0)),
                (
                    14,
                    0,
                    0,
                    (
                        0,
                        0x64000000064 if madison else 0x1000000001,
                        0x400000001 if madison else 0x1000000003,
                        0x64000000064 if madison else 0x400000001,
                    ),
                ),
            ]
            for feature_set in range(21):
                status = (
                    0
                    if not feature_set
                    else -2
                    if feature_set < 16
                    else (
                        (0 if feature_set == 16 else -8)
                        if madison
                        else (1 if feature_set < 18 else 0 if feature_set == 18 else -8)
                    )
                )
                vectors.append(
                    (
                        17,
                        0,
                        feature_set,
                        (status, 0, 0, 0)
                        if madison or feature_set != 18
                        else (0, 0x400A0, 0x400A0, 0xA0),
                    )
                )
            for index, a, b, expected in vectors:
                run(qemu, cross, Path(directory), model, index, a, b, expected)
            # PAL_HALT_INFO Fig.11-14: the fork also models HALT and
            # synthetic power/latencies. The VMM exposes only LIGHT_HALT.
            run(
                qemu,
                cross,
                Path(directory),
                model,
                257,
                0x180000,
                0,
                (0, 0, 0, 0),
                buffer=(0x300003E800010001, 0x100003E800010001, 0, 0, 0, 0, 0, 0),
            )
            # PAL_PLATFORM_ADDR permits relocation; this fork implements it,
            # while the VMM fixes the windows and reports -1 for alternatives.
            for kind, address in (
                (0, 0xFEE00000),
                (1, 0x3FFFFFC000000),
                (0, 0x200000),
                (1, 0x4000000),
            ):
                run(qemu, cross, Path(directory), model, 16, kind, address, (0, 0, 0, 0))
            run(qemu, cross, Path(directory), model, 16, 2, 0, (-2, 0, 0, 0))
            # Fork validation uses -3 for an unimplemented physical address.
            run(qemu, cross, Path(directory), model, 16, 0, 1 << 50, (-3, 0, 0, 0))
            run(qemu, cross, Path(directory), model, 30, 0, 0, (0, 4096, 4096, 0))
            # Pal.h defines platform counts. This fork still rejects them.
            run(qemu, cross, Path(directory), model, 30, 0, 0x100000001, (-2, 0, 0, 0))
            run(qemu, cross, Path(directory), model, 30, 1, 0, (-3, 0, 0, 0))
            # SDM Fig.11-44 defines phases zero and one. The configured
            # fork's TEST_INFO accepts only zero and its TEST_PROC treats
            # test_info as a raw size. Both deviations are checked here.
            for phase in (0, 1, 2):
                run(
                    qemu,
                    cross,
                    Path(directory),
                    model,
                    37,
                    phase,
                    0,
                    (-2, 0, 0, 0) if phase else (0, 0, 1, 0),
                )
                run(
                    qemu,
                    cross,
                    Path(directory),
                    model,
                    258,
                    0x180000,
                    0x1000 | (phase << 56),
                    (-2, 0, 0, 0) if phase else (0, 4, 0, 0),
                    c=1,
                )
            # Cache-array initialization needs no backing memory (p.2:377).
            for address in (0x180025, 0xFEE00000, 0x300000):
                run(
                    qemu,
                    cross,
                    Path(directory),
                    model,
                    31,
                    address,
                    0xABCDEF,
                    (0, 0, 0, 0),
                    buffer=(0x1234,) * 8,
                )
            run(qemu, cross, Path(directory), model, 31, 1 << 63, 0, (-2, 0, 0, 0))
            # Fork bus controls retain parking and lock masking, disabled
            # at reset; VMM has neither facility.
            run(qemu, cross, Path(directory), model, 9, 0, 0, (0, 0x60000000, 0, 0x60000000))
            run(qemu, cross, Path(directory), model, 10, 0, 0, (0, 0, 0, 0))
            # The fork's generic unavailable dispatcher leaves GR4 unchanged;
            # Pal.h PAL_ENTER_IA_32_ENV requires GR4=-1 in our two PALs.
            run(qemu, cross, Path(directory), model, 33, 0, 0, (-1, 0, 0, 0), gr4=123)
            print(
                f"PAL fork {model}: scalar/control/buffer vectors and CPUID[0..4] passed",
                flush=True,
            )


if __name__ == "__main__":
    main()
