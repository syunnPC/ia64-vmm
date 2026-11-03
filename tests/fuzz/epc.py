#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare EPC promotion and its Illegal Operation fault against the fork."""

import argparse
import fuzz


def source(virtual, cpl, ppl, ar, pl):
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0-or-later */",
        f"// EPC: virtual={virtual}, cpl={cpl}, ppl={ppl}, ar={ar}, pl={pl}",
        ".text",
        ".explicit",
        ".global _start",
        "_start:",
        "movl r2=0x118000 ;;",
        "mov cr.iva=r2 ;;",
        "srlz.i ;;",
        "bsw.1 ;;",
        "mov ar.rsc=0 ;;",
        "rsm psr.ic ;;",
        "srlz.d ;;",
    ]
    if virtual:
        lines += ["mov r2=0 ;;", "mov r3=48 ;;", "mov rr[r2]=r3 ;;", "srlz.d ;;"]
        for slot, va, flags in [
            (0, 0x100000, 0x7E1),
            (1, 0x104000, 0x61 | ar << 9 | pl << 7),
            (2, 0x11A000, 0x7E1),
            (3, 0x11D000, 0x7E1),
        ]:
            lines += [
                f"movl r2=0x{va:x} ;;",
                "mov cr.ifa=r2 ;;",
                "mov r3=48 ;;",
                "mov cr.itir=r3 ;;",
                f"movl r3=0x{va | flags:x} ;;",
                f"mov r2={slot} ;;",
                "itr.i itr[r2]=r3 ;;",
                "srlz.i ;;",
            ]
    flags = (1 << 13) | (1 << 44) | cpl << 32 | ((1 << 36) if virtual else 0)
    lines += [
        f"movl r2=0x{ppl << 62:x} ;;",
        "mov ar.pfs=r2 ;;",
        "mov r15=1024 ;;",
        "mov r10=7 ;;",
        f"movl r2=0x{flags:x} ;;",
        "mov cr.ipsr=r2 ;;",
        "movl r2=0x104000 ;;",
        "mov cr.iip=r2 ;;",
        "mov cr.ifs=r0 ;;",
        "rfi ;;",
        ".org 0x4000",
        "// Unmodified Debian 12 __kernel_syscall_via_epc entry bundle.",
        "data8 0x00a023381e008811, 0x0040000000480000",
        "fuzz_checkpoint:",
        "break.m 0x1ffffe ;;",
        '.section .ivt,"ax"',
        ".org 0x5400",
        "{ .mii",
        "nop.m 0",
        "nop.i 0",
        "nop.i 0 ;; }",
        "break.m 0x1ffffe ;;",
        ".data",
        ".zero 512",
    ]
    return "\n".join(lines) + "\n"


class Fixture:
    def __init__(self, assembly):
        self.assembly = assembly

    def text(self):
        return self.assembly


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--workdir", default="build/fuzz/epc")
    ap.add_argument("--runner", default="build/fuzz/ia64-fuzz-runner")
    args = ap.parse_args()
    oracle = fuzz.Fuzzer(fuzz.Tools(fuzz.load_env(), args.runner, args.workdir), args)
    cases = [(False, cpl, ppl, 0, 0) for cpl in range(4) for ppl in range(4)]
    # Fetchable pages only: the instruction's own fault is compared, rather
    # than an unrelated instruction-access fault before EPC is reached.
    cases += [
        (True, cpl, ppl, ar, pl)
        for ar in (1, 3, 5, 6, 7)
        for pl in range(4)
        for cpl in range(4)
        for ppl in range(4)
        if ar == 7 or (ar == 6 and cpl == pl and cpl > 0) or (ar != 6 and cpl <= pl)
    ]
    failures = 0
    for case in cases:
        name = "-".join(map(str, case))
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
    print(f"EPC: {len(cases) - failures}/{len(cases)} passed")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
