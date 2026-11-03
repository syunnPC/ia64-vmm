#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Differential RSE fault fixtures and seeded backing-store fault generation."""

import argparse
import random
import fuzz

FAULTS = ("tlb", "nat", "not-present", "key-miss", "key-permission", "rights", "access-bit")


def source(fault, boundary=False, rfi=False, repair="resume", traps=0, seed=0):
    base = 0x120FE8 if boundary else 0x120100
    pte = {
        "tlb": None,
        "nat": 0x7FD,
        "not-present": 0x7E0,
        "key-miss": 0x7E1,
        "key-permission": 0x7E1,
        "rights": 0x661,
        "access-bit": 0x7C1,
    }[fault]
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0-or-later */",
        f"// RSE {fault}, boundary={boundary}, rfi={rfi}, repair={repair}, seed={seed}",
        ".text",
        ".explicit",
        ".global _start",
        "_start:",
        "movl r2=0x118000 ;;",
        "mov cr.iva=r2 ;;",
        "srlz.i ;;",
        "bsw.1 ;;",
        "ssm psr.ic ;;",
        "srlz.d ;;",
        "mov ar.rsc=0 ;;",
        f"movl r2=0x{base:x} ;;",
        "mov ar.bspstore=r2 ;;",
        "mov ar.rnat=r0 ;;",
        "alloc r14=ar.pfs,0,4,0,0 ;;",
    ]
    lines += [f"mov r{32 + i}={100 + i + seed % 100} ;;" for i in range(4)]
    if rfi:
        lines += ["cover ;;", "flushrs ;;"]
    else:
        lines += [
            "br.call.sptk.many b0=.Lcallee ;;",
            ".Ltarget:",
            "nop.m 0 ;;",
            "fuzz_checkpoint:",
            "break.m 0x1ffffe ;;",
            ".Lcallee:",
            "alloc r14=ar.pfs,0,0,0,0 ;;",
            "flushrs ;;",
        ]
    lines += [
        "rsm psr.ic ;;",
        "srlz.d ;;",
        "mov r2=0 ;;",
        "mov r3=48 ;;",
        "mov rr[r2]=r3 ;;",
        "srlz.d ;;",
    ]
    if boundary:
        lines += [
            "movl r2=0x121000 ;;",
            "mov cr.ifa=r2 ;;",
            "mov r3=48 ;;",
            "mov cr.itir=r3 ;;",
            "movl r3=0x1217e1 ;;",
            "mov r2=1 ;;",
            "itr.d dtr[r2]=r3 ;;",
            "srlz.d ;;",
        ]
    if pte is not None:
        lines += [
            "movl r2=0x120000 ;;",
            "mov cr.ifa=r2 ;;",
            f"mov r3={304 if fault.startswith('key') else 48} ;;",
            "mov cr.itir=r3 ;;",
            f"movl r3=0x{0x120000 | pte:x} ;;",
            "mov r2=0 ;;",
            "itr.d dtr[r2]=r3 ;;",
            "srlz.d ;;",
        ]
    if fault.startswith("key"):
        lines += [
            "mov r2=0 ;;",
            "mov r3=1 ;;",
            "mov pkr[r2]=r3 ;;",
            "mov r2=1 ;;",
            f"mov r3={0 if fault == 'key-miss' else 0x105} ;;",
            "mov pkr[r2]=r3 ;;",
            "srlz.d ;;",
        ]
    if fault == "rights":
        lines += ["mov ar.rsc=12 ;;"]
    flags = (1 << 27) | (1 << 13) | (1 << 44) | traps
    if fault.startswith("key"):
        flags |= 1 << 15
    if rfi:
        lines += [
            f"movl r2=0x{flags:x} ;;",
            "mov cr.ipsr=r2 ;;",
            "movl r2=.Ltarget ;;",
            "mov cr.iip=r2 ;;",
            "movl r2=(1 << 63) | 4 | (4 << 7) ;;",
            "mov cr.ifs=r2 ;;",
            "rfi ;;",
            ".Ltarget:",
            "nop.m 0 ;;",
            "fuzz_checkpoint:",
            "break.m 0x1ffffe ;;",
        ]
    else:
        lines += ["mov ar.pfs=r14 ;;"]
        if traps & (1 << 25):
            lines += ["movl r2=3 << 62 ;;", "or r2=r2,r14 ;;", "mov ar.pfs=r2 ;;"]
        lines += [
            f"movl r2=0x{flags:x} ;;",
            "mov cr.ipsr=r2 ;;",
            "movl r2=.Lret ;;",
            "mov cr.iip=r2 ;;",
            "mov cr.ifs=r0 ;;",
            "rfi ;;",
            ".Lret:",
            "br.ret.sptk.many b0 ;;",
        ]
    lines += ['.section .ivt,"ax"']
    for vector in fuzz.IVT_VECTORS:
        lines += [f".org 0x{vector:x}", "{ .mii", "nop.m 0", "nop.i 0", "nop.i 0 ;; }"]
        if repair == "stop" or vector in (0x2C00, 0x5E00, 0x5F00, 0x6000):
            lines += ["break.m 0x1ffffe ;;"]
            continue
        lines += [
            "mov r8=cr.isr ;;",
            "mov r9=cr.ifa ;;",
            "mov r10=cr.iip ;;",
            "mov r11=ar.bsp ;;",
            "mov r12=ar.bspstore ;;",
            "mov ar.rsc=0 ;;",
        ]
        if repair == "cover":
            lines += ["cover ;;"]
        lines += [
            "movl r16=0x120000 ;;",
            "mov cr.ifa=r16 ;;",
            "mov r16=48 ;;",
            "mov cr.itir=r16 ;;",
            "movl r16=0x1207e1 ;;",
            "mov r17=0 ;;",
            "itr.d dtr[r17]=r16 ;;",
            "mov r16=1 ;;",
            "mov pkr[r17]=r16 ;;",
            "srlz.d ;;",
            "rfi ;;",
        ]
    lines += [".data", ".zero 512"]
    return "\n".join(lines) + "\n"


class Fixture:
    def __init__(self, text):
        self.assembly = text

    def text(self):
        return self.assembly


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--workdir", default="build/fuzz/rse-frames")
    ap.add_argument("--seeds", type=int, default=0)
    ap.add_argument("--seed0", type=int, default=1)
    ap.add_argument("--runner", default="build/fuzz/ia64-fuzz-runner")
    args = ap.parse_args()
    oracle = fuzz.Fuzzer(fuzz.Tools(fuzz.load_env(), args.runner, args.workdir), args)
    cases = [
        (fault, boundary, rfi, repair, 0, 0)
        for fault in FAULTS
        for boundary in (False, True)
        for rfi in (False, True)
        for repair in ("stop", "resume", "cover")
    ]
    cases += [
        ("tlb", boundary, False, repair, traps, 0)
        for boundary in (False, True)
        for repair in ("resume", "cover")
        for traps in (1 << 25, 1 << 26, 1 << 40, (1 << 25) | (1 << 26) | (1 << 40))
    ]
    for seed in range(args.seed0, args.seed0 + args.seeds):
        rng = random.Random(seed)
        cases.append(
            (
                rng.choice(FAULTS),
                rng.choice((False, True)),
                False,
                rng.choice(("stop", "resume", "cover")),
                0,
                seed,
            )
        )
    failures = 0
    for i, case in enumerate(cases):
        name = f"{i:03d}-{'-'.join(map(str, case[:4]))}"
        result = oracle.evaluate(Fixture(source(*case)), name, keep=True)
        if result.get("error") or result.get("div"):
            failures += 1
            print(
                name,
                result.get("detail") if result.get("error") else fuzz.describe(result),
                flush=True,
            )
        else:
            print(name, f"PASS ({len(result['ires']['trace'])} bundles)", flush=True)
    print(f"RSE frames: {len(cases) - failures}/{len(cases)} passed")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
