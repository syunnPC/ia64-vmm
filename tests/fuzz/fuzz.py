#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare generated IA-64 programs with QEMU at bundle boundaries.

Checks registers, faults and scratch memory; minimizes divergences into
tests/fuzz/repro/. See docs/testing.md.
"""

import argparse
import os
import random
import re
import select
import subprocess
import sys
import threading
import time
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENTRY = 0x100000
SCRATCH = 0x110000  # .data: 64 qwords (512-byte aligned, so ld8.fill/st8.spill
NWORDS = 64  # reach every UNAT bit), streamed through r1-r30 by the epilogue
NAT_AREA = 56  # words 56-63: st8.spill of possibly-NaT GRs only, never read
# (a spilled NaT's memory image is implementation-specific,
# SDM 2.3 Vol.1 §4.4.4.6)
IVA = 0x118000  # .ivt: 32 KiB-aligned interruption vector table (both sides)
BSTORE = 0x120000  # RSE backing store (zeroed RAM on both sides)
BS_WORDS = 160  # backing-store words compared by the epilogue (max, below BSP)
BS_BASE_PTR = 0x110200  # RAM word after .data: latest backing-store base (0 = BSTORE)
END_IMM = 0x1FFFFE
MASK64 = (1 << 64) - 1

# --------------------------------------------------------------------------
# Environment


def load_env(defaults=None):
    """CROSS_IA64 / QEMU_IA64 from the environment, else local.env, else defaults."""
    file_env = {}
    path = REPO / "local.env"
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            m = re.match(r"\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$", line)
            if m:
                value = m.group(2).strip()
                if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
                    value = value[1:-1]
                file_env[m.group(1)] = value
    out = {}
    for key in ("CROSS_IA64", "QEMU_IA64"):
        value = os.environ.get(key) or file_env.get(key) or (defaults or {}).get(key)
        if not value:
            sys.exit(f"fuzz: {key} is not set (environment or local.env)")
        out[key] = value
    return out


# --------------------------------------------------------------------------
# Program model.  An Item renders to assembly lines; compound items (forward
# skips and loops) own an inner item list so ddmin can shrink both levels.
# "{L}" in a line is replaced with the item's unique label.


class Item:
    _next = 0
    _lock = threading.Lock()

    def __init__(self, head, inner=None, tail=None, tag="", fixed=False, sub=None):
        with Item._lock:
            Item._next += 1
            self.uid = Item._next
        self.head = head
        self.inner = inner
        self.tail = tail or []
        self.tag = tag  # category, for statistics
        self.fixed = fixed  # never removed by ddmin
        # Call items: (prefix, suffix) lines of the subroutine whose body is
        # `inner`; the subroutine is emitted after the epilogue.
        self.sub = sub

    def clone(self, inner=None):
        it = Item.__new__(Item)
        it.uid, it.head, it.tail, it.tag, it.fixed = (
            self.uid,
            self.head,
            self.tail,
            self.tag,
            self.fixed,
        )
        it.sub = self.sub
        it.inner = self.inner if inner is None else inner
        return it

    def render(self, out, subs):
        label = f".Lf{self.uid}"
        for line in self.head:
            out.append(line.replace("{L}", label))
        if self.sub is not None:
            body = [line.replace("{L}", label) for line in self.sub[0]]
            for child in self.inner or []:
                child.render(body, subs)
            body += [line.replace("{L}", label) for line in self.sub[1]]
            subs.extend(body)
        else:
            for child in self.inner or []:
                child.render(out, subs)
        for line in self.tail:
            out.append(line.replace("{L}", label))


IVT_VECTORS = [i * 0x400 for i in range(20)] + [0x5000 + i * 0x100 for i in range(48)]


class Program:
    def __init__(self, seed, prologue, body, data=None, setup=None, ivt=None, epilogue=None):
        self.seed = seed
        self.prologue = prologue
        self.body = body
        self.data = data or [0] * NWORDS
        self.setup = setup or []  # fixed preamble (IVA, PSR.ic, RSE, main frame)
        self.ivt = ivt or {}  # vector -> "skip" | "rfi" (others terminate)
        self.epilogue = epilogue or EPILOGUE

    def with_lists(self, prologue, body):
        return Program(self.seed, prologue, body, self.data, self.setup, self.ivt, self.epilogue)

    def text(self):
        lines = [
            f"// ia64 differential fuzz program, seed {self.seed}",
            ".text",
            ".global _start",
            "_start:",
        ]
        out, subs = [], []
        for item in self.prologue + self.body:
            item.render(out, subs)
        indent = lambda ls: ["    " + l if not l.endswith(":") else l for l in ls]
        lines += indent(self.setup) + indent(out)
        lines.append("fuzz_checkpoint:")
        lines += indent(self.epilogue)
        lines.append("fuzz_end:")
        lines.append(f"    break.m 0x{END_IMM:x} ;;")
        lines += indent(subs)
        if self.setup:
            lines.append('.section .ivt,"ax"')
            for v in IVT_VECTORS:
                lines.append(f".org 0x{v:x}")
                lines += indent(NOP_BUNDLE + IVT_STUBS[self.ivt.get(v, "end")])
        lines.append(".data")
        lines.append("fuzz_scratch:  // linked at SCRATCH")
        for i in range(0, NWORDS, 4):
            lines.append("    data8 " + ", ".join(f"0x{v:x}" for v in self.data[i : i + 4]))
        return "\n".join(lines) + "\n"

    def count(self):
        def n(items):
            return sum(1 + n(i.inner or []) for i in items)

        return n(self.body)


# Every bundle entry is compared, so streaming memory through registers
# compares it.  The epilogue flushes the register stack (deterministic in
# enforced-lazy mode), reads BSP/BSPSTORE and the defined RNAT bits
# (RNAT{RNATBitIndex:0}, SDM 2.3 Vol.2 §6.5.2), then streams the scratch
# window and the backing store [BSTORE, BSP).  Words above BSP are stale
# spill copies and are not compared.
def rnat_read(dst, mask):
    """dst = AR.RNAT & mask in one bundle (M;MI): only RNAT{RNATBitIndex:0}
    is defined (SDM 2.3 Vol.2 §6.5.2), so the raw value must never be
    visible at a (compared) bundle entry."""
    return [
        ".align 16",
        "{ .mmi",
        f"mov r{dst}=ar.rnat ;;",
        f"and r{dst}=r{dst},r{mask}",
        "nop.i 0 ;; }",
    ]


def rnat_write_ones():
    """RNAT := all ones.  Used where RNAT becomes undefined: every stored but
    uncollected word then reads as NaT-spilled and is never compared (its
    memory image may be an implementation-specific NaT spill)."""
    return ["movl r2=-1 ;;", "mov ar.rnat=r2 ;;"]


def rnat_mask(mask, idx, bspstore):
    """mask = (1 << BSPSTORE{8:3}) - 1: the RNAT bits deposited by RSE stores
    since the last collection store.  SDM 2.3 Vol.2 §6.5.2 calls
    RNAT{RNATBitIndex:0} defined but also makes the whole of RNAT undefined
    after the collection itself is spilled, so bit RNATBitIndex (no register
    stored there yet) is not relied upon."""
    return [
        f"extr.u r{idx}=r{bspstore},3,6 ;;",
        f"mov r{mask}=1 ;;",
        f"shl r{mask}=r{mask},r{idx} ;;",
        f"adds r{mask}=-1,r{mask} ;;",
    ]


def masked_bs_load(dst, addr, bspstore, rnat, t):
    """Load the backing-store word at `addr` into `dst` unless its NaT bit
    (from its collection word, or RNAT if that is not stored yet) is set:
    a spilled NaT's memory image is implementation-specific (SDM 2.3 Vol.1
    §4.4.4.6, Vol.2 §6.4).  t: three scratch GRs. The raw value never
    reaches a GR, so per-bundle state stays deterministic."""
    a, b, c = t
    return [
        f"dep r{a}=-1,r{addr},3,6",
        f"extr.u r{b}=r{addr},3,6 ;;",
        f"cmp.ltu p7,p8=r{a},r{bspstore} ;;",
        f"(p7) ld8 r{c}=[r{a}]",
        f"(p8) mov r{c}=r{rnat} ;;",
        f"shr.u r{c}=r{c},r{b} ;;",
        f"tbit.nz p9,p10=r{c},0 ;;",
        f"(p9) mov r{dst}=0 ;;",
        f"(p10) ld8 r{dst}=[r{addr}] ;;",
    ]


# Every bundle entry is compared, so streaming memory through registers
# compares it.  The epilogue flushes the register stack (deterministic in
# enforced-lazy mode), reads BSP/BSPSTORE and the defined RNAT bits
# (RNAT{RNATBitIndex:0}, SDM 2.3 Vol.2 §6.5.2), then streams the scratch
# window (minus the NaT-spill area) and the backing store [BSTORE, BSP).
# Words above BSP are stale spill copies and are not compared.
def epilogue(order=()):
    """order: lines giving PSR.be the RSE byte order (--big-endian), so that
    collection words and BS_BASE_PTR read as the generator wrote them."""
    return ["flushrs ;;"] + list(order) + EPILOGUE_REST


EPILOGUE_REST = (
    [
        "mov r1=ar.bsp ;;",
        "mov r2=ar.bspstore ;;",
    ]
    + rnat_mask(5, 4, 2)
    + rnat_read(9, 5)
    + [
        f"movl r31=0x{SCRATCH:x} ;;",
    ]
    + [f"ld8 r{10 + i % 14}=[r31],8 ;;" for i in range(NAT_AREA)]
    + [
        # From the latest backing-store base (a relocation drops the partial
        # NaT collection, so older words have unknown NaT-ness), and at most
        # BS_WORDS words below BSP.
        f"movl r29=0x{BS_BASE_PTR:x} ;;",
        "ld8 r29=[r29] ;;",
        "cmp.eq p6,p0=0,r29 ;;",
        f"(p6) movl r29=0x{BSTORE:x} ;;",
        f"movl r28=0x{BS_WORDS * 8:x} ;;",
        "sub r28=r1,r28 ;;",
        "cmp.ltu p6,p0=r29,r28 ;;",
        "(p6) mov r29=r28 ;;",
        "sub r28=r1,r29 ;;",
        "shr.u r28=r28,3 ;;",
        "cmp.eq p6,p0=0,r28 ;;",
        "(p6) br.cond.sptk.few .Lepi_done ;;",
        "adds r28=-1,r28 ;;",
        "mov ar.lc=r28 ;;",
        ".Lepi_loop:",
    ]
    + masked_bs_load(27, 29, 2, 9, (26, 25, 24))
    + [
        "adds r29=8,r29",
        "br.cloop.sptk.few .Lepi_loop ;;",
        ".Lepi_done:",
    ]
)
EPILOGUE = epilogue()
# Every IVT entry starts with a nop-only bundle (the runner observes the
# vector-entry state after the first slot).  "end" stops via the terminal
# break; "skip" resumes after the faulting instruction (bank-0 r16-r22 only,
# branch-free, no predicates); "rfi" resumes at IIP/IPSR.ri (traps).
NOP_BUNDLE = ["{ .mii", "nop.m 0", "nop.i 0", "nop.i 0 ;; }"]
IVT_STUBS = {
    "end": [f"break.m 0x{END_IMM:x} ;;"],
    "skip": [
        "mov r16=cr.ipsr ;;",
        "mov r17=cr.iip ;;",
        "extr.u r18=r16,41,2 ;;",
        "adds r19=1,r18 ;;",
        "shr.u r20=r19,1 ;;",
        "and r20=r20,r19 ;;",
        "shl r21=r20,4 ;;",
        "add r17=r17,r21 ;;",
        "shladd r22=r20,1,r20 ;;",
        "sub r19=r19,r22 ;;",
        "dep r16=r19,r16,41,2 ;;",
        "mov cr.ipsr=r16 ;;",
        "mov cr.iip=r17 ;;",
        "srlz.d ;;",
        "rfi ;;",
    ],
    "rfi": ["srlz.d ;;", "rfi ;;"],
}
RESUMABLE = {
    0x2C00: "skip",
    0x5600: "skip",
    0x5400: "skip",
    0x5A00: "skip",
    0x5500: "skip",
    0x5C00: "skip",
    0x5D00: "rfi",
}


class Frame:
    """Current register frame as the generator models it."""

    def __init__(self, sof=0, sol=0, sor=0, reserved=()):
        self.sof, self.sol, self.sor = sof, sol, sor
        self.reserved = set(reserved)

    @property
    def regs(self):
        return [32 + k for k in range(self.sof) if 32 + k not in self.reserved]

    @property
    def outs(self):
        return list(range(32 + self.sol, 32 + self.sof))

    @property
    def rot(self):
        return set(range(32, 32 + 8 * self.sor))


# --------------------------------------------------------------------------
# Generator

INTERESTING = [
    0,
    1,
    2,
    3,
    7,
    8,
    15,
    16,
    31,
    32,
    63,
    64,
    0x7F,
    0x80,
    0xFF,
    0x100,
    0x7FFF,
    0x8000,
    0xFFFF,
    0x7FFFFFFF,
    0x80000000,
    0xFFFFFFFF,
    0x100000000,
    MASK64,
    MASK64 - 1,
    1 << 63,
    (1 << 63) - 1,
    (1 << 63) + 1,
    0x8080808080808080,
    0x7F7F7F7F7F7F7F7F,
    0x0101010101010101,
    0x8000800080008000,
    0x7FFF7FFF7FFF7FFF,
    0xFFFF0000FFFF0000,
    0x00FF00FF00FF00FF,
    0xFF00FF00FF00FF00,
    0x8000000080000000,
    0x7FFFFFFF7FFFFFFF,
    0x0123456789ABCDEF,
    0xFEDCBA9876543210,
    0x00000000FFFF8000,
    0xFFFFFFFF80000000,
    0x5555555555555555,
    0xAAAAAAAAAAAAAAAA,
]

ALU3 = (
    "add sub and or xor andcm addp4 "
    "padd1 padd1.sss padd1.uus padd1.uuu padd2 padd2.sss padd2.uus padd2.uuu padd4 "
    "psub1 psub1.sss psub1.uus psub1.uuu psub2 psub2.sss psub2.uus psub2.uuu psub4 "
    "pavg1 pavg1.raz pavg2 pavg2.raz pavgsub1 pavgsub2 "
    "pcmp1.eq pcmp1.gt pcmp2.eq pcmp2.gt pcmp4.eq pcmp4.gt "
    "pmin1.u pmax1.u pmin2 pmax2 pmpy2.l pmpy2.r psad1 "
    "mix1.l mix1.r mix2.l mix2.r mix4.l mix4.r pack2.sss pack2.uss pack4.sss "
    "unpack1.h unpack1.l unpack2.h unpack2.l unpack4.h unpack4.l "
    "shl shr shr.u pshl2 pshl4 pshr2 pshr2.u pshr4 pshr4.u"
).split()
ALU1 = "zxt1 zxt2 zxt4 sxt1 sxt2 sxt4 popcnt czx1.l czx1.r czx2.l czx2.r".split()
# Optional Itanium 9300+ integer ops (CPUID[4].x2 / .cz).  The fork's
# itanium2 model does not advertise them and raises Illegal Operation;
# enabled only with --feature-ops.
FEATURE_ALU3 = ["mpy4", "mpyshl4"]
FEATURE_ALU1 = ["clz"]
MUX1 = "@brcst @mix @shuf @alt @rev".split()
CRELS = "eq ne lt le gt ge ltu leu gtu geu".split()
PARALLEL = "and or or.andcm orcm andcm and.orcm".split()
CTYPES = ["", "unc"] + PARALLEL
SIZES = [1, 2, 4, 8]


class Gen:
    def __init__(
        self, seed, length, feature_ops=False, known=False, virtual=False, big_endian=False
    ):
        self.alu3 = ALU3 + (FEATURE_ALU3 if feature_ops else [])
        self.alu1 = ALU1 + (FEATURE_ALU1 if feature_ops else [])
        self.rng = random.Random(seed)
        self.seed = seed
        self.length = length
        self.taint = set()  # GRs whose NaT bit may be set
        self.frame = Frame()  # current register frame (stacked GRs)
        self.depth = 0  # call depth (0 = main program)
        self.bank = 1  # PSR.bn the program runs in
        self.bank_taint = set()  # taint of r16-r31 in the other bank
        self.resume = set()  # IVT vectors whose stub resumes
        self.covered = 0  # stacked registers below BSP at depth 0 (covered frames)
        # Extra exception-boundary cases require a current QEMU fork.
        self.known = known
        self.ftaint = set()  # FRs that may hold NaTVal
        self.fp_arith = False  # --fp-arith: rounding FP arithmetic too
        self.virtual = virtual
        # --big-endian: random UM.be and DCR.be; RSC.be fixed per program, as
        # backing-store collection words are read back in one byte order.
        self.big_endian = big_endian
        self.rsc_be = self.rng.getrandbits(1) << 4 if big_endian else 0

    # -- helpers -----------------------------------------------------------
    def value(self):
        r = self.rng.random()
        if r < 0.45:
            v = self.rng.choice(INTERESTING)
        elif r < 0.6:
            v = self.rng.randrange(-64, 64) & MASK64
        elif r < 0.7:
            v = self.rng.choice(INTERESTING) ^ (1 << self.rng.randrange(64))
        else:
            v = self.rng.getrandbits(64)
        return v & MASK64

    def pool(self):
        """Writable GRs: static r1-r31 and the current frame's stacked
        registers (minus a subroutine's saved PFS/b0 locals)."""
        stacked = self.frame.regs
        if stacked and self.rng.random() < 0.4:
            return stacked
        return list(range(1, 32))

    def gr(self, allow0=False):
        if allow0 and self.rng.random() < 0.08:
            return 0
        return self.rng.choice(self.pool())

    def clean_gr(self):
        choices = [r for r in self.pool() if r not in self.taint]
        if self.rng.random() < 0.05 or not choices:
            return 0
        return self.rng.choice(choices)

    def dest(self):
        return self.rng.choice(self.pool())

    def bnum(self):
        """A branch register the program may write (b0 holds a subroutine's
        return link only while it runs; it is saved/restored around calls)."""
        return self.rng.randrange(1 if self.depth else 0, 8)

    def qp(self, loop=None):
        r = self.rng.random()
        if r < 0.5:
            return 0
        if loop == "ctop" and r < 0.75:
            return self.rng.choice([16, 17, 18, 19, 20, 62, 63])
        return self.rng.randrange(1, 64)

    @staticmethod
    def pfx(qp):
        return f"(p{qp}) " if qp else ""

    def order(self):
        """--big-endian: PSR.be := RSC.be before generator bookkeeping that
        reads or writes memory (collection words, BS_BASE_PTR)."""
        if not self.big_endian:
            return []
        return ["sum 0x2 ;;" if self.rsc_be else "rum 0x2 ;;"]

    def pred_pair(self):
        p1 = self.rng.randrange(0, 64)
        p2 = self.rng.randrange(0, 64)
        while p2 == p1:
            p2 = self.rng.randrange(0, 64)
        return p1, p2

    def write_gr(self, d, qp, tainted):
        if tainted:
            self.taint.add(d)
        elif not qp:
            self.taint.discard(d)

    def src(self, allow0=True):
        return self.gr(allow0)

    # -- instruction families (return (lines, tag) or None) ---------------
    def i_alu3(self, qp):
        op = self.rng.choice(self.alu3)
        d, a, b = self.dest(), self.src(), self.src()
        if op in ("add", "sub") and self.rng.random() < 0.25:
            text = f"{op} r{d}=r{a},r{b},1"
        else:
            text = f"{op} r{d}=r{a},r{b}"
        self.write_gr(d, qp, a in self.taint or b in self.taint or (qp and d in self.taint))
        return [self.pfx(qp) + text], "alu"

    def i_alu_imm(self, qp):
        rng = self.rng
        d, a, b = self.dest(), self.src(), self.src()
        k = rng.randrange(14)
        srcs = [b]
        if k == 0:
            text = f"adds r{d}={rng.randrange(-8192, 8192)},r{b}"
        elif k == 1:
            b = rng.randrange(0, 4)
            srcs = [b]
            text = f"addl r{d}={rng.randrange(-(1 << 21), 1 << 21)},r{b}"
        elif k == 2:
            text = f"addp4 r{d}={rng.randrange(-8192, 8192)},r{b}"
        elif k == 3:
            op = rng.choice(["sub", "and", "or", "xor", "andcm"])
            text = f"{op} r{d}={rng.randrange(-128, 128)},r{b}"
        elif k == 4:
            op = rng.choice(["shladd", "shladdp4"])
            text = f"{op} r{d}=r{a},{rng.randrange(1, 5)},r{b}"
            srcs = [a, b]
        elif k == 5:
            op = rng.choice(["shl", "shr", "shr.u"])
            text = f"{op} r{d}=r{b},{rng.randrange(64)}"
        elif k == 6:
            text = f"shrp r{d}=r{a},r{b},{rng.randrange(64)}"
            srcs = [a, b]
        elif k == 7:
            op = rng.choice(["extr", "extr.u"])
            text = f"{op} r{d}=r{b},{rng.randrange(64)},{rng.randrange(1, 65)}"
        elif k == 8:
            form = rng.randrange(4)
            pos = rng.randrange(64)
            if form == 0:
                text = f"dep r{d}=r{a},r{b},{pos},{rng.randrange(1, 17)}"
                srcs = [a, b]
            elif form == 1:
                text = f"dep r{d}={rng.choice([0, -1])},r{b},{pos},{rng.randrange(1, 65)}"
            elif form == 2:
                text = f"dep.z r{d}=r{a},{pos},{rng.randrange(1, 65)}"
                srcs = [a]
            else:
                text = f"dep.z r{d}={rng.randrange(-128, 128)},{pos},{rng.randrange(1, 65)}"
                srcs = []
        elif k == 9:
            text = f"{rng.choice(self.alu1)} r{d}=r{b}"
        elif k == 10:
            if rng.random() < 0.5:
                text = f"mux1 r{d}=r{a},{rng.choice(MUX1)}"
            else:
                text = f"mux2 r{d}=r{a},0x{rng.randrange(256):x}"
            srcs = [a]
        elif k == 11:
            op = rng.choice(["pmpyshr2", "pmpyshr2.u"])
            text = f"{op} r{d}=r{a},r{b},{rng.choice([0, 7, 15, 16])}"
            srcs = [a, b]
        elif k == 12:
            op = rng.choice(["pshl2", "pshl4", "pshr2", "pshr2.u", "pshr4", "pshr4.u"])
            width = 16 if "2" in op else 32
            cnt = rng.randrange(width) if rng.random() < 0.85 else width - 1
            text = f"{op} r{d}=r{b},{cnt}"
        else:
            op = rng.choice(["pshladd2", "pshradd2"])
            text = f"{op} r{d}=r{a},{rng.randrange(1, 4)},r{b}"
            srcs = [a, b]
        self.write_gr(d, qp, any(s in self.taint for s in srcs) or (qp and d in self.taint))
        return [self.pfx(qp) + text], "alu"

    def i_cmp(self, qp, loop=None):
        rng = self.rng
        p1, p2 = self.pred_pair()
        if loop == "while":
            # Never write rotating predicates in a while loop (termination).
            p1, p2 = rng.randrange(0, 16), rng.randrange(0, 16)
            while p2 == p1:
                p2 = rng.randrange(0, 16)
        kind = rng.randrange(10)
        ctype = rng.choice(CTYPES)
        sfx = f".{ctype}" if ctype else ""
        if kind < 6:
            op = rng.choice(["cmp", "cmp4"])
            if ctype in PARALLEL:
                if rng.random() < 0.6:
                    rel = rng.choice(["eq", "ne"])
                    a, b = self.src(), self.src()
                    if rng.random() < 0.3:
                        text = f"{op}.{rel}{sfx} p{p1},p{p2}={rng.randrange(-127, 127)},r{b}"
                    else:
                        text = f"{op}.{rel}{sfx} p{p1},p{p2}=r{a},r{b}"
                else:
                    rel = rng.choice(["lt", "le", "gt", "ge"])
                    text = f"{op}.{rel}{sfx} p{p1},p{p2}=r0,r{self.src()}"
            else:
                rel = rng.choice(CRELS)
                if rng.random() < 0.3:
                    imm = rng.randrange(-127, 127)
                    if rel in ("leu", "gtu") and imm == 0:  # gas: imm-1 must fit
                        imm = 1
                    text = f"{op}.{rel}{sfx} p{p1},p{p2}={imm},r{self.src()}"
                else:
                    text = f"{op}.{rel}{sfx} p{p1},p{p2}=r{self.src()},r{self.src()}"
        elif kind < 8:
            text = (
                f"tbit.{rng.choice(['z', 'nz'])}{sfx} p{p1},p{p2}=r{self.src()},{rng.randrange(64)}"
            )
        else:
            # tnat: bias towards registers that may hold NaT
            pool = list(self.taint) if self.taint and rng.random() < 0.6 else list(range(32))
            text = f"tnat.{rng.choice(['z', 'nz'])}{sfx} p{p1},p{p2}=r{rng.choice(pool)}"
        return [self.pfx(qp) + text], "cmp"

    def i_mov(self, qp, loop=None):
        rng = self.rng
        k = rng.randrange(9)
        d = self.dest()
        if k == 0:
            self.write_gr(d, qp, False)
            return [self.pfx(qp) + f"movl r{d}=0x{self.value():x}"], "movl"
        if k == 1:
            self.write_gr(d, qp, False)
            return [self.pfx(qp) + f"mov r{d}={rng.randrange(-(1 << 21), 1 << 21)}"], "movl"
        if k == 2:
            a = self.src()
            self.write_gr(d, qp, a in self.taint or (qp and d in self.taint))
            return [self.pfx(qp) + f"mov r{d}=r{a}"], "mov"
        if k == 3:
            self.write_gr(d, qp, qp and d in self.taint)
            return [self.pfx(qp) + f"mov r{d}=ip"], "mov"
        if k == 4:
            self.write_gr(d, qp, qp and d in self.taint)
            return [self.pfx(qp) + f"mov r{d}=pr"], "pr"
        if k == 5 and loop != "while":
            mask = rng.getrandbits(17) | rng.choice([0, 0x10000])
            return [self.pfx(qp) + f"mov pr=r{self.clean_gr()},0x{mask:x}"], "pr"
        if k == 6 and loop != "while":
            imm = (rng.getrandbits(28) - (1 << 27)) << 16
            return [self.pfx(qp) + f"mov pr.rot={imm}"], "pr"
        if k == 7:
            return [self.pfx(qp) + f"mov b{self.bnum()}=r{self.clean_gr()}"], "br"
        self.write_gr(d, qp, qp and d in self.taint)
        return [self.pfx(qp) + f"mov r{d}=b{rng.randrange(8)}"], "br"

    def fpsr_value(self):
        rng = self.rng
        while True:
            v = rng.getrandbits(58)
            if rng.random() < 0.5:
                v = 0x0009804C0270033F ^ (
                    rng.getrandbits(58) & rng.getrandbits(58) & rng.getrandbits(58)
                )
            if (v >> 12) & 1:
                continue
            if any(((v >> s) & 3) == 1 for s in (8, 21, 34, 47)):
                continue
            return v

    def pfs_value(self):
        rng = self.rng
        # Keep generated frames valid for subsequent br.ret instructions.
        sof = rng.randrange(0, 97)
        sol = rng.randrange(0, sof + 1)
        sor = rng.randrange(0, sof // 8 + 1)
        rrb_gr = rng.randrange(0, sor * 8) if sor else 0
        pfm = (
            sof
            | sol << 7
            | sor << 14
            | rrb_gr << 18
            | rng.randrange(96) << 25
            | rng.randrange(48) << 32
        )
        rv = 0
        if rng.random() < 0.05:  # reserved field: must fault on both
            rv = 1 << rng.choice(list(range(38, 52)) + list(range(58, 62)))
        return pfm | rng.getrandbits(6) << 52 | rng.getrandbits(2) << 62 | rv

    def i_ar(self, qp, loop=None):
        rng = self.rng
        k = rng.randrange(12)
        d = self.dest()
        lines = []
        if k <= 3:
            ar = rng.choice(
                ["ccv", "unat", "k0", "k1", "k5", "k7"] + ([] if loop else ["lc", "ec"])
            )
            if rng.random() < 0.3 and ar not in ("k0", "k1", "k5", "k7"):
                lines.append(self.pfx(qp) + f"mov ar.{ar}={rng.randrange(-128, 128)}")
            else:
                lines.append(self.pfx(qp) + f"mov ar.{ar}=r{self.clean_gr()}")
            return lines, "ar"
        if k <= 6:
            # RSC.mode stays 0 (enforced lazy), so BSPSTORE/RNAT are readable
            # and the RSE only performs mandatory spills/fills.
            ar = rng.choice(
                [
                    "ccv",
                    "unat",
                    "lc",
                    "ec",
                    "pfs",
                    "rsc",
                    "bsp",
                    "fpsr",
                    "k0",
                    "k3",
                    "k6",
                    "ar48",
                    "ar112",
                    "bspstore",
                    "csd",
                ]
            )
            name = ar if ar.startswith("ar") and ar[2:].isdigit() else f"ar.{ar}"
            self.write_gr(d, qp, qp and d in self.taint)
            return [self.pfx(qp) + f"mov r{d}={name}"], "ar"
        t = rng.randrange(1, 32)
        self.taint.discard(t)
        if k == 8 and self.depth:  # a subroutine's PFS is its return frame
            k = 7
        if k == 10 and self.depth:  # moving the backing store under live callers
            k = 9
        if k == 7:
            lines = [f"movl r{t}=0x{self.fpsr_value():x} ;;", self.pfx(qp) + f"mov ar.fpsr=r{t}"]
        elif k == 8:
            lines = [f"movl r{t}=0x{self.pfs_value():x} ;;", self.pfx(qp) + f"mov ar.pfs=r{t}"]
        elif k == 9:
            # mode 0 only; pl random (raised to CPL), be the program's RSC.be;
            # loadrs field random (only loadrs reads it).
            v = (rng.getrandbits(2) << 2) | self.rsc_be | rng.getrandbits(14) << 16
            lines = [f"movl r{t}=0x{v:x} ;;", self.pfx(qp) + f"mov ar.rsc=r{t}"]
        elif k == 10:
            # Move the backing store inside its region (depth 0 only).
            # RNAT becomes undefined: rewrite it all-ones (rnat_write_ones).
            v = BSTORE + 8 * rng.randrange(0, 0x800) + rng.randrange(8)
            u, m = rng.sample([x for x in range(3, 32) if x != t], 2)
            self.taint -= {u, m, 2}
            mask = (1 << ((v >> 3) & 63)) - 1
            lines = (
                [
                    f"movl r{t}=0x{v & ~7:x} ;;",
                    f"mov ar.bspstore=r{t} ;;",
                    f"movl r{u}=0x{BS_BASE_PTR:x} ;;",
                ]
                + self.order()
                + [f"st8 [r{u}]=r{t} ;;"]
                + rnat_write_ones()
                + [f"mov r{t}=ar.bsp ;;", f"movl r{m}=0x{mask:x} ;;"]
                + rnat_read(u, m)
            )
        else:
            ar = rng.choice(["ar48", "ar63", "ar112", "ar127"])
            lines = [f"movl r{t}=0x{self.value():x} ;;", self.pfx(qp) + f"mov {ar}=r{t}"]
        return lines, "ar"

    def addr(self, size):
        return SCRATCH + size * self.rng.randrange(NAT_AREA * 8 // size)

    def i_mem(self, qp, loop=None):
        rng = self.rng
        size = rng.choice(SIZES)
        a = rng.randrange(1, 32)
        self.taint.discard(a)
        lines = [f"movl r{a}=0x{self.addr(size):x} ;;"]
        k = rng.randrange(10)
        post = ""
        post_reg = None
        if rng.random() < 0.35:
            if k < 4 and rng.random() < 0.4:
                post_reg = self.src()
                post = f",r{post_reg}"
            else:
                post = f",{rng.randrange(-256, 256)}"
        if k < 4:  # load
            d = self.dest()
            while post and d == a:
                d = self.dest()
            comp = rng.choice(["", "", ".acq", ".bias"]) + rng.choice(["", "", ".nt1", ".nta"])
            fill = size == 8 and rng.random() < 0.2
            op = "ld8.fill" + rng.choice(["", ".nt1", ".nta"]) if fill else f"ld{size}{comp}"
            lines.append(self.pfx(qp) + f"{op} r{d}=[r{a}]{post}")
            if post_reg is not None and post_reg in self.taint:
                self.taint.add(a)
            self.write_gr(d, qp, fill or (qp and d in self.taint))
            return lines, "ld"
        if k < 7:  # store
            spill = size == 8 and rng.random() < 0.25
            if post.startswith(",r"):
                post = ""
            if spill:
                v = self.src()
                if v in self.taint:  # the memory image would be implementation-specific
                    lines[0] = f"movl r{a}=0x{SCRATCH + 8 * rng.randrange(NAT_AREA, NWORDS):x} ;;"
                lines.append(
                    self.pfx(qp) + f"st8.spill{rng.choice(['', '.nta'])} [r{a}]=r{v}{post}"
                )
            else:
                v = self.clean_gr()
                rel = rng.choice(["", "", ".rel"]) + rng.choice(["", "", ".nta"])
                lines.append(self.pfx(qp) + f"st{size}{rel} [r{a}]=r{v}{post}")
            return lines, "st"
        if k == 7:  # advanced load + check load (deterministic without chk.a)
            d = self.dest()
            while d == a:
                d = self.dest()
            lines.append(f"ld{size}.a r{d}=[r{a}] ;;")
            if rng.random() < 0.5:
                b = rng.randrange(1, 32)
                while b in (a, d):
                    b = rng.randrange(1, 32)
                lines.append(f"movl r{b}=0x{self.addr(size) if rng.random() < 0.5 else 0:x} ;;")
                if b in self.taint:
                    self.taint.discard(b)
                if rng.random() < 0.5:
                    lines[-1] = f"mov r{b}=r{a} ;;"
                v = self.clean_gr()
                while v == d:
                    v = self.clean_gr()
                lines.append(
                    f"(p0) st{size} [r{b}]=r{v} ;;"
                    if not lines[-1].endswith("=0x0 ;;")
                    else "nop.m 0 ;;"
                )
            comp = rng.choice([".c.clr", ".c.nc", ".c.clr.acq"])
            lines.append(self.pfx(qp) + f"ld{size}{comp} r{d}=[r{a}]")
            self.taint.discard(d) if not qp else None
            return lines, "ldc"
        # atomics
        d = self.dest()
        v = self.clean_gr()
        sem = rng.choice(["acq", "rel"])
        kind = rng.randrange(3)
        if kind == 0:
            if rng.random() < 0.5:
                t = rng.randrange(1, 32)
                while t == a:
                    t = rng.randrange(1, 32)
                lines.append(f"ld{size} r{t}=[r{a}] ;;")
                if rng.random() < 0.3:
                    lines.append(f"dep r{t}=-1,r{t},{size * 8 if size < 8 else 63},1 ;;")
                lines.append(f"mov ar.ccv=r{t} ;;")
                self.taint.discard(t)
                v = self.clean_gr()
            lines.append(self.pfx(qp) + f"cmpxchg{size}.{sem} r{d}=[r{a}],r{v},ar.ccv")
        elif kind == 1:
            lines.append(self.pfx(qp) + f"xchg{size} r{d}=[r{a}],r{v}")
        else:
            if size < 4:
                size = rng.choice([4, 8])
                lines[0] = f"movl r{a}=0x{self.addr(size):x} ;;"
            inc = rng.choice([-16, -8, -4, -1, 1, 4, 8, 16])
            lines.append(self.pfx(qp) + f"fetchadd{size}.{sem} r{d}=[r{a}],{inc}")
        self.write_gr(d, qp, qp and d in self.taint)
        return lines, "atomic"

    def i_misc(self, qp, loop=None):
        rng = self.rng
        if qp == 63:  # nop.b counts as a branch for gas's (false) PR63 WAR check
            qp = 62
        k = rng.randrange(6)
        if k == 0:
            return [rng.choice(["clrrrb", "clrrrb.pr"])] if loop != "while" else ["nop.i 0"], "rrb"
        if k == 1:
            return [rng.choice(["mf", "mf.a", "sync.i", "invala"])], "misc"
        if k == 2:
            return [self.pfx(qp) + f"invala.e r{self.gr()}"], "misc"
        if k == 3:
            unit = rng.choice(["nop.m", "nop.i", "nop.b", "nop.f", "nop.x"])
            imm = rng.getrandbits(62) if unit == "nop.x" else rng.randrange(1 << 21)
            return [self.pfx(qp) + f"{unit} 0x{imm:x}"], "misc"
        if k == 4:
            if rng.random() < 0.3:  # mov dahr=imm16 is a hint.m encoding on these models
                return [
                    self.pfx(qp) + f"mov dahr{rng.randrange(8)}={rng.randrange(1 << 16)}"
                ], "misc"
            return [self.pfx(qp) + f"hint.{rng.choice(['i', 'm'])} 0"], "misc"
        return ["nop.m 0"], "misc"

    # -- composition -------------------------------------------------------
    def simple(self, loop=None):
        """One straight-line instruction (possibly with setup lines)."""
        r = self.rng.random()
        qp = self.qp("ctop" if loop == "ctop" else None)
        if self.big_endian and self.rng.random() < 0.06:  # UM.be toggle
            lines, tag = [self.pfx(qp) + f"{self.rng.choice(['sum', 'rum'])} 0x2"], "psr"
        elif r < 0.24:
            lines, tag = self.i_alu3(qp)
        elif r < 0.40:
            lines, tag = self.i_alu_imm(qp)
        elif r < 0.52:
            lines, tag = self.i_cmp(qp, loop)
        elif r < 0.58:
            lines, tag = self.i_mov(qp, loop)
        elif r < 0.62:
            lines, tag = self.i_ar(qp, loop)
        elif r < 0.76:
            lines, tag = self.i_mem(qp, loop)
        elif r < 0.88:
            lines, tag = self.i_fp(qp, loop)
        elif r < 0.93:
            lines, tag = self.i_sys(qp, loop)
        elif r < 0.97:
            lines, tag = self.i_mem2(qp, loop)
        else:
            lines, tag = self.i_misc(qp, loop)
        return Item(terminate(lines), tag=tag)

    def group(self, loop=None):
        """2-3 independent ALU/compare instructions in one instruction group."""
        insns, written, read = [], set(), set()
        for _ in range(self.rng.randrange(2, 4)):
            for _attempt in range(6):
                saved = set(self.taint)
                qp = self.qp("ctop" if loop == "ctop" else None)
                r = self.rng.random()
                if r < 0.4:
                    lines, _ = self.i_alu3(qp)
                elif r < 0.7:
                    lines, _ = self.i_alu_imm(qp)
                else:
                    lines, _ = self.i_cmp(qp, loop)
                w, rd = resources(lines[0])
                if w & (written | read) or rd & written:
                    self.taint = saved
                    continue
                written |= w
                read |= rd
                insns.append(lines[0])
                break
        if not insns:
            return self.simple(loop)
        insns[-1] += " ;;"
        return Item(insns, tag="group")

    def parallel_group(self, loop=None):
        """Several same-type parallel compares writing the same predicates in
        one instruction group (the architected WAW exception)."""
        rng = self.rng
        ctype = rng.choice(["and", "or", "or.andcm", "andcm", "orcm", "and.orcm"])
        hi = 16 if loop == "while" else 64
        p1 = rng.randrange(1, hi)
        p2 = rng.randrange(1, hi)
        while p2 == p1:
            p2 = rng.randrange(1, hi)
        lines = []
        for _ in range(rng.randrange(2, 4)):
            qp = 0 if rng.random() < 0.6 else rng.randrange(1, 16)
            while qp in (p1, p2):
                qp = rng.randrange(1, 16)
            k = rng.randrange(4)
            if k == 0:
                text = f"cmp.{rng.choice(['eq', 'ne'])}.{ctype} p{p1},p{p2}=r{self.src()},r{self.src()}"
            elif k == 1:
                text = f"cmp4.{rng.choice(['lt', 'le', 'gt', 'ge'])}.{ctype} p{p1},p{p2}=r0,r{self.src()}"
            elif k == 2:
                text = f"tbit.{rng.choice(['z', 'nz'])}.{ctype} p{p1},p{p2}=r{self.src()},{rng.randrange(64)}"
            else:
                pool = list(self.taint) if self.taint and rng.random() < 0.6 else list(range(32))
                text = f"tnat.{rng.choice(['z', 'nz'])}.{ctype} p{p1},p{p2}=r{rng.choice(pool)}"
            lines.append(self.pfx(qp) + text)
        lines[-1] += " ;;"
        return Item(lines, tag="pcmp")

    def straight(self, n, loop=None):
        out = []
        for _ in range(n):
            r = self.rng.random()
            if r < 0.15:
                out.append(self.group(loop))
            elif r < 0.20:
                out.append(self.parallel_group(loop))
            else:
                out.append(self.simple(loop))
        return out

    def skip(self):
        rng = self.rng
        qp = self.qp()
        if qp == 63:  # gas reports a (false) PR63 WAR across stops for branches
            qp = 62
        before = set(self.taint)
        same_group = rng.random() < 0.4  # SDM: PR/BR writes are visible to a
        stop = "" if same_group else " ;;"  # branch later in the same group
        k = rng.randrange(6)
        pre = []
        if same_group and k in (0, 2, 5) and qp:
            pa = rng.randrange(1, 32)
            if rng.random() < 0.7:
                pre = [
                    f"cmp.{rng.choice(CRELS)} p{qp},p{rng.choice([0, 1, 2])}=r{self.src()},r{self.src()}{stop}"
                ]
            else:
                self.taint.discard(pa)
                pre = [f"movl r{pa}=0x{rng.getrandbits(64):x} ;;", f"mov pr=r{pa},0x1ffff{stop}"]
        if k == 0:
            head = pre + [
                self.pfx(qp) + f"br.cond.{rng.choice(['sptk', 'spnt', 'dptk', 'dpnt'])}"
                f"{rng.choice(['', '.few', '.many'])} {{L}} ;;"
            ]
        elif k == 1:
            t, b = rng.randrange(1, 32), self.bnum()
            self.taint.discard(t)
            low = rng.choice([0, 0, rng.randrange(1, 16)])
            head = [
                f"movl r{t}={{L}}+{low} ;;",
                f"mov b{b}=r{t}{stop}",
                self.pfx(qp) + f"br.cond.sptk.few b{b} ;;",
            ]
        elif k == 2:
            head = pre + [self.pfx(qp) + "brl.cond.sptk.few {L} ;;"]
        elif k == 5:
            # Two branches in one group: the second runs only if the first
            # falls through; the first skips inner1, the second also inner2.
            mid = Item(["{L}:"])
            q2 = self.qp() % 63
            head = pre + [
                self.pfx(qp) + f"br.cond.sptk.few {mid_label(mid)}",
                self.pfx(q2) + "br.cond.sptk.few {L} ;;",
            ]
            inner = self.straight(rng.randrange(1, 3)) + [mid] + self.straight(rng.randrange(1, 3))
            self.taint |= before
            return Item(head, inner, ["{L}:"], tag="branch")
        else:
            pool = list(self.taint) if self.taint and rng.random() < 0.7 else list(range(32))
            head = [self.pfx(qp) + f"chk.s.{rng.choice(['i', 'm'])} r{rng.choice(pool)},{{L}} ;;"]
        inner = self.straight(rng.randrange(1, 4))
        self.taint |= before
        return Item(head, inner, ["{L}:"], tag="branch")

    def loop(self):
        rng = self.rng
        kind = rng.choice(["cloop", "ctop", "cexit", "wtop", "wexit"])
        lc, ec = rng.randrange(0, 4), rng.choice([0, 0, 1, 1, 2, 3, 4, 63])
        if kind in ("ctop", "cexit") and ec > 4:
            ec = rng.randrange(0, 4)
        before = set(self.taint)
        while_kind = kind in ("wtop", "wexit")
        inner_loop = "while" if while_kind else ("ctop" if kind != "cloop" else "cloop")
        # Rotating loop branches rename the frame's rotating GRs, so taint
        # tracked per logical name is made uniform across them.
        rotating = kind != "cloop" and self.frame.sor > 0

        def unify():
            if rotating and self.taint & self.frame.rot:
                self.taint |= self.frame.rot

        # Run the body twice so taint reaching the loop head is accounted for.
        n = rng.randrange(1, 4)
        unify()
        state = self.rng.getstate()
        self.straight(n, inner_loop)
        unify()
        self.rng.setstate(state)
        inner = self.straight(n, inner_loop)
        self.taint |= before
        unify()
        last = inner[-1]
        if (
            rng.random() < 0.4
            and last.inner is None
            and last.tag in ("alu", "cmp", "group")
            and last.head[-1].endswith(" ;;")
        ):
            w = set()
            for line in last.head:
                if not line.endswith(":") and "=" in line:
                    w |= resources(line.removesuffix(" ;;"))[0]
            if "p63" not in w and sum(l.endswith(";;") for l in last.head) == 1:
                inner[-1] = Item(last.head[:-1] + [last.head[-1][:-3]], tag=last.tag)
        lc_line = (
            f"mov ar.lc={lc} ;;" if rng.random() < 0.7 else f"movl r2={lc} ;;\nmov ar.lc=r2 ;;"
        )
        if kind == "cloop":
            head = lc_line.split("\n") + ["{L}:"]
            tail = ["br.cloop.sptk.few {L} ;;"]
        elif kind == "ctop":
            head = lc_line.split("\n") + [f"mov ar.ec={ec} ;;", "{L}:"]
            tail = ["br.ctop.sptk.few {L} ;;"]
        elif kind == "cexit":
            head = lc_line.split("\n") + [f"mov ar.ec={ec} ;;", "{L}:"]
            tail = ["br.cexit.spnt.few {L}x ;;", "br.cond.sptk.few {L} ;;", "{L}x:"]
        else:
            q = rng.randrange(16, 21)
            head = [f"mov ar.ec={min(ec, 5)} ;;"]
            if rng.random() < 0.5:
                head.append(f"mov pr.rot=0x{(rng.getrandbits(5) << 16):x} ;;")
            head.append("{L}:")
            if kind == "wtop":
                tail = [f"(p{q}) br.wtop.sptk.few {{L}} ;;"]
            else:
                tail = [f"(p{q}) br.wexit.spnt.few {{L}}x ;;", "br.cond.sptk.few {L} ;;", "{L}x:"]
        if "movl r2" in lc_line:
            self.taint.discard(2)
        return Item(head, inner, tail, tag="loop")

    def prologue(self):
        rng = self.rng
        items = []
        nat_regs = rng.sample(list(range(4, 32)) + self.frame.regs, rng.randrange(0, 7))
        for r in nat_regs:
            spill = SCRATCH + 8 * rng.randrange(NWORDS)
            items.append(
                Item(
                    [
                        f"movl r3=0x{spill:x} ;;",
                        f"movl r2=0x{1 << ((spill >> 3) & 63):x} ;;",
                        "mov ar.unat=r2 ;;",
                        f"ld8.fill r{r}=[r3] ;;",
                    ],
                    tag="init",
                )
            )
            self.taint.add(r)
        items.append(Item([f"movl r2=0x{rng.getrandbits(64):x} ;;", "mov pr=r2,-1 ;;"], tag="init"))
        for b in range(8):
            if rng.random() < 0.6:
                items.append(
                    Item([f"movl r2=0x{self.value():x} ;;", f"mov b{b}=r2 ;;"], tag="init")
                )
        for ar in ("ccv", "unat", "lc", "ec", "k0", "k3", "k6"):
            v = self.value() if ar != "ec" else rng.randrange(64)
            items.append(Item([f"movl r2=0x{v:x} ;;", f"mov ar.{ar}=r2 ;;"], tag="init"))
        for f in list(range(2, 32)) + rng.sample(range(32, 128), 12):
            kind = rng.choice(["sig", "sig", "d", "s", "exp"])
            v = self.fp_value(kind)
            items.append(Item([f"movl r2=0x{v:x} ;;", f"setf.{kind} f{f}=r2 ;;"], tag="init"))
        for r in range(1, 32):
            if r not in nat_regs:
                items.append(Item([f"movl r{r}=0x{self.value():x} ;;"], tag="init"))
        return items

    # -- system: PSR, control registers ------------------------------------------
    # PSR bits safe to toggle: up ac mfl mfh pk dfl dfh sp di si.  Never be
    # (not built in the interpreter), dt/it/rt (virtual mode), i, ic (except in
    # the interruption-CR bracket), bn (bsw), the debug/trap bits, cpl, ri.
    PSR_SAFE = [2, 3, 4, 5, 15, 18, 19, 20, 22, 23]

    def i_sys(self, qp, loop=None):
        rng = self.rng
        k = rng.randrange(8)
        d = self.dest()
        t = rng.randrange(3, 32)  # r2 is a temporary below
        if k == 0:
            self.write_gr(d, qp, qp and d in self.taint)
            return [self.pfx(qp) + f"mov r{d}=psr"], "psr"
        if k == 1:
            bits = sum(1 << b for b in rng.sample(self.PSR_SAFE, rng.randrange(1, 4)))
            op = rng.choice(["rsm", "ssm"])
            return [self.pfx(qp) + f"{op} 0x{bits:x} ;;", "srlz.d"], "psr"
        if k == 2:
            um = [1, 2, 3, 4, 5] if self.big_endian else [2, 3, 4, 5]
            bits = sum(1 << b for b in rng.sample(um, rng.randrange(1, 3)))
            if rng.random() < 0.3:  # mov psr.um (and back)
                self.taint.discard(t)
                self.write_gr(d, qp, qp and d in self.taint)
                return [
                    f"movl r{t}=0x{bits:x} ;;",
                    self.pfx(qp) + f"mov psr.um=r{t} ;;",
                    "srlz.d ;;",
                    f"mov r{d}=psr.um",
                ], "psr"
            return [self.pfx(qp) + f"{rng.choice(['rum', 'sum'])} 0x{bits:x} ;;", "srlz.d"], "psr"
        if k == 3:
            bits = sum(1 << b for b in rng.sample(self.PSR_SAFE, rng.randrange(1, 4)))
            self.taint.discard(t)
            return [
                f"mov r{t}=psr ;;",
                f"movl r2=0x{bits:x} ;;",
                f"xor r{t}=r{t},r2 ;;",
                self.pfx(qp) + f"mov psr.l=r{t} ;;",
                "srlz.d",
            ], "psr"
        if k == 4:
            # (LRR0/1 are not read: their reset value is model-defined, README.)
            cr = rng.choice(
                [
                    "dcr",
                    "itm",
                    "iva",
                    "pta",
                    "ipsr",
                    "isr",
                    "iip",
                    "iipa",
                    "ifs",
                    "itv",
                    "tpr",
                    "lid",
                    "irr0",
                    "irr1",
                    "irr2",
                    "irr3",
                    "pmv",
                    "cmcv",
                    "ivr",
                ]
            )
            self.write_gr(d, qp, qp and d in self.taint)
            return [self.pfx(qp) + f"mov r{d}=cr.{cr}"] + (
                [";;", "srlz.d"] if cr == "ivr" else []
            ), "cr"
        if k == 5:
            # Writable non-interruption CRs with defined values (masked
            # vectors stay masked: no timer/external interrupt delivery).
            cr = rng.choice(["dcr", "pta", "tpr", "itv", "pmv", "cmcv"])
            v = {
                "dcr": rng.getrandbits(1)
                | (rng.getrandbits(1) << 1 if self.big_endian else 0)
                | rng.getrandbits(7) << 8,
                "pta": rng.getrandbits(1)
                | rng.randrange(15, 62) << 2
                | rng.getrandbits(1) << 8
                | rng.getrandbits(49) << 15,
                "tpr": rng.getrandbits(4) << 4 | rng.getrandbits(1) << 16,
                "itv": rng.randrange(16, 256) | 1 << 16,
                "pmv": rng.randrange(16, 256) | 1 << 16,
                "cmcv": rng.randrange(16, 256) | 1 << 16,
            }[cr]
            if rng.random() < 0.05:  # a reserved field (per CR): Reserved Register/Field
                v |= 1 << rng.choice(
                    {
                        "dcr": [3, 7, 15, 40, 63],
                        "pta": [1, 9, 14],
                        "tpr": [0, 3, 8, 20, 63],
                        "itv": [9, 13, 20, 63],
                        "pmv": [9, 13, 20, 63],
                        "cmcv": [9, 13, 20, 63],
                    }[cr]
                )
            self.taint.discard(t)
            return [f"movl r{t}=0x{v:x} ;;", self.pfx(qp) + f"mov cr.{cr}=r{t} ;;", "srlz.d"], "cr"
        if k == 6:
            # Interruption CRs: writable only with PSR.ic = 0 (else Illegal
            # Operation); write a valid value and read it back.  A fault in
            # here would be delivered with PSR.ic = 0 and resume from stale
            # IIP/IPSR, so only values without reserved fields are written
            # (IFS: a consistent frame marker; ITIR: a supported page size).
            cr = rng.choice(["iip", "ifa", "itir", "iipa", "iim", "iha", "ifs", "isr", "ipsr"])
            pfm = self.pfs_value() & ((1 << 38) - 1)
            v = {
                "iip": self.value(),
                "ifa": self.value(),
                "iipa": self.value(),
                "iim": self.value(),
                "itir": rng.choice([12, 13, 14, 16, 18, 20, 22, 24, 26, 28]) << 2
                | rng.getrandbits(24) << 8,
                "iha": rng.getrandbits(62) << 2,
                "ifs": pfm | rng.getrandbits(1) << 63,
                # ISR: code/vector and x..ed, with ei in 0..2 (3 is reserved)
                "isr": rng.getrandbits(24)
                | rng.getrandbits(9) << 32
                | rng.randrange(3) << 41
                | rng.getrandbits(1) << 43,
                "ipsr": rng.choice([0x2000, 0x100000002030, 0x1000002000, 0x20000002000]),
            }[cr]
            u = rng.choice([x for x in range(1, 32) if x != t])
            self.taint -= {t, u}
            if rng.random() < 0.15:  # with PSR.ic = 1: Illegal Operation fault
                return [
                    f"movl r{t}=0x{v:x} ;;",
                    self.pfx(qp) + f"mov cr.{cr}=r{t} ;;",
                    "srlz.d",
                ], "cr"
            # Unpredicated: a skipped write would read back an older, possibly
            # undefined (Table 8-1 "x") value.
            return [
                "rsm 0x2000 ;;",
                "srlz.d ;;",
                f"movl r{t}=0x{v:x} ;;",
                f"mov cr.{cr}=r{t} ;;",
                "srlz.d ;;",
                f"mov r{u}=cr.{cr} ;;",
                "ssm 0x2000 ;;",
                "srlz.d",
            ], "cr"
        # speculative loads: valid address, definite-NaT base, unimplemented PA
        size = rng.choice(SIZES)
        op = f"ld{size}.{rng.choice(['s', 's', 'sa'])}"
        kind = rng.randrange(3)
        lines = []
        if kind == 0:
            lines = [f"movl r{t}=0x{self.addr(size):x} ;;"]
            self.taint.discard(t)
        elif kind == 1:
            spill = SCRATCH + 8 * rng.randrange(NAT_AREA)
            lines = [
                f"movl r{t}=0x{spill:x} ;;",
                f"movl r2=0x{1 << ((spill >> 3) & 63):x} ;;",
                "mov ar.unat=r2 ;;",
                f"ld8.fill r{t}=[r{t}] ;;",
            ]
            self.taint.add(t)
            self.taint.discard(2)
        else:
            lines = [f"movl r{t}=0x{self.addr(size) | 1 << rng.randrange(50, 63):x} ;;"]
            self.taint.discard(t)
        while d == t:
            d = self.dest()
        lines.append(self.pfx(qp) + f"{op} r{d}=[r{t}]")
        self.write_gr(d, qp, True)
        return lines, "spec"

    def i_mem2(self, qp, loop=None):
        """16-byte accesses (Montecito: CPUID[4].ao), lfetch, fc."""
        rng = self.rng
        t = rng.randrange(1, 32)
        self.taint.discard(t)
        k = rng.randrange(5)
        if k <= 2:
            lines = [f"movl r{t}=0x{self.addr(16):x} ;;"]
            d = self.dest()
            while d == t:
                d = self.dest()
            if k == 0:
                lines.append(self.pfx(qp) + f"ld16{rng.choice(['', '.acq'])} r{d},ar.csd=[r{t}]")
                self.write_gr(d, qp, qp and d in self.taint)
            elif k == 1:
                lines.append(
                    self.pfx(qp)
                    + f"st16{rng.choice(['', '.rel'])} [r{t}]=r{self.clean_gr()},ar.csd"
                )
            else:
                if rng.random() < 0.5:
                    u = rng.choice([x for x in range(1, 32) if x != t])
                    self.taint.discard(u)
                    lines += [f"ld8 r{u}=[r{t}] ;;", f"mov ar.ccv=r{u} ;;"]
                lines.append(
                    self.pfx(qp) + f"cmp8xchg16.{rng.choice(['acq', 'rel'])} "
                    f"r{d}=[r{t}],r{self.clean_gr()},ar.csd,ar.ccv"
                )
                self.write_gr(d, qp, qp and d in self.taint)
            return lines, "mem16"
        if k == 3:
            hint = rng.choice(["", ".nt1", ".nt2", ".nta"])
            excl = rng.choice(["", ".excl"])
            flt = rng.choice(["", ".fault"])
            post = rng.choice(["", f",{rng.randrange(-256, 256)}", f",r{self.src()}"])
            if post.startswith(",r") and int(post[2:]) in self.taint:
                self.taint.add(t)
            return [
                f"movl r{t}=0x{self.addr(8):x} ;;",
                self.pfx(qp) + f"lfetch{flt}{excl}{hint} [r{t}]{post}",
            ], "lfetch"
        return [
            f"movl r{t}=0x{self.addr(8):x} ;;",
            self.pfx(qp) + f"{rng.choice(['fc', 'fc.i'])} r{t}",
        ], "fc"

    # -- floating point (integer-visible / exact operations) -----------------
    FP_SPECIAL_D = [
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
        0x7FEFFFFFFFFFFFFF,
        0x4330000000000000,
        0x43E0000000000000,
        0xC3E0000000000000,
        0x41DFFFFFFFC00000,
        0x3FE0000000000000,
    ]

    def fp_value(self, kind):
        rng = self.rng
        if kind == "d":
            return rng.choice(self.FP_SPECIAL_D) if rng.random() < 0.5 else rng.getrandbits(64)
        if kind == "s":
            return (
                rng.choice(
                    [
                        0,
                        0x80000000,
                        0x3F800000,
                        0x7F800000,
                        0x7FC00000,
                        0x7FA00000,
                        1,
                        0x007FFFFF,
                        0x4F000000,
                        0xCF000000,
                    ]
                )
                if rng.random() < 0.5
                else rng.getrandbits(32)
            )
        if kind == "exp":
            return (
                rng.choice([0, 0xFFFF, 0x1003E, 0x1FFFF, 0x2FFFF, 0x1FFFE, 1, 0x10000])
                if rng.random() < 0.5
                else rng.getrandbits(18)
            )
        return self.value()

    def fr(self, dest=False):
        rng = self.rng
        if not dest and rng.random() < 0.08:
            return rng.choice([0, 1])
        return rng.randrange(2, 32) if rng.random() < 0.7 else rng.randrange(32, 128)

    def clean_fr(self):
        choices = [f for f in range(2, 128) if f not in self.ftaint]
        return self.rng.choice(choices) if choices and self.rng.random() < 0.95 else 0

    def write_fr(self, d, qp, tainted):
        if tainted:
            self.ftaint.add(d)
        elif not qp:
            self.ftaint.discard(d)

    def i_fp(self, qp, loop=None):
        rng = self.rng
        sf = f".s{rng.randrange(4)}"
        k = rng.randrange(14)
        d = self.fr(True)
        a, b, c = self.fr(), self.fr(), self.fr()
        ft = lambda *xs: any(x in self.ftaint for x in xs) or (qp and d in self.ftaint)
        if k == 0:
            kind = rng.choice(["sig", "exp", "s", "d"])
            g = self.src()
            self.write_fr(d, qp, g in self.taint or (qp and d in self.ftaint))
            return [self.pfx(qp) + f"setf.{kind} f{d}=r{g}"], "fp"
        if k == 1:
            kind = rng.choice(["sig", "exp", "s", "d"])
            g = self.dest()
            self.write_gr(g, qp, a in self.ftaint or (qp and g in self.taint))
            return [self.pfx(qp) + f"getf.{kind} r{g}=f{a}"], "fp"
        if k == 2:
            op = rng.choice(["xma.l", "xma.h", "xma.hu"])
            self.write_fr(d, qp, ft(a, b, c))
            return [self.pfx(qp) + f"{op} f{d}=f{a},f{b},f{c}"], "fp"
        if k == 3:
            op = rng.choice(["fcvt.fx", "fcvt.fxu", "fcvt.fx.trunc", "fcvt.fxu.trunc"])
            self.write_fr(d, qp, ft(a))
            return [self.pfx(qp) + f"{op}{sf} f{d}=f{a}"], "fp"
        if k == 4:
            self.write_fr(d, qp, ft(a))
            return [self.pfx(qp) + f"fcvt.xf f{d}=f{a}"], "fp"
        if k in (5, 6):
            op = rng.choice(
                [
                    "fmerge.s",
                    "fmerge.ns",
                    "fmerge.se",
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
                    "fpmerge.s",
                    "fpmerge.ns",
                    "fpmerge.se",
                    "fpack",
                ]
            )
            self.write_fr(d, qp, ft(a, b))
            return [self.pfx(qp) + f"{op} f{d}=f{a},f{b}"], "fp"
        if k == 7:
            self.write_fr(d, qp, ft(a, b, c))
            return [self.pfx(qp) + f"fselect f{d}=f{a},f{b},f{c}"], "fp"
        if k == 8:
            p1, p2 = self.pred_pair()
            if loop == "while":
                p1, p2 = rng.sample(range(0, 16), 2)
            # Include .unc destination preservation on FP faults with --known.
            ctype = rng.choice(["", ".unc"]) if self.known else ""
            if rng.random() < 0.5:
                rel = rng.choice(
                    [
                        "eq",
                        "lt",
                        "le",
                        "gt",
                        "ge",
                        "unord",
                        "neq",
                        "nlt",
                        "nle",
                        "ngt",
                        "nge",
                        "ord",
                    ]
                )
                return [self.pfx(qp) + f"fcmp.{rel}{ctype}{sf} p{p1},p{p2}=f{a},f{b}"], "fp"
            cls = rng.randrange(1, 0x200)
            return [
                self.pfx(qp) + f"fclass.{rng.choice(['m', 'nm'])}{ctype} p{p1},p{p2}=f{a},0x{cls:x}"
            ], "fp"
        if k == 9:
            # FR memory: exact formats only (no rounding stores).
            fmt, size = rng.choice([("8", 8), ("s", 4), ("d", 8), ("e", 16), (".fill", 16)])
            t = rng.randrange(1, 32)
            self.taint.discard(t)
            lines = [f"movl r{t}=0x{self.addr(size):x} ;;"]
            if rng.random() < 0.6:
                self.write_fr(d, qp, fmt == ".fill" or (qp and d in self.ftaint))
                post = f",{rng.choice([0, size, -size, 16])}" if rng.random() < 0.3 else ""
                lines.append(self.pfx(qp) + f"ldf{fmt} f{d}=[r{t}]{post}")
            else:
                st = {"8": "stf8", "s": None, "d": None, "e": "stfe", ".fill": "stf.spill"}[fmt]
                if st is None:
                    st, size = "stf8", 8
                    lines[0] = f"movl r{t}=0x{self.addr(8):x} ;;"
                if st == "stf.spill":
                    src = self.fr()
                    if src in self.ftaint:  # NaTVal spill image is defined, but keep out
                        src = self.clean_fr()
                else:
                    src = self.clean_fr()
                lines.append(self.pfx(qp) + f"{st} [r{t}]=f{src}")
            return lines, "fpmem"
        if k == 10:
            t = rng.randrange(1, 32)
            self.taint.discard(t)
            e = rng.choice([x for x in range(2, 128) if x % 2 == 0])
            o = rng.choice([x for x in range(3, 128) if x % 2 == 1 and (x < 32) == (e < 32)])
            pair = (e, o) if rng.random() < 0.5 else (o, e)
            op, size = rng.choice([("ldfp8", 16), ("ldfps", 8), ("ldfpd", 16)])
            for f in pair:
                self.write_fr(f, qp, qp and f in self.ftaint)
            return [
                f"movl r{t}=0x{self.addr(16):x} ;;",
                self.pfx(qp) + f"{op} f{pair[0]},f{pair[1]}=[r{t}]",
            ], "fpmem"
        if k == 11:
            if rng.random() < 0.5:
                return [
                    self.pfx(qp) + f"fsetc{sf} 0x{rng.randrange(128):x},0x{rng.randrange(128):x}"
                ], "fp"
            return [self.pfx(qp) + f"fclrf{sf}"], "fp"
        if k == 12 and self.fp_arith:
            op = rng.choice(["fma", "fms", "fnma", "fma.s", "fma.d", "fpma", "fpms", "fpnma"])
            pc = ""
            self.write_fr(d, qp, ft(a, b, c))
            return [self.pfx(qp) + f"{op}{pc}{sf} f{d}=f{a},f{b},f{c}"], "fparith"
        if k == 12 or k == 13:
            if self.fp_arith and k == 13:
                op = rng.choice(
                    ["fmin", "fmax", "famin", "famax", "fpmin", "fpmax", "fadd", "fsub", "fmpy"]
                )
                self.write_fr(d, qp, ft(a, b))
                return [self.pfx(qp) + f"{op}{sf} f{d}=f{a},f{b}"], "fparith"
            self.write_fr(d, qp, ft(a))
            return [self.pfx(qp) + f"mov f{d}=f{a}"], "fp"
        return [self.pfx(qp) + f"mov f{d}=f{a}"], "fp"

    # -- frames, calls and the RSE ------------------------------------------
    def frame_shape(self, inputs, min_locals=0, reserve=0):
        """(i, l, o, r) for alloc: r*8 rotating registers kept below the
        last `reserve` locals (a subroutine's saved PFS/b0)."""
        rng = self.rng
        l = rng.randrange(min_locals, min_locals + 14)
        o = rng.randrange(0, 9)
        while inputs + l + o > 96:
            l, o = max(min_locals, l - 4), max(0, o - 2)
        rmax = (inputs + l - reserve) // 8
        r = 8 * rng.randrange(0, min(rmax, 3) + 1) if rng.random() < 0.5 else 0
        return inputs, l, o, r

    def init_frame(self, regs):
        """Initialize freshly allocated registers (architecturally undefined
        after alloc): 2-3 independent `mov rN=imm22` per group."""
        lines = []
        regs = list(regs)
        while regs:
            group = regs[: self.rng.randrange(1, 4)]
            regs = regs[len(group) :]
            for k, reg in enumerate(group):
                v = self.rng.randrange(-(1 << 21), 1 << 21)
                lines.append(f"mov r{reg}={v}" + (" ;;" if k == len(group) - 1 else ""))
                self.taint.discard(reg)
        return lines

    def setup(self):
        """Fixed preamble: IVT, interruption collection, bank 1, enforced-lazy
        RSE on a backing store in RAM, and the main register frame."""
        i, l, o, r = self.frame_shape(0, 0)
        self.frame = Frame(l + o, l, r // 8)
        lines = [
            f"movl r2=0x{IVA:x} ;;",
            "mov cr.iva=r2 ;;",
            "srlz.i ;;",
            "ssm 0x2000 ;;",
            "srlz.d ;;",
            "bsw.1 ;;",
            "mov ar.rsc=0 ;;",
            f"movl r2=0x{BSTORE:x} ;;",
            "mov ar.bspstore=r2 ;;",
        ]
        if self.rsc_be:
            lines.append(f"mov ar.rsc={self.rsc_be} ;;")
        lines.append(f"alloc r2=ar.pfs,0,{l},{o},{r} ;;")
        for reg in self.frame.regs:
            lines.append(f"movl r{reg}=0x{self.value():x} ;;")
        if self.virtual:
            lines += self.mmu_setup()
        if self.big_endian and self.rng.random() < 0.5:
            lines.append("sum 0x2 ;;")
        return lines

    def mmu_setup(self):
        """Guest-owned translations and VHPT; both CPUs execute the same IVT.

        Region 0 pins code, IVT, scratch and backing store. Region 1 aliases
        scratch via a short/long VHPT and an explicit TC insertion. No oracle
        firmware identity assists are involved in this non-firmware window.
        """
        rng = self.rng
        ps, rid, long_format = rng.choice([12, 13, 14]), rng.randrange(1, 16), rng.randrange(2)
        self.mmu_rid = rid
        alias, table = (1 << 61) | 0x400000, 0x130000
        hpn = (alias & ((1 << 61) - 1)) >> ps
        offset = (((hpn ^ rid) << 5) if long_format else hpn << 3) & 0x7FFF
        pte_addr = table + offset
        tag = hpn ^ rid << 39
        lines = [
            "rsm psr.ic ;;",
            "mov r2=0 ;;",
            "mov r3=48 ;;",
            "mov rr[r2]=r3 ;;",
            "mov cr.ifa=r2 ;;",
            "mov r3=112 ;;",
            "mov cr.itir=r3 ;;",
            "movl r3=0x100000000007e1 ;;",
            "itr.i itr[r2]=r3 ;;",
            "mov r3=2017 ;;",
            "itr.d dtr[r2]=r3 ;;",
            "srlz.d ;;",
            "srlz.i ;;",
            "mov r3=1 ;;",
            "mov pkr[r2]=r3 ;;",
            f"movl r2=0x{alias:x} ;;",
            f"mov r3={(rid << 8) | (ps << 2) | 1} ;;",
            "mov rr[r2]=r3 ;;",
            "mov r2=1 ;;",
            f"mov r3={(rid << 8) | 1} ;;",
            "mov pkr[r2]=r3 ;;",
            f"movl r3=0x{(1 << 61) | table:x} ;;",
            "mov cr.ifa=r3 ;;",
            "mov r3=64 ;;",
            "mov cr.itir=r3 ;;",
            f"movl r3=0x{table | 0x7E1:x} ;;",
            "itr.d dtr[r2]=r3 ;;",
            "srlz.d ;;",
            f"movl r2=0x{table | 61 | (long_format << 8):x} ;;",
            "mov cr.pta=r2 ;;",
            f"movl r2=0x{pte_addr:x} ;;",
            f"movl r3=0x{SCRATCH | 0x7E1:x} ;;",
            "st8 [r2]=r3,8 ;;",
            f"mov r3={(rid << 8) | (ps << 2)} ;;",
            "st8 [r2]=r3,8 ;;",
            f"movl r3=0x{tag:x} ;;",
            "st8 [r2]=r3 ;;",
            "mov r3=psr ;;",
            f"movl r2=0x{(1 << 44) | (1 << 36) | (1 << 27) | (1 << 17) | (1 << 13):x} ;;",
            "or r3=r3,r2 ;;",
            "mov cr.ipsr=r3 ;;",
            "movl r2=.Lmmu_enabled ;;",
            "mov cr.iip=r2 ;;",
            "mov cr.ifs=r0 ;;",
            "rfi ;;",
            ".align 16",
            ".Lmmu_enabled:",
            f"movl r2=0x{alias:x} ;;",
            "ld8 r4=[r2] ;;",
            "probe.r r5=r2,3 ;;",
            "tpa r6=r2 ;;",
            "tak r7=r2 ;;",
            "thash r3=r2 ;;",
            "ttag r4=r2 ;;",
            "mov r3=psr ;;",
            "rsm psr.ic ;;",
            "mov cr.ifa=r2 ;;",
            f"mov r4={(rid << 8) | (ps << 2)} ;;",
            "mov cr.itir=r4 ;;",
            f"movl r4=0x{SCRATCH | 0x7E1:x} ;;",
            "itc.d r4 ;;",
            "srlz.d ;;",
            "mov psr.l=r3 ;;",
            "srlz.d ;;",
            "ld8 r4=[r2] ;;",
        ]
        for purge in rng.sample(["ptc.l", "ptc.g", "ptc.ga"], 3):
            lines += [f"mov r3={ps << 2} ;;", f"{purge} r2,r3 ;;", "srlz.d ;;", "ld8 r4=[r2] ;;"]
        lines += ["ptc.e r2 ;;", "srlz.d ;;", "ld8 r4=[r2] ;;"]
        serialized = []
        for line in lines:
            serialized.append(line)
            if line.startswith(("rsm psr.ic", "mov rr[", "mov pkr[", "mov cr.pta=")):
                serialized.append("srlz.d ;;")
        return serialized

    def mmu_fault_tail(self):
        """One architected data MMU fault after the ordinary random body."""
        kind = self.rng.choice(range(10))
        alias = (1 << 61) | 0x600000
        flags = [0, 0x7FD, 0x7E1, 0x7E1, 0x661, 0x7A1, 0x7C1]
        lines = ["rsm psr.ic ;;", "srlz.d ;;", "rsm psr.pk ;;", "srlz.d ;;"]
        if kind < 7:
            lines += [
                f"movl r2=0x{alias:x} ;;",
                "mov cr.ifa=r2 ;;",
                "mov r3=16944 ;;",
                "mov cr.itir=r3 ;;",
                f"movl r3=0x{SCRATCH | flags[kind]:x} ;;",
                "itc.d r3 ;;",
                "srlz.d ;;",
            ]
            if kind in (2, 3):
                lines += [
                    "mov r3=2 ;;",
                    f"mov r4={0x4205 if kind == 3 else 0} ;;",
                    "mov pkr[r3]=r4 ;;",
                    "ssm psr.pk ;;",
                    "srlz.d ;;",
                ]
        elif kind == 7:
            alias = (2 << 61) | 0x600000
            lines += [f"movl r2=0x{alias:x} ;;", "mov r3=560 ;;", "mov rr[r2]=r3 ;;", "srlz.d ;;"]
        else:
            rid = self.mmu_rid
            table, pta = 0x130000, 0x13013D
            offset = (((alias & ((1 << 61) - 1)) >> 12 ^ rid) << 5) & 0x7FFF
            lines += [
                f"movl r2=0x{alias:x} ;;",
                f"mov r3={(rid << 8) | 49} ;;",
                "mov rr[r2]=r3 ;;",
                "srlz.d ;;",
                f"movl r3=0x{pta:x} ;;",
                "mov cr.pta=r3 ;;",
                "srlz.d ;;",
            ]
            if kind == 8:
                lines += [f"movl r3=0x{table:x} ;;", "mov r4=112 ;;", "ptr.d r3,r4 ;;", "srlz.d ;;"]
            else:
                tag = ((alias & ((1 << 61) - 1)) >> 12) ^ rid << 39
                lines += [
                    f"movl r3=0x{table + offset:x} ;;",
                    f"movl r4=0x{SCRATCH | 0x7E3:x} ;;",
                    "st8 [r3]=r4,8 ;;",
                    "mov r4=48 ;;",
                    "st8 [r3]=r4,8 ;;",
                    f"movl r4=0x{tag:x} ;;",
                    "st8 [r3]=r4 ;;",
                ]
        lines += ["ssm psr.ic ;;", "srlz.d ;;"]
        op = "probe.r.fault r2,3" if kind == 4 else "st8 [r2]=r0" if kind == 5 else "ld8 r4=[r2]"
        lines += [op + " ;;"]
        return Item(lines, tag="mmu fault")

    DATA_OPS = (
        [
            "ld8 r4=[r2]",
            "ld4 r4=[r2]",
            "ld1 r4=[r2]",
            "ld8.acq r4=[r2]",
            "ld8.bias r4=[r2]",
            "ld8.a r4=[r2]",
            "ld8.s r4=[r2]",
            "ld8.sa r4=[r2]",
        ],
        [
            "st8 [r2]=r0",
            "st8 [r2]=r3",
            "st4 [r2]=r3",
            "st1 [r2]=r3",
            "st8.rel [r2]=r0",
            "st8.spill [r2]=r3",
        ],
        ["xchg8 r4=[r2],r0", "fetchadd8.acq r4=[r2],1", "cmpxchg8.acq r4=[r2],r0,ar.ccv"],
        [
            "ld16 r4,ar.csd=[r2]",
            "st16 [r2]=r3,ar.csd",
            "cmp8xchg16.acq r4=[r2],r0,ar.csd,ar.ccv",
            "fetchadd4.rel r4=[r2],-1",
            "xchg1 r4=[r2],r3",
        ],
        [
            "lfetch [r2]",
            "lfetch.fault [r2]",
            "lfetch.fault.excl [r2]",
            "probe.r.fault r2,{pl}",
            "probe.w.fault r2,{pl}",
            "probe.rw.fault r2,{pl}",
            "probe.r r4=r2,{pl}",
            "probe.w r4=r2,{pl}",
        ],
    )

    def mmu_protection_tail(self):
        """One reference to a page with random P/A/D/AR/PL/ED bits and a
        random protection key setup, ending the program or completing.
        Fault order: SDM Vol.2 §4.1.8 and Table 5-6."""
        rng = self.rng
        fetch = rng.random() < 0.3
        pte = (rng.random() < 0.97) | (rng.random() < 0.55) << 5 | (rng.random() < 0.5) << 6
        pte |= (0 if rng.random() < 0.8 else rng.randrange(4)) << 7
        pte |= rng.choice([3, 3, 3, 2, 1, 0, 4, 5, 6, 7]) << 9 | (rng.random() < 0.2) << 52
        key = rng.randrange(0x200, 0x10000)
        keymode = rng.choice(["rights"] * 5 + ["absent", "other", "invalid"])
        rights = rng.getrandbits(3) if rng.random() < 0.7 else 0
        if rng.random() < 0.4:
            # Only the A/D bits can fault: key present without disables,
            # present page, full rights at PL0.
            pte = pte & 0x60 | 1 | 3 << 9
            keymode, rights = "rights", 0
        op = rng.choice(rng.choice(self.DATA_OPS)).format(pl=rng.randrange(4))
        # A speculative load keeps checking keys and rights after a deferred
        # NaT Page (Vol.2 Table 5-3).
        if (
            not fetch
            and rng.random() < 0.08
            and (self.known or not op.split()[0].endswith((".s", ".sa")))
        ):
            pte |= 7 << 2  # NaTPage
        elif not fetch and rng.random() < 0.15:
            # UC, UCE, WC: semaphores and 16-byte references take Unsupported
            # Data Reference, after every other data fault (Vol.2 §4.4.9).
            pte |= rng.choice([4, 5, 6]) << 2
        alias = (1 << 61) | 0x600000
        lines = [
            "rsm psr.ic ;;",
            "srlz.d ;;",
            "rsm psr.pk ;;",
            "srlz.d ;;",
            f"movl r2=0x{alias:x} ;;",
            "mov cr.ifa=r2 ;;",
            f"movl r3={key << 8 | 12 << 2} ;;",
            "mov cr.itir=r3 ;;",
        ]
        if keymode != "absent":
            pkr = {
                "rights": key << 8 | 1 | rights << 1,
                "invalid": key << 8 | rights << 1,
                "other": (key ^ 0x155) << 8 | 1 | rights << 1,
            }[keymode]
            lines += ["mov r3=2 ;;", f"movl r4=0x{pkr:x} ;;", "mov pkr[r3]=r4 ;;", "srlz.d ;;"]
        if fetch:
            lines += [f"movl r3=0x{pte:x} + {{L}}p ;;", "itc.i r3 ;;", "srlz.i ;;"]
        else:
            lines += [f"movl r3=0x{SCRATCH | pte:x} ;;", "itc.d r3 ;;", "srlz.d ;;"]
        if rng.random() < 0.85:
            lines += ["ssm psr.pk ;;", "srlz.d ;;"]
        if not fetch:
            if "ld8.s" in op:
                # --known exercises independent deferral bits DM..DD (Table 5-4).
                mask = rng.getrandbits(7) if self.known else rng.choice([0, 0x7F])
                lines += [
                    "mov r5=cr.dcr ;;",
                    f"movl r6=0x{~(0x7F << 8) & MASK64:x} ;;",
                    "and r5=r5,r6 ;;",
                    f"movl r6=0x{mask << 8:x} ;;",
                    "or r5=r5,r6 ;;",
                    "mov cr.dcr=r5 ;;",
                    "srlz.d ;;",
                ]
            offset = 8 * rng.randrange(NAT_AREA)
            if offset:
                lines.append(f"adds r2=0x{offset:x},r2 ;;")
        # Either enable interruption collection directly, or rfi with the
        # one-instruction suppression bits of IPSR (PSR.da/ed for the data
        # reference, PSR.ia for the fetch; SDM Vol.2 §5.5.4) set.
        if rng.random() < 0.4:
            bits = 1 << 13 | self.bank << 44  # mov r=psr does not return PSR.bn
            if fetch:
                # PSR.ia suppresses the access-bit fault for one instruction.
                bits |= (self.known and rng.random() < 0.5) << 45
            else:
                bits |= (rng.random() < 0.5) << 38 | (rng.random() < 0.3) << 43
            # rfi with PSR.ic = 0 does not update IIPA (Vol.2 §3.3.5.6), so
            # the written value is what the first fault reports.
            lines += [
                f"movl r6=0x{ENTRY + 16 * rng.randrange(0x800):x} ;;",
                "mov cr.iipa=r6 ;;",
                "mov r5=psr ;;",
                f"movl r6=0x{bits:x} ;;",
                "or r5=r5,r6 ;;",
                "mov cr.ipsr=r5 ;;",
                "mov r6=r2 ;;" if fetch else "movl r6={L}r ;;",
                "mov cr.iip=r6 ;;",
                "mov cr.ifs=r0 ;;",
                "rfi ;;",
            ]
            if not fetch:
                lines += [".align 16", "{L}r:"]
        else:
            lines += ["ssm psr.ic ;;", "srlz.d ;;"]
            if fetch:
                lines += ["mov b6=r2 ;;", "br.cond.sptk.few b6 ;;"]
        if fetch:
            lines += [
                "br.cond.sptk.few {L}c ;;",
                ".align 4096",
                "{L}p:",
                "movl r6={L}c ;;",
                "mov b6=r6 ;;",
                "br.cond.sptk.few b6 ;;",
                ".align 16",
                "{L}c:",
            ]
            return Item(lines, tag="mmu protection")
        lines.append(op + " ;;")
        return Item(lines, tag="mmu protection")

    def call_item(self, max_depth):
        """br.call into a subroutine with its own frame; its body may call
        deeper.  Inputs are exactly the caller's outputs."""
        rng = self.rng
        caller, caller_taint = self.frame, set(self.taint)
        inputs = len(caller.outs)
        i, l, o, r = self.frame_shape(inputs, 2, 2)
        sof = i + l + o
        rp, rb = 32 + i + l - 2, 32 + i + l - 1
        # Callee view of the caller's outputs.
        self.taint = {t for t in self.taint if t < 32}
        for k, out in enumerate(caller.outs):
            if out in caller_taint:
                self.taint.add(32 + k)
        self.frame = Frame(sof, i + l, r // 8, reserved=(rp, rb))
        self.depth += 1
        fresh = [reg for reg in range(32 + i, 32 + sof) if reg not in (rp, rb)]
        prefix = ["{L}s:", f"alloc r{rp}=ar.pfs,{i},{l},{o},{r} ;;", f"mov r{rb}=b0 ;;"]
        prefix += self.init_frame(fresh)
        inner = self.body_items(rng.randrange(2, 8), max_depth)
        suffix = [f"mov ar.pfs=r{rp} ;;", f"mov b0=r{rb} ;;", "br.ret.sptk.many b0 ;;"]
        callee_taint = self.taint
        self.depth -= 1
        self.frame = caller
        self.taint = {t for t in callee_taint if t < 32}
        self.taint |= {t for t in caller_taint if t >= 32 and t not in caller.outs}
        for k, out in enumerate(caller.outs):
            if 32 + k in callee_taint:
                self.taint.add(out)
        if rng.random() < 0.3:
            t, b = rng.randrange(1, 32), rng.randrange(1, 8)
            self.taint.discard(t)
            head = [f"movl r{t}={{L}}s ;;", f"mov b{b}=r{t} ;;", f"br.call.sptk.many b0=b{b} ;;"]
        else:
            head = ["br.call.sptk.many b0={L}s ;;"]
        return Item(head, inner, [], tag="call", sub=(prefix, suffix))

    def cover_item(self, loadrs=False):
        """cover at depth 0, then a fresh main frame (fully initialized).
        With loadrs: flushrs; cover; reload N bytes of the covered frames
        (a nonzero RSC.loadrs needs CFM.sof == 0, SDM 2.3 Vol.3 loadrs);
        RNAT, undefined afterwards, is rewritten all-ones in the same bundle."""
        rng = self.rng
        self.covered += self.frame.sof
        lines = ["cover ;;"]
        if loadrs and self.covered:
            n = 8 * rng.randrange(1, min(self.covered, 90) + 1)
            # loadrs invalidates registers below BSP-N without storing them
            # (SDM 2.3 Vol.3 loadrs), so older backing-store words may never
            # have been written: the comparison base moves to the new BSPSTORE.
            lines = (
                [
                    "flushrs ;;",
                    "cover ;;",
                    f"movl r2=0x{n << 16 | self.rsc_be:x} ;;",
                    "mov ar.rsc=r2 ;;",
                    "movl r3=-1 ;;",
                    ".align 16",
                    "{ .mmi",
                    "loadrs ;;",
                    "mov ar.rnat=r3",
                    "nop.i 0 ;; }",
                    f"mov ar.rsc={self.rsc_be} ;;",
                    "mov r3=ar.bspstore ;;",
                    f"movl r2=0x{BS_BASE_PTR:x} ;;",
                ]
                + self.order()
                + ["st8 [r2]=r3 ;;"]
            )
            self.taint.discard(3)
        i, l, o, r = self.frame_shape(0, 0)
        self.taint = {t for t in self.taint if t < 32}
        self.frame = Frame(l + o, l, r // 8)
        lines.append(f"alloc r2=ar.pfs,0,{l},{o},{r} ;;")
        self.taint.discard(2)
        lines += self.init_frame(self.frame.regs)
        return Item(lines, tag="rse")

    def rse_item(self):
        """flushrs, then BSP/BSPSTORE, the defined RNAT bits and a word below
        BSP (NaT-masked); or flushrs + loadrs(0) with RNAT saved/restored
        around it (loadrs leaves RNAT undefined, SDM 2.3 Vol.3 loadrs)."""
        rng = self.rng
        t = rng.sample(range(3, 32), 5)
        self.taint -= set(t)
        a, b, c, d, e = t
        lines = ["flushrs ;;", f"mov r{a}=ar.bspstore ;;", f"mov r{b}=ar.bsp ;;"]
        lines += rnat_mask(e, d, a) + rnat_read(c, e)
        if rng.random() < 0.6:
            if rng.random() < 0.5:
                scratch = rng.sample([x for x in range(1, 32) if x not in t], 3)
                self.taint -= set(scratch)
                lines += [f"adds r{d}={-8 * rng.randrange(1, 40)},r{b} ;;"]
                lines += self.order() + masked_bs_load(e, d, a, c, scratch)
        else:
            lines += [
                f"mov ar.rsc={self.rsc_be} ;;",
                ".align 16",
                "{ .mmi",
                "loadrs ;;",
                f"mov ar.rnat=r{c}",
                "nop.i 0 ;; }",
                f"mov r{a}=ar.bspstore ;;",
            ]
        return Item(lines, tag="rse")

    def bsw_item(self):
        """Switch register banks (r16-r31); the other bank's taint is kept."""
        other = self.bank_taint
        self.bank_taint = {t for t in self.taint if 16 <= t < 32}
        self.taint = {t for t in self.taint if not 16 <= t < 32} | other
        self.bank ^= 1
        return Item([f"bsw.{self.bank} ;;"], tag="bsw")

    def body_items(self, n, max_depth):
        """Top-level (or subroutine) body: straight code, branches, loops,
        calls, RSE operations, bank switches and resumable faults."""
        rng = self.rng
        items = []
        while len(items) < n:
            r = rng.random()
            if r < 0.06:
                items.append(self.skip())
            elif r < 0.12:
                items.append(self.loop())
            elif r < 0.17 and self.depth < max_depth:
                items.append(self.call_item(max_depth))
            elif r < 0.20:
                items.append(self.rse_item())
            elif r < 0.21 and self.depth == 0:
                items.append(self.cover_item(rng.random() < 0.5))
            elif r < 0.22:
                items.append(self.bsw_item())
            elif r < 0.23:
                hint = rng.choice(["sptk", "loop", "exit", "dptk"])
                items.append(Item([f"brp.{hint}.imp {{L}}, {{L}} ;;"], tail=["{L}:"], tag="brp"))
            elif r < 0.24 and self.resume & {0x5600, 0x5400, 0x2C00}:
                items.append(self.fault_tail(self.resume))
            else:
                items.extend(self.straight(1))
        return items

    FAULT_KINDS = {
        0: 0x5600,
        1: 0x5600,
        2: 0x5600,
        3: 0x5600,
        4: 0x5600,
        7: 0x5600,
        5: 0x5400,
        6: 0x5400,
        8: 0x5400,
        9: 0x5400,
        10: 0x5400,
        11: 0x5400,
        13: 0x5400,
        12: 0x2C00,
        14: 0x5400,
    }

    def fault_tail(self, vectors=None):
        """One instruction that must fault (or not) identically on both
        sides; `vectors` restricts it to resumable vectors (mid-body)."""
        rng = self.rng
        r = rng.randrange(1, 32)
        a = rng.choice([x for x in range(1, 32) if x != r])
        # Make r definitely NaT (ld8.fill with its UNAT bit set).
        spill = SCRATCH + 8 * rng.randrange(NWORDS)
        nat = [
            f"movl r{a}=0x{spill:x} ;;",
            f"movl r{r}=0x{1 << ((spill >> 3) & 63):x} ;;",
            f"mov ar.unat=r{r} ;;",
            f"ld8.fill r{r}=[r{a}] ;;",
        ]
        kinds = [k for k, v in self.FAULT_KINDS.items() if vectors is None or v in vectors]
        if not self.known:
            # Keep reserved-register and resumed-template cases opt-in.
            kinds = [k for k in kinds if k not in (9, 14)]
            if 0x5400 in self.resume:
                kinds = [k for k in kinds if k != 13] or [5]
        k = rng.choice(kinds)
        if k in (0, 1, 2, 3, 4, 7):
            self.taint.add(r)
        self.taint.discard(a)
        if k == 12:  # non-terminal break (IIM: zero for break.b)
            unit = rng.choice(
                ["m", "i", "b", "f"] + (["x"] if self.known or 0x2C00 not in self.resume else [])
            )
            imm = rng.getrandbits(62) if unit == "x" else rng.randrange(1 << 21)
            # 0x1ffffe ends the run; 0x100000-0x100004 are PAL/FPSWA/debug
            # bridge immediates that either side may intercept.
            while imm & 0x1FFFFF == END_IMM or 0x100000 <= imm & 0x1FFFFF <= 0x100004:
                imm ^= 1 << rng.randrange(21)
            lines = [f"break.{unit} 0x{imm:x} ;;"]
        elif k == 13:  # reserved template / raw bundle: Illegal Operation
            tmpl = rng.choice([0x06, 0x07, 0x14, 0x15, 0x1E, 0x1F])
            lines = [".align 16", f"data8 0x{0x200 | tmpl:x}, 0x0004000000000200"]
        elif k == 14:  # mov r=dahr[r]: Itanium 9500 only, Reserved if PR[qp]
            lines = [f"mov r{a}=dahr[r{rng.randrange(32)}] ;;"]
        elif k == 9:  # reserved application registers
            ar = rng.choice([8, 15, 20, 22, 31, 33, 37, 41, 45, 47, 67, 80, 100, 111])
            lines = [f"mov r{a}=ar{ar} ;;" if rng.random() < 0.5 else f"mov ar{ar}=r{a} ;;"]
        elif k == 10:  # reserved RSC field
            v = rng.getrandbits(5) | 1 << rng.choice(list(range(5, 16)) + list(range(30, 64)))
            lines = [f"movl r{a}=0x{v:x} ;;", f"mov ar.rsc=r{a} ;;"]
        elif k == 11:  # reserved FPSR field
            v = self.fpsr_value()
            v = rng.choice(
                [
                    v | 1 << rng.randrange(58, 64),
                    v | 1 << 12,
                    (v & ~(3 << 8)) | 1 << 8,
                    (v & ~(3 << 21)) | 1 << 21,
                    (v & ~(3 << 34)) | 1 << 34,
                    (v & ~(3 << 47)) | 1 << 47,
                ]
            )
            lines = [f"movl r{a}=0x{v:x} ;;", f"mov ar.fpsr=r{a} ;;"]
        elif k == 0:
            lines = nat + [f"movl r{a}=0x{SCRATCH:x} ;;", f"st8 [r{a}]=r{r} ;;"]
        elif k == 1:
            lines = nat + [f"mov b{rng.randrange(8)}=r{r} ;;"]
        elif k == 2:
            lines = nat + [f"mov pr=r{r},0x{rng.getrandbits(17):x} ;;"]
        elif k == 3:
            lines = nat + [f"mov ar.{rng.choice(['ccv', 'unat', 'lc', 'ec', 'k2'])}=r{r} ;;"]
        elif k == 4:
            lines = nat + [f"ld8 r{rng.randrange(1, 32)}=[r{r}] ;;"]
        elif k == 5:
            p = rng.randrange(1, 64)
            lines = [f"cmp.eq.unc p{p},p{p}=r{rng.randrange(32)},r{rng.randrange(32)} ;;"]
        elif k == 6:
            lines = [f"add r0=r{rng.randrange(32)},r{rng.randrange(32)} ;;"]
        elif k == 7:
            lines = nat + [
                f"movl r{a}=0x{SCRATCH:x} ;;",
                f"xchg8 r{rng.randrange(1, 32)}=[r{a}],r{r} ;;",
            ]
        else:
            lines = [f"mov ar.bsp=r{rng.randrange(1, 32)} ;;"]
        return Item(lines, tag="fault")

    def program(self, fault_tail=0.0):
        rng = self.rng
        setup = self.setup()
        ivt = {v: kind for v, kind in RESUMABLE.items() if rng.random() < 0.6}
        self.resume = set(ivt)
        pro = self.prologue()
        body = self.body_items(self.length, rng.choice([2, 4, 6, 10]))
        if rng.random() < fault_tail:
            body.append(self.fault_tail())
        if self.virtual:
            body.append(
                self.mmu_protection_tail() if self.rng.random() < 0.7 else self.mmu_fault_tail()
            )
        return Program(
            self.seed,
            pro,
            body,
            [self.value() for _ in range(NWORDS)],
            setup,
            ivt,
            epilogue(self.order()),
        )


def terminate(lines):
    """Append a stop to every instruction line, except labels, directives
    and lines inside explicit `{ ... }` bundles."""
    out, depth = [], 0
    for l in lines:
        if l.startswith("{"):
            depth += 1
        if depth or l.endswith((";;", ":")) or l.startswith("."):
            out.append(l)
        else:
            out.append(l + " ;;")
        if l.endswith("}"):
            depth -= 1
    return out


def mid_label(item):
    return f".Lf{item.uid}"


REG_RE = re.compile(r"\b([rpb])(\d+)\b")


def resources(line):
    """(written, read) register sets of one ALU/compare line, qp included."""
    m = re.match(r"\s*(?:\(p(\d+)\)\s*)?(\S+)\s+(.*?)=(.*)$", line)
    qp, _, dst, src = m.groups()
    written = {f"{k}{n}" for k, n in REG_RE.findall(dst) if not (k == "p" and n == "0")}
    read = {f"{k}{n}" for k, n in REG_RE.findall(src)}
    if qp:
        read.add(f"p{qp}")
    return written, read


# --------------------------------------------------------------------------
# Build and run


class Tools:
    def __init__(self, env, runner, workdir, runner_args=()):
        self.cross = env["CROSS_IA64"]
        self.qemu = env["QEMU_IA64"]
        self.runner = str(runner)
        self.runner_args = list(runner_args)
        self.workdir = Path(workdir)
        self.workdir.mkdir(parents=True, exist_ok=True)

    def build(self, program, name):
        self.workdir.mkdir(parents=True, exist_ok=True)
        src = self.workdir / f"{name}.S"
        obj, elf, image = (src.with_suffix(s) for s in (".o", ".elf", ".bin"))
        src.write_text(program.text())
        r = subprocess.run(
            [self.cross + "as", "-xexplicit", str(src), "-o", str(obj)],
            capture_output=True,
            text=True,
        )
        # The fault tail deliberately uses r0 as a target and p1 == p2.
        intended = ("Invalid use of `r0' as output operand", "Invalid duplicate use of `p")
        warn = [
            l
            for l in r.stderr.splitlines()
            if ("arning" in l or "rror" in l) and not any(x in l for x in intended)
        ]
        if r.returncode or warn:
            raise AsmError("\n".join(warn[:10]) or r.stderr)
        r = subprocess.run(
            [
                self.cross + "ld",
                f"-Ttext=0x{ENTRY:x}",
                f"-Tdata=0x{SCRATCH:x}",
                f"--section-start=.ivt=0x{IVA:x}",
                "-e",
                "_start",
                str(obj),
                "-o",
                str(elf),
            ],
            capture_output=True,
            text=True,
        )
        if r.returncode:
            raise AsmError(r.stderr)
        subprocess.run(
            [self.cross + "objcopy", "-O", "binary", str(elf), str(image)],
            check=True,
            capture_output=True,
        )
        nm = subprocess.run(
            [self.cross + "nm", str(elf)], check=True, capture_output=True, text=True
        ).stdout
        m = re.search(r"^([0-9a-f]+) \w fuzz_checkpoint$", nm, re.M)
        checkpoint = int(m[1], 16) if m else 0  # --run files may lack the label
        return src, elf, image, checkpoint

    def disasm(self, elf):
        """bundle address -> list of (slot, instruction text), nops included."""
        out = subprocess.run(
            [self.cross + "objdump", "-d", str(elf)], check=True, capture_output=True, text=True
        ).stdout
        bundles, cur = {}, None
        for line in out.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\t(?:[0-9a-f]{2} )+\s*\t(.*)$", line)
            if not m:
                continue
            addr, text = int(m[1], 16), m[2].strip()
            if text.startswith("["):
                cur = addr & ~15
                text = text.split("]", 1)[1].strip()
            if not text or cur is None:
                continue
            bundles.setdefault(cur, []).append(text.rstrip(";").strip())
        out = {}
        for addr, insns in bundles.items():
            slots = [0, 1] if len(insns) == 2 else [0, 1, 2]  # MLX: L+X is slot 1
            out[addr] = list(zip(slots, insns))
        return out

    def run_interp(self, image, checkpoint, budget=50000):
        r = subprocess.run(
            [
                self.runner,
                *self.runner_args,
                "--trace",
                "--checkpoint",
                hex(checkpoint),
                "--budget",
                str(budget),
                str(image),
            ],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if r.returncode:
            raise RuntimeError(f"runner failed: {r.stderr[-2000:]}")
        trace, checkpoint_state, final, exit_info = [], None, None, {}
        state, tag, ip = None, None, None
        for line in r.stdout.splitlines():
            if line.startswith("GR "):
                _, i, v, n = line.split()
                state[f"r{i}"] = int(v, 16)
                state[f"r{i}.nat"] = int(n)
            elif line.startswith("STATE "):
                parts = line.split()
                tag, ip, state = parts[1], int(parts[2][3:], 16), {}
            elif line.startswith("FR "):
                _, i, lo, hi, n = line.split()
                state[f"f{i}"] = int(hi, 16) << 64 | int(lo, 16)
                state[f"f{i}.nat"] = int(n)
            elif line.startswith("CR "):
                _, n, v = line.split()
                state[f"cr.{n}"] = int(v, 16)
            elif line.startswith("PSR "):
                state["psr"] = int(line.split()[1], 16)
            elif line.startswith("BR "):
                _, i, v = line.split()
                state[f"b{i}"] = int(v, 16)
            elif line.startswith("PR "):
                state["pr"] = int(line.split()[1], 16)
            elif line.startswith("AR "):
                _, n, v = line.split()
                state[f"ar.{n}"] = int(v, 16)
            elif line.startswith("CFM "):
                _, n, v = line.split()
                state[f"cfm.{n}"] = int(v, 16)
            elif line == "ENDSTATE":
                if tag == "bundle":
                    trace.append((ip, state))
                elif tag == "checkpoint":
                    checkpoint_state = state
                else:
                    final = state
            elif line.startswith("EXIT "):
                exit_info = dict(re.findall(r"(\w+)=((?:(?! \w+=).)*)", line[5:]))
            elif line.startswith("FETCH_FAULT "):
                address = int(line.split("=", 1)[1], 16)
                if trace and trace[-1][0] == address:
                    trace.pop()
        kind = exit_info.get("kind")
        if kind == "end":
            term = {"kind": "end", "ip": int(exit_info["ip"], 16), "slot": int(exit_info["slot"])}
        elif kind == "fault":
            term = {
                "kind": "fault",
                "fault": exit_info["fault"],
                "ip": int(exit_info["ip"], 16),
                "slot": int(exit_info["slot"]),
                "info": int(exit_info["info"], 16),
            }
        else:
            term = {
                "kind": kind or "?",
                "detail": exit_info.get("detail", ""),
                "reason": exit_info.get("reason"),
                "ip": int(exit_info.get("ip", "0"), 16),
                "slot": int(exit_info.get("slot", "0")),
            }
        return {"trace": trace, "checkpoint": checkpoint_state, "final": final, "term": term}

    def run_fork(self, image, checkpoint, max_blocks=8000, timeout=90):
        cmd = [
            self.qemu,
            "-machine",
            "itanium2-vpc,nvram=none",
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
            "-nodefaults",
            "-d",
            "cpu,fpu,nochain",
            "-accel",
            "tcg,one-insn-per-tb=on",
            "-D",
            "/dev/stdout",
        ]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        blocks, cur, pending = [], None, b""
        deadline = time.monotonic() + timeout
        terminal, stuck = None, False
        try:
            while (
                time.monotonic() < deadline
                and terminal is None
                and not stuck
                and len(blocks) < max_blocks
            ):
                if not select.select([proc.stdout], [], [], 0.2)[0]:
                    if proc.poll() is not None:
                        break
                    continue
                chunk = os.read(proc.stdout.fileno(), 1 << 20)
                if not chunk:
                    break
                pending += chunk
                *lines, pending = pending.split(b"\n")
                for raw in lines:
                    if raw.startswith(b"RSE-PGR") or raw.startswith(b"r") or raw.startswith(b"p"):
                        continue  # human-readable duplicates of IA64STATE
                    line = raw.decode("ascii", "replace")
                    if line.startswith("IP: "):
                        if cur is not None and cur.get("cfm"):
                            finish_fork_block(cur)
                            blocks.append(cur)
                        cur = {"state": {}, "ip": int(line.split()[1], 16)}
                        continue
                    if cur is None:
                        continue
                    if line.startswith("IA64STATE "):
                        parse_fork_line(line, cur)
                        if line.startswith("IA64STATE CFM"):
                            cur["cfm"] = True
                            # The terminal break, delivered to IVA+0x2c00 with
                            # IIM 0x1ffffe; the interpreter stops it in its
                            # break hook, so the last compared state is the
                            # entry of the bundle holding that break.
                            st = cur["state"]
                            if (
                                cur["ip"] == (st.get("cr.iva", -1) & ~0x7FFF) + 0x2C00
                                and cur["exc"]["fault_imm"] == END_IMM
                            ):
                                blocks.append(cur)
                                terminal = cur
                                break
                            if len(blocks) > 64 and all(x["ip"] == cur["ip"] for x in blocks[-64:]):
                                stuck = True
                                break
                        elif line.startswith("IA64STATE FR index=07f"):
                            # FRs (-d fpu) follow the CFM line; the last one ends the block.
                            finish_fork_block(cur)
                            blocks.append(cur)
                            cur = None
                    elif line.startswith("AR.ITC:"):
                        m = re.search(r"AR\.LC: 0x([0-9a-f]+) AR\.EC: 0x([0-9a-f]+)", line)
                        cur["state"]["ar.lc"], cur["state"]["ar.ec"] = int(m[1], 16), int(m[2], 16)
                    elif line.startswith("CFM: "):
                        m = re.search(r"BSP=0x([0-9a-f]+) BSPSTORE=0x([0-9a-f]+)", line)
                        cur["state"]["ar.bsp"], cur["state"]["ar.bspstore"] = (
                            int(m[1], 16),
                            int(m[2], 16),
                        )
                    elif line.startswith(("IIP: ", "ISR: ", "IVA: ", "IHA: ", "CR.DCR: ")):
                        for k, v in re.findall(r"(?:CR\.)?([A-Z0-9]+): (?:0x)?([0-9a-f]+)", line):
                            if k in FORK_CRS:
                                cur["state"][f"cr.{k.lower()}"] = int(v, 16)
        finally:
            proc.kill()
            proc.wait()
            err = proc.stderr.read().decode(errors="replace")
            proc.stdout.close()
            proc.stderr.close()
        if terminal is None:
            reason = (
                "block limit"
                if len(blocks) >= max_blocks
                else f"stuck at {blocks[-1]['ip']:#x}"
                if stuck
                else "timeout/exit"
            )
            return {
                "trace": [(b["ip"], b["state"]) for b in blocks],
                "checkpoint": None,
                "final": blocks[-1]["state"] if blocks else None,
                "term": {"kind": "stop", "detail": f"fork: {reason} {err.strip()[:200]}"},
            }
        trace = [(b["ip"], b["state"]) for b in blocks[:-1]]
        cp = next((s for ip, s in trace if ip == checkpoint), None)
        exc = terminal["exc"]
        term = {
            "kind": "end",
            "ip": exc["fault_ip"],
            "slot": (terminal["state"].get("cr.isr", 0) >> 41) & 3,
        }
        return {"trace": trace, "checkpoint": cp, "final": None, "term": term}


FORK_CRS = {
    "IIP",
    "IFA",
    "IPSR",
    "ISR",
    "IFS",
    "IIM",
    "IVA",
    "IIPA",
    "ITIR",
    "IHA",
    "PTA",
    "DCR",
    "ITV",
    "TPR",
}


def finish_fork_block(cur):
    """Drop stacked GRs outside the current frame (not architecturally
    visible; the fork's env->gr[32+] only caches the current frame)."""
    st = cur["state"]
    sof = st.get("cfm.sof", 0)
    for i in range(32 + sof, 128):
        st.pop(f"r{i}", None)
        st.pop(f"r{i}.nat", None)


def parse_fork_line(line, cur):
    s = cur["state"]
    parts = line.split()
    kind = parts[1]
    fields = dict(p.split("=", 1) for p in parts[2:])
    if kind == "GR":
        i = int(fields["index"], 16)
        s[f"r{i}"] = int(fields["value"], 16)
        s[f"r{i}.nat"] = int(fields["nat"])
    elif kind == "BR":
        s[f"b{int(fields['index'], 16)}"] = int(fields["value"], 16)
    elif kind == "PR":
        s["pr"] = int(fields["value"], 16)
    elif kind == "AR":
        for k in ("ccv", "unat", "fpsr", "csd", "ssd", "rsc", "rnat", "pfs"):
            s[f"ar.{k}"] = int(fields[k], 16)
    elif kind == "FR":
        i = int(fields["index"], 16)
        s[f"f{i}"] = int(fields["high"], 16) << 64 | int(fields["low"], 16)
        s[f"f{i}.nat"] = int(fields["nat"])
    elif kind == "META":
        s["psr"] = int(fields["psr"], 16)
    elif kind == "CFM":
        for k, v in fields.items():
            s[f"cfm.{k}"] = int(v, 16)
    elif kind == "EXCEPTION":
        cur["exc"] = {k: int(v, 16) for k, v in fields.items()}


class AsmError(Exception):
    pass


# --------------------------------------------------------------------------
# Comparison

FAULT_VECTORS = {
    "Register NaT Consumption": 0x5600,
    "Illegal Operation": 0x5400,
    "Reserved Register/Field": 0x5400,
    "Privileged Register": 0x5400,
    "Break Instruction": 0x2C00,
    "Unaligned Data Reference": 0x5A00,
    "Unsupported Data Reference": 0x5B00,
}
VECTOR_NAMES = {
    0x5600: "NaT consumption",
    0x5400: "general exception",
    0x2C00: "break",
    0x5A00: "unaligned",
    0x5B00: "unsupported data",
    0x5300: "speculation",
    0x5500: "disabled FP",
}


# PSR.ri is the current slot, which both models keep separately (and the
# interpreter only materializes it in IPSR); it is not observable otherwise.
PSR_MASK = ~(3 << 41) & MASK64


def state_diff(a, b, ignore=()):
    keys = (set(a) & set(b)) - set(ignore)
    out = []
    # SDM Vol.2 §6.8: the unloaded prefix of an incomplete current frame
    # has no architectural value yet. Keep comparing its loaded suffix.
    missing = 0
    bsp, store = a.get("ar.bsp", 0), a.get("ar.bspstore", 0)
    if store > bsp and store - bsp <= 96 * 8 + 16:
        missing = sum((addr >> 3) & 63 != 63 for addr in range(bsp, store, 8))
    for k in sorted(keys, key=sort_key):
        gr = re.fullmatch(r"r(\d+)(?:\.nat)?", k)
        if gr and int(gr[1]) >= 32:
            offset = int(gr[1]) - 32
            sor = a.get("cfm.sor", 0) * 8
            if offset < sor:
                offset = (offset + a.get("cfm.rrb_gr", 0)) % sor
            if offset < missing:
                continue
        if k == "psr":
            if (a[k] ^ b[k]) & PSR_MASK:
                out.append((k, a[k], b[k]))
        elif a[k] != b[k]:
            # A NaT'd GR's data is implementation-specific (SDM 2.3 Vol.1
            # §4.4.4.1); compare the NaT bits only.
            if k[0] == "r" and a.get(k + ".nat") == b.get(k + ".nat") == 1:
                continue
            out.append((k, a[k], b[k]))
    if "ar.rnat" in keys:
        # Outside enforced-lazy mode the RSE may load/store (and update
        # RNAT) at any time.  RNAT{62:BSPSTORE{8:3}+1} is undefined
        # (SDM 2.3 Vol.2 §6.5.2); compare only the defined bits.
        index = (a.get("ar.bspstore", 0) >> 3) & 63
        mask = (1 << index) - 1  # see rnat_mask()
        if (a.get("ar.rsc", 0) | b.get("ar.rsc", 0)) & 3 or not (
            a["ar.rnat"] ^ b["ar.rnat"]
        ) & mask:
            out = [d for d in out if d[0] != "ar.rnat"]
    return out


def sort_key(k):
    m = re.match(r"([a-z.]+?)(\d+)?(\.nat)?$", k)
    return (m[1], int(m[2]) if m[2] else -1, m[3] or "") if m else (k, 0, "")


def collapse(trace):
    """Keep the first of consecutive entries with the same bundle address.

    The fork may log several blocks per bundle (it splits translation
    blocks at some slots, e.g. after mid-bundle stops) and its dump has no
    slot field; the first block at a new address is the slot-0 entry.
    Applied to both sides, so one-bundle loops are treated alike."""
    out = []
    for ip, state in trace:
        if not out or out[-1][0] != ip:
            out.append((ip, state))
    return out


# SDM 2.3 Vol.2 Table 8-1 (PSR.ic=1): IIP/IPSR/IIPA/IFS and ISR are always
# written. Translation vectors define ITIR and, for walker/miss vectors, IHA.
# An "x" resource stays unspecified
# until a later interruption writes it.
VECTOR_SET = set(IVT_VECTORS)


REQUIRED_KEYS = [
    "r1",
    "r1.nat",
    "f2",
    "f2.nat",
    "f127",
    "b0",
    "pr",
    "ar.lc",
    "ar.ec",
    "ar.bsp",
    "ar.bspstore",
    "ar.rnat",
    "ar.fpsr",
    "ar.unat",
    "ar.csd",
    "cr.iip",
    "cr.ipsr",
    "cr.isr",
    "cr.ifa",
    "cr.dcr",
    "psr",
    "cfm.sof",
    "cfm.rrb_pr",
]


def delivery_undefined(vector, undef, isr=0):
    undef = set(undef)
    translations = (
        0,
        0x400,
        0x800,
        0xC00,
        0x1000,
        0x1800,
        0x1C00,
        0x2000,
        0x2400,
        0x2800,
        0x5000,
        0x5100,
        0x5200,
        0x5300,
    )
    if vector == 0x1400:  # Nested TLB preserves all interruption resources.
        return undef
    for res, writers in (
        ("cr.itir", translations),
        ("cr.iha", (0, 0x400, 0x800)),
        (
            "cr.ifa",
            translations
            + (0x5900, 0x5A00, 0x5B00)
            + ((0x5E00,) if isr & 0xFFFF == 0x20 else ())
            + ((0x5600,) if isr & 0xF0 == 0x20 else ()),
        ),
        ("cr.iim", (0x2C00,)),
    ):
        if vector in writers:
            undef.discard(res)
        else:
            undef.add(res)
    return undef


def compare(ires, fres):
    """Return None when equivalent, else a divergence description.

    Both runs end at `break 0x1ffffe` in slot 0 of a bundle (program end or
    an IVT stub after interruption delivery). The full state at every bundle
    entry up to and including that bundle is compared."""
    it, ft = collapse(ires["trace"]), collapse(fres["trace"])
    iterm, fterm = ires["term"], fres["term"]
    if it and ft:
        missing = [k for k in REQUIRED_KEYS if k not in it[0][1] or k not in ft[0][1]]
        if missing:  # a parser regression must not silently shrink the compared state
            raise RuntimeError(f"compared state lacks {missing}")
    undef = set()
    for k in range(min(len(it), len(ft))):
        (iip, ist), (fip, fst) = it[k], ft[k]
        if iip != fip:
            return {
                "category": "control",
                "index": k,
                "diffs": [],
                "detail": f"bundle #{k}: interp ip {iip:#x}, fork ip {fip:#x}",
                "culprit": it[k - 1][0] if k else None,
            }
        vector = iip - (ist.get("cr.iva", 0) & ~0x7FFF)
        if vector in VECTOR_SET and k and it[k - 1][0] != iip:
            undef = delivery_undefined(vector, undef, ist.get("cr.isr", 0))
        diffs = state_diff(ist, fst, undef)
        if diffs:
            return {
                "category": "state",
                "index": k,
                "diffs": diffs,
                "at": iip,
                "culprit": it[k - 1][0] if k else None,
                "detail": f"state before bundle #{k} at {iip:#x}",
            }
    if iterm["kind"] == fterm["kind"] == "end" and len(it) == len(ft):
        return None
    idesc = {k: (f"{v:#x}" if k == "ip" else v) for k, v in iterm.items()}
    detail = f"bundles interp {len(it)} fork {len(ft)}; interp {idesc} / fork {fterm}"
    if iterm["kind"] != "end":
        # The interpreter stopped (unimplemented, unmapped access, budget)
        # where the fork went on: blame the stopping instruction.
        reason = re.sub(r"[^a-z0-9]+", "-", str(iterm.get("detail") or iterm["kind"]).lower())[:40]
        return {
            "category": f"interp-stop-{reason}".rstrip("-"),
            "index": len(it),
            "diffs": [],
            "culprit": iterm.get("ip"),
            "slot": iterm.get("slot"),
            "detail": detail,
        }
    if fterm["kind"] != "end":
        return {"category": "unresolved", "index": len(ft), "diffs": [], "detail": detail}
    k = min(len(it), len(ft))
    return {
        "category": "control",
        "index": k,
        "diffs": [],
        "detail": detail,
        "culprit": it[k - 1][0] if k else None,
    }


# --------------------------------------------------------------------------
# Execution of one program, minimization and reporting


class Fuzzer:
    def __init__(self, tools, args):
        self.tools = tools
        self.args = args
        self.counter = 0
        self.lock = threading.Lock()

    def name(self, prefix):
        with self.lock:
            self.counter += 1
            return f"{prefix}-{os.getpid()}-{self.counter}"

    def evaluate(self, program, name=None, keep=False):
        name = name or self.name(f"s{program.seed}")
        try:
            src, elf, image, checkpoint = self.tools.build(program, name)
        except AsmError as e:
            return {"error": "asm", "detail": str(e)}
        except Exception as e:  # e.g. the work directory vanished under us
            return {"error": "harness", "detail": f"{type(e).__name__}: {e}"}
        try:
            ires = self.tools.run_interp(image, checkpoint)
            # The fork cannot legitimately need many more bundles than the
            # interpreter; bounding it keeps interpreter-stop cases cheap.
            fres = self.tools.run_fork(
                image, checkpoint, max_blocks=min(8000, 2 * len(ires["trace"]) + 400)
            )
            div = compare(ires, fres)
            out = {"div": div, "ires": ires, "fres": fres, "elf": elf, "src": src}
            if div and div.get("culprit") is not None:
                insns = self.tools.disasm(elf).get(div["culprit"], [])
                slot = div.get("slot")
                pick = [t for sl, t in insns if sl == slot] if slot is not None else []
                out["culprit_insns"] = pick or [
                    t for _, t in insns if not re.match(r"(\(p\d+\)\s*)?nop\.", t)
                ]
            return out
        except Exception as e:  # harness failure: reported, never "interesting"
            return {"error": "harness", "detail": f"{type(e).__name__}: {e}"}
        finally:
            if not keep:
                for suffix in (".S", ".o", ".elf", ".bin"):
                    try:
                        (self.tools.workdir / f"{name}{suffix}").unlink()
                    except FileNotFoundError:
                        pass

    @staticmethod
    def signature(result):
        div = result["div"]
        return (
            div["category"],
            tuple(sorted({mnemonic(t) for t in result.get("culprit_insns", [])})),
        )

    def interesting(self, program, sig):
        r = self.evaluate(program)
        if "error" in r or not r["div"] or r["div"]["category"] == "unresolved":
            return False
        s = self.signature(r)
        return s[0] == sig[0] and (not sig[1] or set(s[1]) & set(sig[1]))

    def ddmin(self, items, make, sig, pool):
        """ddmin on a list; make(sublist) -> Program.  Candidates of one round
        are evaluated in parallel on the process pool."""

        def test_all(cands):
            return list(pool.map(eval_interesting, [(make(c), sig) for c in cands]))

        n = 2
        while len(items) >= 2:
            chunk = max(1, len(items) // n)
            subsets = [items[i : i + chunk] for i in range(0, len(items), chunk)]
            complements = [items[:i] + items[i + chunk :] for i in range(0, len(items), chunk)]
            cands = subsets + complements if n > 2 else subsets
            hit = next((i for i, ok in enumerate(test_all(cands)) if ok), None)
            if hit is not None:
                items = cands[hit]
                n = 2 if hit < len(subsets) else max(n - 1, 2)
                continue
            if n >= len(items):
                break
            n = min(len(items), n * 2)
        if len(items) == 1 and test_all([[]])[0]:
            items = []
        return items

    def minimize(self, program, sig, pool):
        pro, body = list(program.prologue), list(program.body)
        body = self.ddmin(body, lambda b: program.with_lists(pro, b), sig, pool)
        # Shrink compound items' inner lists.
        for idx, item in enumerate(body):
            if item.inner:

                def make(inner, idx=idx, item=item):
                    return program.with_lists(
                        pro, body[:idx] + [item.clone(inner)] + body[idx + 1 :]
                    )

                body[idx] = item.clone(self.ddmin(list(item.inner), make, sig, pool))
        # Try to flatten each compound item to its inner items (drop the loop/branch).
        for idx in range(len(body)):
            item = body[idx]
            if item.inner is not None:
                nb = body[:idx] + list(item.inner) + body[idx + 1 :]
                if self.interesting(program.with_lists(pro, nb), sig):
                    body = nb
                    break
        pro = self.ddmin(pro, lambda p: program.with_lists(p, body), sig, pool)
        return program.with_lists(pro, body)


# Process-pool workers (log parsing is CPU-bound Python).
_WORKER = None


def init_worker(env, args):
    global _WORKER
    _WORKER = Fuzzer(Tools(env, args.runner, args.workdir), args)


def eval_interesting(job):
    program, sig = job
    return _WORKER.interesting(program, sig)


def run_seed_task(seed):
    """Generate and evaluate one seed; returns a small picklable summary."""
    args = _WORKER.args
    gen = Gen(seed, args.length, args.feature_ops, args.known, args.virtual, args.big_endian)
    gen.fp_arith = args.fp_arith
    program = gen.program(args.fault_tail)
    r = _WORKER.evaluate(program, f"s{seed}")
    if r.get("error"):
        return seed, program, r
    out = {
        "div": r["div"],
        "culprit_insns": r.get("culprit_insns", []),
        "iterm": r["ires"]["term"],
        "nbundles": len(r["ires"]["trace"]),
    }
    return seed, program, out


def mnemonic(text):
    """Signature token of one disassembled instruction."""
    text = re.sub(r"^\(p\d+\)\s*", "", text)
    op, _, operands = text.partition(" ")
    if op.startswith("mov") and "=" in operands:
        dst, src = (x.strip() for x in operands.split("=", 1))
        src = src.split(",")[0]
        for kind, reg in (("to", dst), ("from", src)):
            m = re.match(r"(ar\.\w+|ar\d+|pr(\.rot)?|b\d|ip|psr\S*)$", reg)
            if m:
                return f"mov-{kind}-{re.sub(r'^b[0-7]$', 'br', reg)}"
    return op


def fmt(v):
    return f"{v:#018x}" if isinstance(v, int) and v > 1 else str(v)


def describe(result):
    div = result["div"]
    lines = [f"category: {div['category']}", f"where:    {div.get('detail', '')}"]
    if div.get("culprit") is not None:
        lines.append(
            f"culprit bundle {div['culprit']:#x}: " + " ; ".join(result.get("culprit_insns", []))
        )
    for k, iv, fv in div.get("diffs", [])[:40]:
        lines.append(f"  {k:<12} interp={fmt(iv)}  fork={fmt(fv)}")
    return lines


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--runner", default=str(REPO / "build/fuzz/ia64-fuzz-runner"))
    ap.add_argument(
        "--default-cross", help="CROSS_IA64 if neither the environment nor local.env sets it"
    )
    ap.add_argument(
        "--default-qemu", help="QEMU_IA64 if neither the environment nor local.env sets it"
    )
    ap.add_argument("--seeds", type=int, default=50)
    ap.add_argument("--seed0", type=int, default=1)
    ap.add_argument("--length", type=int, default=40, help="body items per program")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--workdir", default=str(REPO / "build/fuzz/work"))
    ap.add_argument("--repro-dir", default=str(REPO / "tests/fuzz/repro"))
    ap.add_argument("--no-minimize", action="store_true")
    ap.add_argument(
        "--fault-tail",
        type=float,
        default=0.1,
        help="probability of appending a (usually) faulting instruction",
    )
    ap.add_argument(
        "--processes",
        action="store_true",
        help="process pool instead of threads (much faster log parsing; needs a "
        "working multiprocessing forkserver, which some sandboxes lack)",
    )
    ap.add_argument(
        "--fp-arith",
        action="store_true",
        help="also generate rounding FP arithmetic (fma family, fmin/fmax, parallel)",
    )
    ap.add_argument(
        "--known",
        action="store_true",
        help="include extra exception-boundary and MMU fault-priority cases",
    )
    ap.add_argument(
        "--virtual",
        action="store_true",
        help="enable instruction/data/RSE translation with TRs, TCs and a guest VHPT",
    )
    ap.add_argument(
        "--big-endian",
        action="store_true",
        help="also generate UM.be/DCR.be changes and a per-program RSC.be",
    )
    ap.add_argument(
        "--feature-ops",
        action="store_true",
        help="also generate mpy4/mpyshl4/clz (not in the fork's Itanium 2 profile)",
    )
    ap.add_argument(
        "--max-per-signature",
        type=int,
        default=1,
        help="minimize/save at most this many programs per divergence signature",
    )
    ap.add_argument("--emit", type=int, help="print the program for one seed and exit")
    ap.add_argument("--run", help="compare one existing .S file (e.g. a reproducer) and exit")
    args = ap.parse_args()

    if args.emit is not None:
        gen = Gen(
            args.emit, args.length, args.feature_ops, args.known, args.virtual, args.big_endian
        )
        gen.fp_arith = args.fp_arith
        sys.stdout.write(gen.program(args.fault_tail).text())
        return 0
    env = load_env({"CROSS_IA64": args.default_cross, "QEMU_IA64": args.default_qemu})
    tools = Tools(env, args.runner, args.workdir)
    fuzzer = Fuzzer(tools, args)
    if args.run:
        return run_file(fuzzer, Path(args.run))

    seeds = range(args.seed0, args.seed0 + args.seeds)
    found = {}
    stats = {"pass": 0, "diverge": 0, "unresolved": 0, "asm": 0}
    unresolved = {}
    ends, bundles = {}, []
    t0 = time.monotonic()
    # Each worker waits on separate assembler/runner/QEMU processes. Threads
    # avoid Python 3.14's forkserver Unix socket in restricted test sandboxes.
    executor = ProcessPoolExecutor if args.processes else ThreadPoolExecutor
    pool = executor(max_workers=args.jobs, initializer=init_worker, initargs=(env, args))
    for seed, program, r in pool.map(run_seed_task, seeds, chunksize=1):
        if r.get("error"):
            stats[r["error"]] = stats.get(r["error"], 0) + 1
            print(f"seed {seed}: {r['error']} error: {r['detail'][:300]}", file=sys.stderr)
            continue
        div = r["div"]
        if div is None:
            stats["pass"] += 1
            term = r["iterm"]
            key = term["kind"] if term["kind"] != "fault" else f"fault:{term['fault']}"
            ends[key] = ends.get(key, 0) + 1
            bundles.append(r["nbundles"])
            continue
        if div["category"] == "unresolved":
            stats["unresolved"] += 1
            unresolved.setdefault(div["detail"][:160], []).append(seed)
            continue
        stats["diverge"] += 1
        sig = fuzzer.signature(r)
        found.setdefault(sig, []).append((seed, program, r))
        print(
            f"seed {seed}: DIVERGENCE {sig[0]} {' '.join(sig[1])}: {div.get('detail', '')}",
            flush=True,
        )
    print(f"\n{args.seeds} seeds in {time.monotonic() - t0:.0f}s: {stats}")
    if bundles:
        print(
            f"agreeing runs ended by: {ends}; bundles executed: "
            f"min {min(bundles)} avg {sum(bundles) // len(bundles)} max {max(bundles)}"
        )
    for detail, ss in unresolved.items():
        print(f"unresolved (seeds {ss[:8]}): {detail}")
    repro_dir = Path(args.repro_dir)
    for sig, hits in found.items():
        print(f"\n=== {sig[0]} {' '.join(sig[1])}: {len(hits)} seed(s) {[h[0] for h in hits[:10]]}")
        for seed, program, r in hits[: args.max_per_signature]:
            if not args.no_minimize:
                program = fuzzer.minimize(program, sig, pool)
                r = fuzzer.evaluate(program, f"min-{seed}")
                if not r.get("div"):
                    print(f"seed {seed}: minimization lost the divergence")
                    continue
            report = describe(r)
            print("\n".join(report))
            save_repro(repro_dir, sig, seed, program, report)
    pool.shutdown()
    return 1 if found else 0


def save_repro(repro_dir, sig, seed, program, report):
    repro_dir.mkdir(parents=True, exist_ok=True)
    slug = re.sub(r"[^A-Za-z0-9_.-]+", "_", f"{sig[0]}-{'-'.join(sig[1]) or 'none'}")[:80]
    path = repro_dir / f"{slug}-seed{seed}.S"
    header = [
        f"// Reproducer (seed {seed}); run: python3 tests/fuzz/fuzz.py --run {os.path.relpath(path, REPO)}"
    ]
    header += ["// " + l for l in report]
    path.write_text("\n".join(header) + "\n" + program.text())
    print(f"saved {os.path.relpath(path, REPO)}")


def run_file(fuzzer, path):
    """Compare an existing assembly file (its own epilogue/labels are used)."""
    text = path.read_text()
    body = [l for l in text.splitlines()]

    class Raw(Program):
        def text(self):
            return "\n".join(body) + "\n"

    r = fuzzer.evaluate(Raw(0, [], []), "run", keep=True)
    if r.get("error"):
        print(r["detail"])
        return 2
    if not r["div"]:
        print(
            f"{path}: no divergence ({len(r['ires']['trace'])} bundles, terminal {r['ires']['term']})"
        )
        return 0
    print("\n".join(describe(r)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
