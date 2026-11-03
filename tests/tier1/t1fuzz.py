#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Random differential programs over the Tier1 v1 subset.

Generates random loop programs restricted to the v1 instruction subset:
integer, shift/bitfield, normal and .unc compares (cmp and
cmp4, register and imm8 forms), predicated pure operations, forward
branches inside the loop, side exits, loads (plain, .acq, speculative,
advanced and check; immediate and register post-increment) and stores
(plain, .rel, immediate post-increment), br.cloop as the back-edge, the
in-region helper classes (tbit/tnat, parallel compares) and mf. Some loops
also branch back to the header from the middle of the body (a second
back-edge) or contain counted inner loops (backward
branches to other targets): one or two in sequence, an inner
loop nested in another, a branch back to the header from inside one, and a
loop entered at its header and, from before it, in its body (two entries:
no natural loop); their own counter registers bound them all. Each program
starts with tests/fuzz/fuzz.py's fixed preamble (IVT, PSR.ic, bank 1,
enforced-lazy RSE, a random frame with random size of locals, outputs and
rotating registers; --virtual: fuzz.py's guest MMU set-up). (frame-relative
homes) Before the frame's alloc the preamble moves the
frame base: `alloc r2=ar.pfs,0,K,0,0 ;; br.call` leaves BOF = K with K
dirty registers, K random in 1..95, in about 40 % of the programs chosen so
that the frame wraps the 96-register ring (BOF + sof > 96); the alloc then
spills what does not fit. The program then optionally rotates the frame
with br.ctop (random rrb.gr/rrb.pr), sets random NaT bits on registers the
loop writes only under predicates and never reads, and starts its pointers
at random residues (an unaligned one takes the guest's Unaligned Data
Reference vector). The loop starts at the global label `t1fuzz_loop`;
fuzz.py's epilogue then streams memory through registers.

Half the programs also call functions from the loop body: br.call from any slot, from slot 1 of an MBB bundle, brl.call,
under predicates; the functions allocate a frame on the caller's outputs
(an alloc spills when the frame base leaves too few registers), save and
restore b0 and ar.pfs, call leaf functions (the frame of their outputs),
run chk.s (on ld8.s results and, in physical mode, ldf8.s results,
deferred in physical mode, with recovery code), write ar.ec, read ar.pfs
and return, sometimes first under a predicate. They lie before the loop,
on its pages or a page below them, or after the epilogue. Some loop
bodies run chk.s on a NaT sink: while its NaT bit is set, the check
branches to recovery code that leaves the sink as it is.

--fp (off by default) makes the loops FP-heavy: about half of the body
instructions are FP arithmetic (fma family and pseudo-ops in every
precision and status field, the parallel forms, min/max, compares, class,
conversions, frcpa/frsqrta), FR merges and logic, FR loads and stores
through the loop's pointers, setf/getf and fsetc/fclrf. The set-up gives
the FRs normal, special, single-pair, 82-bit and NaTVal values and FPSR the
Linux default with or without the i flags, an enabled trap, other
controls or random contents. T1H_FP (tests/tier1/fuzz_runner.c) selects
the runner's FP execution.

--mmu-writers (off by default; Tier1 MMU log) ends
the loop body of every virtual program with one MMU-log writer block, so
the writer runs in the lower tier between two region entries: itc.d,
ptc.l/ptc.g/ptc.ga/ptc.e/ptr.d followed by an itc.d, an itr.d/ptr.d pair of
data TRs, itc.i/ptr.i, itr.i/ptr.i, mov rr, mov pkr, mov pkr of one key
into two registers, or a ptc.l whose following load walks the VHPT. The
blocks use r2 and r3 only (outside the body's register pools), alternate
through ar.lc's parity, and keep clear of the pinned region-0 TRs. With
T1H_MMU_MUTE=<family> (tests/tier1/engine_tier1.c) the runner must then
end in hostwin's completeness-check panic (make fuzz-tier1-mmu-mutation).

build/tier1-fuzz-runner runs each program from a snapshot at the loop
header under every engine (interpreter, Tier0, Tier1:
whole runs, random budget splits, with and without the store log,
and random forced-exit arming) and compares with the stepping reference under
the comparison relation. The QEMU fork stays the interpreter's oracle (make fuzz-ia64), not
Tier1's.

    python3 -I tests/tier1/t1fuzz.py --runner build/tier1-fuzz-runner
        [--seeds N] [--seed0 S] [--jobs J] [--virtual-share F] [--keep DIR]
        [--mmu-writers random|BLOCK] [--fp]
"""

import argparse
import importlib.util
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


fuzz = load("fuzz", os.path.join(ROOT, "tests", "fuzz", "fuzz.py"))
MASK64 = fuzz.MASK64
LOAD_BASE = fuzz.SCRATCH  # random words of .data: loads read them
STORE_BASE = fuzz.SCRATCH + 0x400  # stores stay clear of .data and BS_BASE_PTR
LP, SP, INC = 20, 21, 22  # load pointer, store pointer, register increment
CONT, INNER, INNER2 = 23, 24, 25  # counters of the extra back-edges and the inner loops
CRELS = fuzz.CRELS


# --mmu-writers blocks (see the module docstring). ALIAS is fuzz.py's
# region-1 page (VHPT walker, TC insertions); the TR and I-side VAs lie in
# regions 5 and 6 above the 256 MiB that the region-0 TRs map (RID 0 there
# too). Two VAs per block, base + parity * 4 KiB, parity = ar.lc & 1.
ALIAS = (1 << 61) | 0x400000
MMU_TR_VA = (5 << 61) | 0x20000000
MMU_I_VA = (6 << 61) | 0x30000000
MMU_PTE = fuzz.SCRATCH | 0x7E1
MMU_PS = 12 << 2


def _parity(reg):
    return ["mov %s=ar.lc ;;" % reg, "and %s=1,%s ;;" % (reg, reg)]


def _itc(side):
    return [
        "rsm psr.ic ;;",
        "srlz.d ;;",
        "movl r2=0x%x ;;" % ALIAS,
        "mov cr.ifa=r2 ;;",
        "mov r3=%d ;;" % MMU_PS,
        "mov cr.itir=r3 ;;",
        "movl r3=0x%x ;;" % MMU_PTE,
        "itc.%s r3 ;;" % side,
        "ssm psr.ic ;;",
        "srlz.d ;;",
    ]


def _pair(base, purge, insert, slot0):
    """Purges the previous iteration's VA (the other parity), then inserts
    this iteration's VA (TR slot slot0 + parity, or the TC)."""
    lines = _parity("r3") + [
        "shl r3=r3,12 ;;",
        "movl r2=0x%x ;;" % (base + 0x1000),
        "sub r2=r2,r3 ;;",
        "mov r3=%d ;;" % MMU_PS,
        "%s r2,r3 ;;" % purge,
        "srlz.d ;;",
    ]
    lines += _parity("r3") + [
        "shl r3=r3,12 ;;",
        "movl r2=0x%x ;;" % base,
        "add r2=r2,r3 ;;",
        "rsm psr.ic ;;",
        "srlz.d ;;",
        "mov cr.ifa=r2 ;;",
        "mov r2=%d ;;" % MMU_PS,
        "mov cr.itir=r2 ;;",
    ]
    if slot0 is None:
        lines += ["movl r3=0x%x ;;" % MMU_PTE, "%s r3 ;;" % insert]
    else:
        lines += _parity("r2") + [
            "adds r2=%d,r2 ;;" % slot0,
            "movl r3=0x%x ;;" % MMU_PTE,
            "%s[r2]=r3 ;;" % insert,
        ]
    return lines + ["ssm psr.ic ;;", "srlz.d ;;", "srlz.i ;;"]


def _purge_alias(op):
    addr = ["movl r2=0x%x ;;" % ALIAS]
    if op == "ptc.e":
        return addr + ["ptc.e r2 ;;", "srlz.d ;;"]
    return addr + ["mov r3=%d ;;" % MMU_PS, "%s r2,r3 ;;" % op, "srlz.d ;;"]


MMU_BLOCKS = {
    "itc": _itc("d"),
    "ptc_l": _purge_alias("ptc.l") + _itc("d"),
    "ptc_g": _purge_alias("ptc.g") + _itc("d"),
    "ptc_ga": _purge_alias("ptc.ga") + _itc("d"),
    "ptc_e": _purge_alias("ptc.e") + _itc("d"),
    "ptr_tc": _purge_alias("ptr.d") + _itc("d"),
    "tr_d": _pair(MMU_TR_VA, "ptr.d", "itr.d dtr", 6),
    "tc_i": _pair(MMU_I_VA, "ptr.i", "itc.i", None),
    "tr_i": _pair(MMU_I_VA + 0x100000, "ptr.i", "itr.i itr", 6),
    "rr": _parity("r3")
    + [
        "shl r3=r3,8 ;;",
        "movl r2=0x%x ;;" % (0x500 << 8 | 12 << 2),
        "add r3=r2,r3 ;;",
        "movl r2=0x%x ;;" % (7 << 61),
        "mov rr[r2]=r3 ;;",
        "srlz.d ;;",
    ],
    "pkr": _parity("r3")
    + [
        "shl r3=r3,8 ;;",
        "movl r2=0x3001 ;;",
        "add r3=r2,r3 ;;",
        "mov r2=5 ;;",
        "mov pkr[r2]=r3 ;;",
        "srlz.d ;;",
    ],
    "pkr_dup": _parity("r2")
    + ["adds r2=6,r2 ;;", "movl r3=0x3201 ;;", "mov pkr[r2]=r3 ;;", "srlz.d ;;"],
    "walk": [
        "movl r2=0x%x ;;" % ALIAS,
        "mov r3=%d ;;" % (14 << 2),
        "ptc.l r2,r3 ;;",
        "srlz.d ;;",
        "ld8 r3=[r2] ;;",
    ],
}


FPSR_LINUX = 0x0009804C0270033F
FPSR_I = sum(1 << (18 + 13 * sf) for sf in range(4))  # the i flags of sf0-sf3
FP_DOUBLES = [
    0,
    1 << 63,
    0x3FF0000000000000,
    0xBFF0000000000000,
    0x7FF0000000000000,
    0xFFF0000000000000,
    0x7FF8000000000000,
    0x7FF4000000000000,
    1,
    0x000FFFFFFFFFFFFF,
    0x0010000000000000,
    0x7FEFFFFFFFFFFFFF,
    0x3FEFFFFFFFFFFFFF,
    0x4330000000000000,
]
FP_SINGLES = [
    0,
    0x80000000,
    0x3F800000,
    0x7F800000,
    0x7FC00000,
    0x7FA00000,
    1,
    0x007FFFFF,
    0x00800000,
    0x7F7FFFFF,
    0x3F7FFFFF,
]
FP_TEMP = 127  # set-up scratch FR, outside the pool


class V1Gen:
    def __init__(self, seed, virtual, mmu_writers=None, fp=False):
        self.g = fuzz.Gen(seed, 0, virtual=virtual)
        self.rng = self.g.rng
        self.seed = seed
        self.label = 0
        self.virtual = virtual
        self.mmu_writers = mmu_writers
        self.fp = fp

    def lbl(self, kind):
        self.label += 1
        return ".L%s%d" % (kind, self.label)

    def qp(self):
        return 0 if self.rng.random() < 0.55 else self.rng.randrange(1, 64)

    @staticmethod
    def pfx(qp):
        return "(p%d) " % qp if qp else ""

    def pred_pair(self):
        return self.g.pred_pair()

    def counter_pair(self):
        """A compare's targets whose first is not p0: it qualifies a counted branch."""
        p1, p2 = self.pred_pair()
        return (p2, p1) if p1 == 0 else (p1, p2)

    # -- register pools ---------------------------------------------------
    def setup_pools(self):
        rng = self.rng
        static = [r for r in range(4, 32) if r not in (LP, SP, INC, CONT, INNER, INNER2, 12)]
        stacked = self.g.frame.regs
        self.regs = static + stacked
        self.sinks = rng.sample(self.regs, rng.randrange(0, 3))
        self.dests = [r for r in self.regs if r not in self.sinks]
        self.srcs = list(self.dests)

    def src(self):
        return 0 if self.rng.random() < 0.06 else self.rng.choice(self.srcs)

    def dest(self, qp):
        # NaT sinks: written only under a predicate and never read.
        if qp and self.sinks and self.rng.random() < 0.3:
            return self.rng.choice(self.sinks)
        return self.rng.choice(self.dests)

    # -- instruction families ---------------------------------------------
    def alu(self, qp):
        rng = self.rng
        d, a, b = self.dest(qp), self.src(), self.src()
        k = rng.randrange(12)
        if k == 0:
            op = rng.choice(["add", "sub", "and", "or", "xor", "andcm"])
            if op in ("add", "sub") and rng.random() < 0.3:
                t = "%s r%d=r%d,r%d,1" % (op, d, a, b)
            else:
                t = "%s r%d=r%d,r%d" % (op, d, a, b)
        elif k == 1:
            t = "adds r%d=%d,r%d" % (d, rng.randrange(-8192, 8192), b)
        elif k == 2:
            t = "addl r%d=%d,r%d" % (d, rng.randrange(-(1 << 21), 1 << 21), rng.randrange(0, 4))
        elif k == 3:
            op = rng.choice(["sub", "and", "or", "xor", "andcm"])
            t = "%s r%d=%d,r%d" % (op, d, rng.randrange(-128, 128), b)
        elif k == 4:
            t = "shladd r%d=r%d,%d,r%d" % (d, a, rng.randrange(1, 5), b)
        elif k == 5:
            t = "%s r%d=r%d,r%d" % (rng.choice(["shl", "shr", "shr.u"]), d, a, b)
        elif k == 6:
            t = "%s r%d=r%d,%d" % (rng.choice(["shl", "shr", "shr.u"]), d, a, rng.randrange(64))
        elif k == 7:
            t = "%s r%d=r%d,%d,%d" % (
                rng.choice(["extr", "extr.u"]),
                d,
                a,
                rng.randrange(64),
                rng.randrange(1, 65),
            )
        elif k == 8:
            if rng.random() < 0.5:
                t = "dep.z r%d=r%d,%d,%d" % (d, a, rng.randrange(64), rng.randrange(1, 65))
            else:
                t = "dep.z r%d=%d,%d,%d" % (
                    d,
                    rng.randrange(-128, 128),
                    rng.randrange(64),
                    rng.randrange(1, 65),
                )
        elif k == 9:
            t = "%s r%d=r%d" % (rng.choice(["sxt1", "sxt2", "sxt4", "zxt1", "zxt2", "zxt4"]), d, a)
        elif k == 10:
            t = "movl r%d=0x%x" % (d, self.g.value())
        else:
            t = "mov r%d=r%d" % (d, a)
        return [self.pfx(qp) + t]

    def cmp(self, qp):
        rng = self.rng
        p1, p2 = self.pred_pair()
        op = rng.choice(["cmp", "cmp4"])
        rel = rng.choice(CRELS)
        unc = ".unc" if rng.random() < 0.4 else ""
        if rng.random() < 0.4:
            # The assembler rewrites le/gt/leu/gtu with imm8 +- 1; 32-bit
            # and 64-bit unsigned forms of leu/gtu have no imm8 0.
            imm = rng.randrange(-127, 128)
            if rel in ("leu", "gtu") and imm == 0:
                imm = 1
            t = "%s.%s%s p%d,p%d=%d,r%d" % (op, rel, unc, p1, p2, imm, self.src())
        else:
            t = "%s.%s%s p%d,p%d=r%d,r%d" % (op, rel, unc, p1, p2, self.src(), self.src())
        return [self.pfx(qp) + t]

    def helper(self, qp):
        rng = self.rng
        p1, p2 = self.pred_pair()
        k = rng.randrange(3)
        ctype = rng.choice(["and", "or", "or.andcm"])
        if k == 0:
            t = "tbit.%s%s p%d,p%d=r%d,%d" % (
                rng.choice(["z", "nz"]),
                rng.choice(["", ".unc", "." + ctype]),
                p1,
                p2,
                self.src(),
                rng.randrange(64),
            )
        elif k == 1:
            t = "tnat.%s%s p%d,p%d=r%d" % (
                rng.choice(["z", "nz"]),
                rng.choice(["", ".unc", "." + ctype]),
                p1,
                p2,
                self.src(),
            )
        else:
            op, form = rng.choice(["cmp", "cmp4"]), rng.random()
            if form < 0.5:
                a = "r%d" % self.src()
                rel = rng.choice(["eq", "ne"])
            elif form < 0.75:
                a = "%d" % rng.randrange(-128, 128)
                rel = rng.choice(["eq", "ne"])
            else:  # the zero forms: r0 against r3
                a = "r0"
                rel = rng.choice(["lt", "le", "gt", "ge"])
            t = "%s.%s.%s p%d,p%d=%s,r%d" % (op, rel, ctype, p1, p2, a, self.src())
        return [self.pfx(qp) + t]

    def load(self, qp):
        rng = self.rng
        n = rng.choice([1, 2, 4, 8])
        kind = rng.random()
        if kind < 0.6:
            acq = ".acq" if rng.random() < 0.2 else ""
        else:  # speculative, advanced, check
            acq = rng.choice([".s", ".sa", ".a", ".c.clr", ".c.nc", ".c.clr.acq"])
        d = self.dest(qp)
        inc = rng.random()
        if inc < 0.4:
            post = ",%d" % rng.choice([8, 8, 16, -8])
        elif inc < 0.6:
            post = ",r%d" % INC
        else:
            post = ""
        return [self.pfx(qp) + "ld%d%s r%d=[r%d]%s" % (n, acq, d, LP, post)]

    def store(self, qp):
        rng = self.rng
        n = rng.choice([1, 2, 4, 8])
        rel = ".rel" if rng.random() < 0.2 else ""
        post = ",%d" % rng.choice([8, 16]) if rng.random() < 0.6 else ""
        return [self.pfx(qp) + "st%d%s [r%d]=r%d%s" % (n, rel, SP, self.src(), post)]

    # -- FP (--fp) --------------------------------------------------------
    def fr(self, dest=False):
        if not dest and self.rng.random() < 0.08:
            return self.rng.randrange(2)
        return self.rng.choice(self.frs)

    def sf(self):
        return ".s%d" % self.rng.randrange(4)

    def fp_double(self):
        rng = self.rng
        if rng.random() < 0.25:
            return rng.choice(FP_DOUBLES)
        return (
            rng.getrandbits(1) << 63 | (1023 - 10 + rng.randrange(21)) << 52 | rng.getrandbits(52)
        )

    def fp_single(self):
        rng = self.rng
        if rng.random() < 0.25:
            return rng.choice(FP_SINGLES)
        return rng.getrandbits(1) << 31 | (127 - 8 + rng.randrange(17)) << 23 | rng.getrandbits(23)

    def fp_init(self, f):
        """FR f: a double, a single, a pair of singles, an 82-bit value or NaTVal."""
        rng, k = self.rng, self.rng.random()
        if k < 0.4:
            return ["movl r2=0x%x ;;" % self.fp_double(), "setf.d f%d=r2 ;;" % f]
        if k < 0.6:
            return ["movl r2=0x%x ;;" % self.fp_single(), "setf.s f%d=r2 ;;" % f]
        if k < 0.75:
            return [
                "movl r2=0x%x ;;" % (self.fp_single() << 32 | self.fp_single()),
                "setf.sig f%d=r2 ;;" % f,
            ]
        if k < 0.95:
            exp = (
                rng.choice(
                    [0, 0x1FFFF, 0x1003E, 0xFF81, 0xFC01, 0x1007E, 0x103FE, rng.getrandbits(17)]
                )
                if rng.random() < 0.3
                else 0xFFFF - 70 + rng.randrange(141)
            )
            sig = rng.getrandbits(64) | (1 << 63 if rng.random() < 0.85 else 0)
            return [
                "movl r2=0x%x ;;" % sig,
                "movl r3=0x%x ;;" % (rng.getrandbits(1) << 17 | exp),
                "setf.sig f%d=r2 ;;" % f,
                "setf.exp f%d=r3 ;;" % FP_TEMP,
                "fmerge.se f%d=f%d,f%d ;;" % (f, FP_TEMP, f),
            ]
        spill = fuzz.SCRATCH + 8 * rng.randrange(fuzz.NAT_AREA)
        return [
            "movl r3=0x%x ;;" % spill,
            "movl r2=0x%x ;;" % (1 << ((spill >> 3) & 63)),
            "mov ar.unat=r2 ;;",
            "ld8.fill r2=[r3] ;;",
            "setf.d f%d=r2 ;;" % f,
        ]

    def fpsr(self):
        rng, k = self.rng, self.rng.random()
        if k < 0.5:
            return FPSR_LINUX | FPSR_I
        if k < 0.65:
            return FPSR_LINUX
        if k < 0.75:
            return (FPSR_LINUX | FPSR_I) & ~(1 << rng.randrange(6))
        if k < 0.9:
            sf = 6 + 13 * rng.randrange(4)
            return (
                (FPSR_LINUX | FPSR_I) ^ rng.choice([1, 2, 3]) << (sf + 4) ^ rng.randrange(4) << sf
            )
        return rng.getrandbits(58) & ~(1 << 12)

    def fp_insn(self, qp):
        rng, sf = self.rng, self.sf()
        d, a, b, c = self.fr(True), self.fr(), self.fr(), self.fr()
        k = rng.random()
        if k < 0.32:
            op = rng.choice(["fma", "fms", "fnma"]) + rng.choice(["", "", ".s", ".d"])
            t = "%s%s f%d=f%d,f%d,f%d" % (op, sf, d, a, b, c)
        elif k < 0.42:
            op = rng.choice(["fmpy", "fadd", "fsub", "fnmpy"]) + rng.choice(["", ".s", ".d"])
            if rng.random() < 0.2:
                t = "fnorm%s%s f%d=f%d" % (rng.choice(["", ".s", ".d"]), sf, d, a)
            else:
                t = "%s%s f%d=f%d,f%d" % (op, sf, d, a, b)
        elif k < 0.54:
            r = rng.random()
            if r < 0.6:
                t = "%s%s f%d=f%d,f%d,f%d" % (rng.choice(["fpma", "fpms", "fpnma"]), sf, d, a, b, c)
            elif r < 0.7:
                t = "fpmpy%s f%d=f%d,f%d" % (sf, d, a, b)
            elif r < 0.85:
                t = "%s%s f%d=f%d,f%d" % (
                    rng.choice(["fpmin", "fpmax", "fpamin", "fpamax"]),
                    sf,
                    d,
                    a,
                    b,
                )
            else:
                rel = rng.choice(["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"])
                t = "fpcmp.%s%s f%d=f%d,f%d" % (rel, sf, d, a, b)
        elif k < 0.62:
            r = rng.random()
            p1, p2 = self.pred_pair()
            if r < 0.4:
                t = "%s%s f%d=f%d,f%d" % (
                    rng.choice(["fmin", "fmax", "famin", "famax"]),
                    sf,
                    d,
                    a,
                    b,
                )
            elif r < 0.8:
                rel = rng.choice(["eq", "lt", "le", "gt", "ge", "unord", "neq", "nlt", "ord"])
                t = "fcmp.%s%s%s p%d,p%d=f%d,f%d" % (
                    rel,
                    rng.choice(["", ".unc"]),
                    sf,
                    p1,
                    p2,
                    a,
                    b,
                )
            else:
                t = "fclass.%s%s p%d,p%d=f%d,0x%x" % (
                    rng.choice(["m", "nm"]),
                    rng.choice(["", ".unc"]),
                    p1,
                    p2,
                    a,
                    rng.randrange(1, 0x200),
                )
        elif k < 0.68:
            r = rng.random()
            if r < 0.5:
                t = "%s%s f%d=f%d" % (
                    rng.choice(["fcvt.fx", "fcvt.fxu", "fcvt.fx.trunc", "fcvt.fxu.trunc"]),
                    sf,
                    d,
                    a,
                )
            elif r < 0.75:
                t = "fcvt.xf f%d=f%d" % (d, a)
            else:
                t = "%s%s f%d=f%d" % (
                    rng.choice(["fpcvt.fx", "fpcvt.fxu", "fpcvt.fx.trunc"]),
                    sf,
                    d,
                    a,
                )
        elif k < 0.72:
            p = rng.randrange(1, 64)
            r = rng.random()
            if r < 0.35:
                t = "frcpa%s f%d,p%d=f%d,f%d" % (sf, d, p, a, b)
            elif r < 0.6:
                t = "frsqrta%s f%d,p%d=f%d" % (sf, d, p, a)
            elif r < 0.8:
                t = "fprcpa%s f%d,p%d=f%d,f%d" % (sf, d, p, a, b)
            else:
                t = "fprsqrta%s f%d,p%d=f%d" % (sf, d, p, a)
        elif k < 0.82:
            r = rng.random()
            if r < 0.8:
                op = rng.choice(
                    [
                        "fmerge.s",
                        "fmerge.ns",
                        "fmerge.se",
                        "fpmerge.s",
                        "fpmerge.ns",
                        "fpmerge.se",
                        "fpack",
                        "fmix.l",
                        "fmix.r",
                        "fmix.lr",
                        "fand",
                        "fandcm",
                        "for",
                        "fxor",
                        "fswap",
                        "fswap.nl",
                        "fswap.nr",
                        "fsxt.l",
                        "fsxt.r",
                    ]
                )
                t = "%s f%d=f%d,f%d" % (op, d, a, b)
            elif r < 0.9:
                t = "%s f%d=f%d" % (
                    rng.choice(["fpabs", "fpneg", "fpnegabs", "fabs", "fneg", "mov"]),
                    d,
                    a,
                )
            else:
                t = "fselect f%d=f%d,f%d,f%d" % (d, a, b, c)
        elif k < 0.92:
            fmt, size = rng.choice([("s", 4), ("d", 8), ("8", 8)])
            if rng.random() < 0.55:
                post = rng.random()
                post = (
                    ",%d" % rng.choice([size, 8, -8])
                    if post < 0.5
                    else ",r%d" % INC
                    if post < 0.6
                    else ""
                )
                t = "ldf%s f%d=[r%d]%s" % (fmt, d, LP, post)
            else:
                post = ",%d" % rng.choice([size, 8]) if rng.random() < 0.6 else ""
                t = "stf%s [r%d]=f%d%s" % (fmt, SP, a, post)
        elif k < 0.98:
            kind = rng.choice(["s", "d", "sig", "exp"])
            if rng.random() < 0.5:
                t = "setf.%s f%d=r%d" % (kind, d, self.src())
            else:
                t = "getf.%s r%d=f%d" % (kind, self.dest(qp), a)
        else:
            t = (
                "fsetc%s 0x%x,0x%x" % (sf, rng.randrange(128), rng.randrange(128))
                if rng.random() < 0.5
                else "fclrf%s" % sf
            )
        return [self.pfx(qp) + t]

    def back_edge(self):
        """A branch back to the header, taken at most CONT times in all."""
        p1, p2 = self.counter_pair()
        return [
            "cmp.ltu p%d,p%d=0,r%d ;;" % (p1, p2, CONT),
            "(p%d) adds r%d=-1,r%d ;;" % (p1, CONT, CONT),
            "(p%d) br.cond.dptk.few t1fuzz_loop ;;" % p1,
        ]

    def inner_loop(self, counter, nest):
        """A counted loop: a backward branch to its own label, at most 4
        iterations per arrival. Its body may hold a loop nested in it (with
        the other counter) or a back-edge to the header; inserted blocks
        never split one another, so no instruction lands between a counter's
        compare and its branch. Returns its lines, one list per unit: a
        label inserted between units (a second entry) skips no compare."""
        rng = self.rng
        p1, p2 = self.counter_pair()
        label = self.lbl("in")
        units = []
        for _ in range(rng.randrange(1, 4)):
            qp, r = self.qp(), rng.random()
            if r < 0.5:
                line = self.alu(qp)[0]
            elif r < 0.7:
                line = self.cmp(qp)[0]
            elif r < 0.85:
                line = self.helper(qp)[0]
            else:
                line = self.pfx(qp) + "ld8 r%d=[r%d]" % (self.dest(qp), LP)
            units.append([line + " ;;"])
        if nest and rng.random() < 0.25:
            units.insert(
                rng.randrange(len(units) + 1),
                [l for u in self.inner_loop(INNER2, False) for l in u],
            )
        if rng.random() < 0.15:
            units.insert(rng.randrange(len(units) + 1), self.back_edge())
        return (
            [["mov r%d=%d ;;" % (counter, rng.randrange(0, 4))], [label + ":"]]
            + units
            + [
                [
                    "cmp.ltu p%d,p%d=0,r%d ;;" % (p1, p2, counter),
                    "(p%d) adds r%d=-1,r%d ;;" % (p1, counter, counter),
                    "(p%d) br.cond.dptk.few %s ;;" % (p1, label),
                ]
            ]
        )

    # -- calls ---------------------------------------------
    def callee_op(self, regs):
        """One instruction of a callee: on its frame's registers and the
        static registers of the pool (never the pointers, counters or ar.lc;
        the caller's stacked names are other registers in its frame)."""
        rng = self.rng
        qp = self.qp()
        dests = regs + [r for r in self.dests if r < 32]
        srcs = regs + [r for r in self.srcs if r < 32]
        d, a, b = rng.choice(dests), rng.choice(srcs), rng.choice(srcs)
        k = rng.randrange(6)
        if k == 0:
            t = "%s r%d=r%d,r%d" % (rng.choice(["add", "sub", "and", "or", "xor"]), d, a, b)
        elif k == 1:
            t = "adds r%d=%d,r%d" % (d, rng.randrange(-8192, 8192), b)
        elif k == 2:
            t = "shladd r%d=r%d,%d,r%d" % (d, a, rng.randrange(1, 5), b)
        elif k == 3:
            p1, p2 = self.pred_pair()
            t = "cmp.%s p%d,p%d=r%d,r%d" % (rng.choice(CRELS), p1, p2, a, b)
        elif k == 4:
            t = "ld8 r%d=[r%d]" % (d, LP)
        else:
            t = "mov r%d=r%d" % (d, a)
        return [self.pfx(qp) + t + " ;;"]

    def check(self, regs):
        """chk.s on a register (an ld8.s result half the time: deferred,
        NaT, in physical mode) or, in physical mode, on an FR (an ldf8.s
        result half the time: NaTVal) with its recovery code, which clears
        it."""
        rng = self.rng
        rec, back = self.lbl("rcv"), self.lbl("bk")
        if not self.virtual and rng.random() < 0.3:
            f = rng.randrange(2, 32)
            lines = ["ldf8.s f%d=[r%d] ;;" % (f, LP)] if rng.random() < 0.5 else []
            lines += ["%schk.s f%d,%s ;;" % (self.pfx(self.qp()), f, rec), back + ":"]
            return lines, [rec + ":", "mov f%d=f0 ;;" % f, "br.sptk.few %s ;;" % back]
        r = rng.choice(regs + [r for r in self.dests if r < 32])
        lines = ["ld8.s r%d=[r%d] ;;" % (r, LP)] if rng.random() < 0.5 else []
        lines += [
            "%schk.s.%s r%d,%s ;;" % (self.pfx(self.qp()), rng.choice(["i", "m"]), r, rec),
            back + ":",
        ]
        return lines, [
            rec + ":",
            "mov r%d=%d ;;" % (r, rng.randrange(1, 1000)),
            "br.sptk.few %s ;;" % back,
        ]

    def sink_check(self, qp):
        """chk.s on a NaT sink in the loop body; the recovery code (placed
        with the functions) counts in a destination and returns."""
        rng = self.rng
        r, d = rng.choice(self.sinks), rng.choice(self.dests)
        rec, back = self.lbl("rcv"), self.lbl("bk")
        self.recoveries += [rec + ":", "adds r%d=1,r%d ;;" % (d, d), "br.sptk.few %s ;;" % back]
        return ["%schk.s.%s r%d,%s" % (self.pfx(qp), rng.choice(["i", "m"]), r, rec), back + ":"]

    def leaf(self, outs):
        """A function without a frame of its own: the caller's outputs."""
        label = self.lbl("leaf")
        regs = [32 + k for k in range(outs)]
        lines = [label + ":"]
        for _ in range(self.rng.randrange(1, 5)):
            lines += self.callee_op(regs)
        return label, lines + ["br.ret.sptk.many b0 ;;"]

    def function(self, inputs):
        """alloc (the inputs are the caller's outputs), b0 and ar.pfs saved,
        a body of callee ops, nested leaf calls and chk.s, b0 and ar.pfs
        restored, br.ret (sometimes a predicated one first)."""
        rng = self.rng
        i, l, o = inputs, rng.randrange(2, 8), rng.randrange(0, 4)
        rp, rb = 32 + i + l - 2, 32 + i + l - 1
        label = self.lbl("fn")
        regs = [r for r in range(32, 32 + i + l + o) if r not in (rp, rb)]
        lines = [
            label + ":",
            "alloc r%d=ar.pfs,%d,%d,%d,0 ;;" % (rp, i, l, o),
            "mov r%d=b0 ;;" % rb,
        ]
        lines += [
            "mov r%d=%d ;;" % (r, rng.randrange(-(1 << 21), 1 << 21)) for r in regs if r >= 32 + i
        ]
        tail = []
        for _ in range(rng.randrange(2, 9)):
            k = rng.random()
            if k < 0.15 and o:
                leaf, leaf_lines = self.leaf(o)
                tail += leaf_lines
                lines.append("%sbr.call.sptk.many b0=%s ;;" % (self.pfx(self.qp()), leaf))
            elif k < 0.25:
                check, recovery = self.check(regs)
                lines += check
                tail += recovery
            elif k < 0.3:
                lines.append("mov ar.ec=%d ;;" % rng.randrange(0, 64))
            elif k < 0.35:
                lines.append(
                    "mov r%d=ar.pfs ;;" % rng.choice(regs + [r for r in self.dests if r < 32])
                )
            else:
                lines += self.callee_op(regs)
        lines += ["mov b0=r%d ;;" % rb, "mov ar.pfs=r%d ;;" % rp]
        if rng.random() < 0.2:
            lines.append("(p%d) br.ret.sptk.many b0 ;;" % rng.randrange(1, 64))
            lines += self.callee_op(regs)
        return label, lines + ["br.ret.sptk.many b0 ;;"] + tail

    def call(self, qp):
        """A call of one of the program's functions: from any slot the
        assembler picks, from slot 1 of an MBB bundle, or a long call."""
        rng = self.rng
        target = rng.choice(self.funcs)
        r = rng.random()
        if r < 0.2:
            return [
                "{ .mbb\n nop.m 0\n %sbr.call.sptk.many b0=%s\n nop.b 0 ;; }"
                % (self.pfx(qp), target)
            ]
        if r < 0.3:
            return ["%sbrl.call.sptk.many b0=%s" % (self.pfx(qp), target)]
        return ["%sbr.call.sptk.many b0=%s" % (self.pfx(qp), target)]

    def body(self, n):
        rng = self.rng
        out, pending = [], []  # pending: [label, instructions left before it]
        for i in range(n):
            for p in [p for p in pending if p[1] <= 0]:
                out.append(p[0] + ":")
                pending.remove(p)
            qp = self.qp()
            if self.fp and rng.random() < 0.5:
                out += self.fp_insn(qp)
                for p in pending:
                    p[1] -= 1
                continue
            r = rng.random()
            if r < 0.30:
                out += self.alu(qp)
            elif r < 0.50:
                out += self.cmp(qp)
            elif r < 0.65:
                out += self.load(qp)
            elif r < 0.77:
                out += self.store(qp)
            elif r < 0.83 and i < n - 2:
                label = self.lbl("f")
                out.append("%sbr.cond.sptk.few %s" % (self.pfx(qp), label))
                pending.append([label, rng.randrange(1, 4)])
            elif r < 0.86:
                out.append("(p%d) br.cond.dpnt.few .Lt1exit" % rng.randrange(1, 64))
            elif r < 0.92:
                out += self.helper(qp)
            elif r < 0.94:
                out.append(self.pfx(qp) + "mf")
            elif r < 0.97 and self.funcs:
                out += self.call(qp)
            elif r < 0.985 and self.sinks:
                out += self.sink_check(qp)
            else:
                out += self.alu(qp)
            for p in pending:
                p[1] -= 1
        for p in pending:
            out.append(p[0] + ":")
        out = [l if l.endswith(":") or l.endswith("}") else l + " ;;" for l in out]
        # Second back-edges to the header and counted inner loops, one or
        # two; a loop is sometimes also entered in its body from a branch
        # before it (two entries). Inserted blocks never split one another.
        units = [[l] for l in out]
        for _ in range(rng.randrange(1, 3) if self.funcs else 0):
            units.insert(
                rng.randrange(len(units) + 1),
                [l if l.endswith("}") else l + " ;;" for l in self.call(self.qp())],
            )
        if rng.random() < 0.4:
            for _ in range(rng.randrange(1, 3)):
                units.insert(rng.randrange(len(units) + 1), self.back_edge())
        if rng.random() < 0.35:
            for _ in range(1 if rng.random() < 0.7 else 2):
                loop = self.inner_loop(INNER, True)
                if rng.random() < 0.1:
                    mid = self.lbl("mid")
                    loop.insert(rng.randrange(2, len(loop)), [mid + ":"])
                    loop.insert(0, ["(p%d) br.cond.sptk.few %s ;;" % (rng.randrange(1, 64), mid)])
                units.insert(rng.randrange(len(units) + 1), [l for u in loop for l in u])
        return [l for u in units for l in u]

    def frame_base(self, setup):
        """Moves the frame base before the setup's frame alloc: a frame of K locals, then br.call, gives BOF = K."""
        rng, sof = self.rng, self.g.frame.sof
        at = next(i for i, l in enumerate(setup) if l.startswith("alloc r2=ar.pfs,0,"))
        if sof > 1 and rng.random() < 0.4:
            k = rng.randrange(97 - sof, 96)  # BOF + sof > 96: the frame wraps
        else:
            k = rng.randrange(0, 96)
        if k:
            setup[at:at] = [
                "alloc r2=ar.pfs,0,%d,0,0 ;;" % k,
                "br.call.sptk.many b0=.Lt1bof ;;",
                ".Lt1bof:",
            ]
        return setup

    def program(self):
        rng, g = self.rng, self.g
        setup = self.frame_base(g.setup())
        self.setup_pools()
        # Functions the loop calls: before the loop on its
        # pages, or after the epilogue.
        self.funcs, funcs, self.recoveries = [], [], []
        if rng.random() < 0.5:
            for _ in range(rng.randrange(1, 4)):
                label, lines = self.function(len(g.frame.outs))
                self.funcs.append(label)
                funcs += lines
        inline = funcs and rng.random() < 0.6
        ivt = {v: kind for v, kind in fuzz.RESUMABLE.items() if rng.random() < 0.6}
        pre = []
        # Random rotation bases: br.ctop rotates GRs (sor > 0) and PRs.
        if rng.random() < 0.6:
            pre += [
                "movl r2=0x%x ;;" % rng.getrandbits(64),
                "mov pr=r2,-1 ;;",
                "mov ar.lc=%d ;;" % rng.randrange(0, 6),
                "mov ar.ec=%d ;;" % rng.randrange(0, 3),
                ".Lt1rot:",
                "br.ctop.sptk.few .Lt1rot ;;",
            ]
        for r in self.regs:
            if r >= 32 and r in g.frame.rot and rng.random() < 0.5:
                continue  # rotated values from the frame set-up
            pre.append("movl r%d=0x%x ;;" % (r, g.value()))
        for r in self.sinks:  # NaT through ld8.fill (fuzz.py's idiom)
            spill = fuzz.SCRATCH + 8 * rng.randrange(fuzz.NAT_AREA)
            pre += [
                "movl r3=0x%x ;;" % spill,
                "movl r2=0x%x ;;" % (1 << ((spill >> 3) & 63)),
                "mov ar.unat=r2 ;;",
                "ld8.fill r%d=[r3] ;;" % r,
            ]
        residue = rng.randrange(1, 8) if rng.random() < 0.08 else 0
        pre += [
            "movl r%d=0x%x ;;" % (LP, LOAD_BASE + 8 * rng.randrange(8, 40) + residue),
            "movl r%d=0x%x ;;"
            % (
                SP,
                STORE_BASE
                + 8 * rng.randrange(0, 16)
                + (rng.randrange(1, 8) if rng.random() < 0.04 else 0),
            ),
            "mov r%d=%d ;;" % (INC, rng.choice([8, 16, -8])),
            "mov r%d=%d ;;" % (CONT, rng.randrange(0, 13)),
            "mov r%d=0 ;;" % INNER,
            "mov r%d=0 ;;" % INNER2,
            "movl r2=0x%x ;;" % rng.getrandbits(64),
            "mov pr=r2,-1 ;;",
            "mov ar.lc=%d ;;" % rng.randrange(0, 12),
        ]
        if self.fp:
            self.frs = list(range(2, 32)) + rng.sample(range(32, FP_TEMP), 6)
            for f in self.frs:
                pre += self.fp_init(f)
            pre += ["movl r2=0x%x ;;" % self.fpsr(), "mov ar.fpsr=r2 ;;"]
        writers = []
        if self.mmu_writers and self.virtual:
            # Its own generator: the rest of the program is as without it.
            name = self.mmu_writers
            if name == "random":
                name = sorted(MMU_BLOCKS)[
                    random.Random(self.seed * 7919 + 17).randrange(len(MMU_BLOCKS))
                ]
            writers = MMU_BLOCKS[name]
        loop = (
            [".global t1fuzz_loop", "t1fuzz_loop:"]
            + self.body(rng.randrange(6, 28))
            + writers
            + ["{ .mib", "nop.m 0", "nop.i 0", "br.cloop.sptk.few t1fuzz_loop ;; }", ".Lt1exit:"]
        )
        blocks = funcs + self.recoveries
        if blocks and (inline or not funcs):
            # Half of the functions before the loop lie a page below it (its
            # own generator: the rest of the program is as without it).
            far = funcs and random.Random(self.seed * 104729 + 3).random() < 0.5
            pre += (
                ["br.sptk.few .Lt1fend ;;"]
                + blocks
                + ([".skip 4096"] if far else [])
                + [".Lt1fend:"]
            )
        body = [fuzz.Item(pre + loop, tag="t1")]
        if funcs and not inline:
            body.append(fuzz.Item([], None, [], tag="t1fn", sub=(blocks, [])))
        data = [g.value() for _ in range(fuzz.NWORDS)]
        return fuzz.Program(self.seed, [], body, data, setup, ivt)


def build(job):
    """Generates and assembles one program; returns (seed, image, loop address) or (seed, None, why)."""
    seed, work, virtual_share, mmu_writers, fp = job
    rng = random.Random(seed)
    virtual = rng.random() < virtual_share
    program = V1Gen(seed, virtual, mmu_writers, fp).program()
    tools = fuzz.Tools(
        {"CROSS_IA64": os.environ["CROSS_IA64"], "QEMU_IA64": "unused"}, "unused", work
    )
    name = "t1f%d" % seed
    try:
        src, elf, image, _ = tools.build(program, name)
    except fuzz.AsmError as e:
        return seed, None, "asm: %s" % str(e).split("\n")[0]
    nm = subprocess.run(
        [tools.cross + "nm", str(elf)], check=True, capture_output=True, text=True
    ).stdout
    m = re.search(r"^([0-9a-f]+) \w t1fuzz_loop$", nm, re.M)
    if not m:
        return seed, None, "no t1fuzz_loop symbol"
    for f in (elf, src.with_suffix(".o")):
        f.unlink()
    return seed, str(image), int(m.group(1), 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", required=True)
    ap.add_argument("--seeds", type=int, default=200)
    ap.add_argument("--seed0", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--virtual-share", type=float, default=0.25)
    ap.add_argument("--batch", type=int, default=50, help="programs per runner process")
    ap.add_argument("--keep", help="keep sources and images in this directory")
    ap.add_argument(
        "--mmu-writers",
        choices=["random"] + sorted(MMU_BLOCKS),
        help="end each virtual loop body with an MMU-log writer block",
    )
    ap.add_argument("--fp", action="store_true", help="FP-heavy loop bodies and FR/FPSR set-up")
    args = ap.parse_args()
    if not os.environ.get("CROSS_IA64"):
        sys.exit("t1fuzz.py: CROSS_IA64 is not set (environment or local.env via make)")
    work = args.keep or tempfile.mkdtemp(prefix="tier1-fuzz-")
    os.makedirs(work, exist_ok=True)
    seeds = list(range(args.seed0, args.seed0 + args.seeds))
    failed, asm_errors, ran, results = [], 0, 0, {}
    try:
        with ProcessPoolExecutor(max_workers=args.jobs) as pool:
            built = list(
                pool.map(
                    build,
                    [(s, work, args.virtual_share, args.mmu_writers, args.fp) for s in seeds],
                    chunksize=8,
                )
            )
        programs = []
        for seed, image, extra in built:
            if image is None:
                asm_errors += 1
                if asm_errors <= 3:
                    print("t1fuzz: seed %d not assembled (%s)" % (seed, extra))
                continue
            programs.append((seed, image, extra))
        batches = [programs[i : i + args.batch] for i in range(0, len(programs), args.batch)]

        def run(batch):
            lines = "".join("%s %x %d\n" % (image, loop, seed) for seed, image, loop in batch)
            r = subprocess.run(
                [os.path.abspath(args.runner)], input=lines, capture_output=True, text=True
            )
            return batch, r

        from concurrent.futures import ThreadPoolExecutor

        died = {}  # image -> stderr of a runner that stopped before reporting it
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for batch, r in pool.map(run, batches):
                if r.returncode:
                    for seed, image, loop in batch:
                        died[image] = r.stderr
                for line in r.stdout.splitlines():
                    m = re.match(r"^(\S+) (ok|FAIL|skip)\b(.*)$", line)
                    if m:
                        results[m.group(1)] = (m.group(2), m.group(3).strip())
                if r.returncode not in (0, 1):
                    for seed, image, loop in batch:
                        results.setdefault(
                            image,
                            (
                                "FAIL",
                                "runner exited %d: %s" % (r.returncode, r.stderr.strip()[-400:]),
                            ),
                        )
        counts = {"ok": 0, "FAIL": 0, "skip": 0}
        instructions = 0
        forced = {}
        bof = {"wraps": 0, "moved_wraps": 0, "reused": 0, "bofs": set()}
        checked = {"executions": 0, "programs": 0}
        nests = {"regions": 0, "programs": 0}
        calls = {"regions": 0, "programs": 0}
        callees = {"regions": 0, "programs": 0}
        spills = {"spills": 0, "programs": 0}
        chain = {"links": 0, "programs": 0, "kicks": 0, "forced": 0}
        for seed, image, loop in programs:
            status, detail = results.get(
                image,
                ("FAIL", "no result from the runner: %s" % died.get(image, "").strip()[-400:]),
            )
            counts[status] += 1
            ran += 1
            m = re.match(r"^(\d+) instructions", detail)
            if status == "ok" and m:
                instructions += int(m.group(1))
                kind = detail.split(", ")[-1]
                forced[kind] = forced.get(kind, 0) + 1
                c = re.search(r"checked lockstep \((\d+) Tier1 executions checked\)", detail)
                if c:
                    checked["executions"] += int(c.group(1))
                    checked["programs"] += int(c.group(1)) > 0
                n = re.search(r"(\d+) regions with inner loops", detail)
                if n:
                    nests["regions"] += int(n.group(1))
                    nests["programs"] += int(n.group(1)) > 0
                n = re.search(r"(\d+) regions with calls", detail)
                if n:
                    calls["regions"] += int(n.group(1))
                    calls["programs"] += int(n.group(1)) > 0
                n = re.search(r"(\d+) regions with callee pages, (\d+) in-region spills", detail)
                if n:
                    callees["regions"] += int(n.group(1))
                    callees["programs"] += int(n.group(1)) > 0
                    spills["spills"] += int(n.group(2))
                    spills["programs"] += int(n.group(2)) > 0
                h = re.search(
                    r"(\d+) chain links, kick at entry \d+( \(not reached\))?, product path ([^,]+)",
                    detail,
                )
                if h:
                    chain["links"] += int(h.group(1))
                    chain["programs"] += int(h.group(1)) > 0
                    chain["kicks"] += not h.group(2)
                    chain["forced"] += h.group(3).startswith("forced")
                b = re.search(
                    r"bof (\d+) sof (\d+)( wraps)?, moved to bof (\d+)( wraps)?(, region reused)?",
                    detail,
                )
                if b:
                    bof["bofs"].add(int(b.group(1)))
                    bof["wraps"] += bool(b.group(3))
                    bof["moved_wraps"] += bool(b.group(5))
                    bof["reused"] += bool(b.group(6))
            if status == "FAIL":
                failed.append((seed, image, detail))
        for seed, image, detail in failed[:10]:
            print("t1fuzz: seed %d FAILED: %s" % (seed, detail))
            keep = os.path.join(ROOT, "build", "tier1", "fuzz-failures")
            os.makedirs(keep, exist_ok=True)
            for suffix in (".S", ".bin"):
                path = image[:-4] + suffix
                if os.path.exists(path):
                    shutil.copy(path, keep)
        print(
            "t1fuzz: %d programs (%d not assembled), %d equal to the reference under every engine "
            "(%d instructions after the loop header in all), %d skipped (never reached the loop), "
            "%d failed"
            % (len(seeds), asm_errors, counts["ok"], instructions, counts["skip"], counts["FAIL"])
        )
        print(
            "t1fuzz: Tier1 forced cases: %s"
            % ", ".join("%s %d" % kv for kv in sorted(forced.items()))
        )
        print(
            "t1fuzz: checked lockstep: %d Tier1 executions checked in %d programs, 0 divergences "
            "(a divergence fails its program)" % (checked["executions"], checked["programs"])
        )
        print(
            "t1fuzz: loop forest: %d programs ended with regions holding inner loops (%d regions)"
            % (nests["programs"], nests["regions"])
        )
        print(
            "t1fuzz: calls: %d programs ended with regions holding calls or returns (%d regions), %d with "
            "regions holding callee pages on other code pages (%d regions)"
            % (calls["programs"], calls["regions"], callees["programs"], callees["regions"])
        )
        print(
            "t1fuzz: register stack: %d allocs spilled inside regions in %d programs"
            % (spills["spills"], spills["programs"])
        )
        print(
            "t1fuzz: product path (tier0+tier1): %d regions entered at chain points in %d programs, %d runs "
            "with a kick at a Tier1 entry, %d forced cases"
            % (chain["links"], chain["programs"], chain["kicks"], chain["forced"])
        )
        print(
            "t1fuzz: frame base at the loop header: %d distinct BOFs, %d frames wrapping the 96-register ring; "
            "rerun at a moved BOF: %d wrapping, %d with the header region of the first BOF reused"
            % (len(bof["bofs"]), bof["wraps"], bof["moved_wraps"], bof["reused"])
        )
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    return 1 if failed or (asm_errors > len(seeds) // 20) else 0


if __name__ == "__main__":
    sys.exit(main())
