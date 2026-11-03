# SPDX-License-Identifier: GPL-2.0-or-later
"""Conformance of the rule prover's models.

(1) IA-64: every sem_ia64.py function, composed exactly as the lifter
    lifts each Tier1-subset instruction form (the forms
    listed in isel.rules), against ia64_execute() on >= 10^5 random and
    edge inputs per decoded opcode. The instructions are assembled by
    $CROSS_IA64 as and decoded by the interpreter's own bundle decoder in
    build/tier1-rule-runner. Each case checks the whole visible result:
    step kind, decoded opcode (it must be the opcode the form names), r2,
    r3, r10, every predicate, AR.LC and the memory word.
(2) AMD64: every T1_X86_OPS row that sem_amd64.py models, assembled by the
    host as and executed natively; registers must match exactly, flags only
    where flags.py defines them (an undefined flag is havoc).
    The load*/store* rows (register forms, used by LOAD/STORE rules) access
    the image's memory word. Other memory rows, the immediate stores, and
    call/ret/push/pop/popfq/mfence are not used by rule templates and are
    listed as skipped. The bytes come from the host assembler; the
    generated table entries are executed through x86enc by
    table_conformance.py.

The expected values are the z3 expressions of the models themselves,
compiled once per form into Python (compile_fn) for speed; a sample of
every form is also evaluated by z3.simplify and must agree with the
compiled function, so the oracle stays the prover's own model.

    $T1_Z3_VENV/bin/python -I tests/tier1/rules/conformance.py
        --runner build/tier1-rule-runner --cross $CROSS_IA64
        [--cases N] [--x86-states N] [--seed S] [--jobs J] [--only REGEX]
"""

import argparse
import importlib.util
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import multiprocessing
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(name, path):
    # -I drops the script directory from sys.path: import by path.
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


try:
    import z3
except ImportError:
    sys.stderr.write("conformance.py: z3 is not importable; run it with $T1_Z3_VENV/bin/python\n")
    sys.exit(1)
rulelib = load("rulelib", os.path.join(ROOT, "engine", "tier1", "rules", "rulelib.py"))
flags = load("flags", os.path.join(HERE, "flags.py"))
sem_ia64 = load("sem_ia64", os.path.join(HERE, "sem_ia64.py"))
sem_amd64 = load("sem_amd64", os.path.join(HERE, "sem_amd64.py"))
S = sem_ia64

MASK64 = 2**64 - 1
SAMPLE_Z3 = 24  # cases per form re-evaluated by z3.simplify

# ---------------------------------------------------------------- z3 -> Python

_BIN = {
    z3.Z3_OP_BADD: "+",
    z3.Z3_OP_BMUL: "*",
    z3.Z3_OP_BAND: "&",
    z3.Z3_OP_BOR: "|",
    z3.Z3_OP_BXOR: "^",
}
_UCMP = {z3.Z3_OP_ULT: "<", z3.Z3_OP_ULEQ: "<=", z3.Z3_OP_UGT: ">", z3.Z3_OP_UGEQ: ">="}
_SCMP = {z3.Z3_OP_SLT: "<", z3.Z3_OP_SLEQ: "<=", z3.Z3_OP_SGT: ">", z3.Z3_OP_SGEQ: ">="}


def compile_fn(inputs, outputs):
    """Python function f(values) -> tuple evaluating the z3 expressions
    `outputs` (bit-vectors as unsigned ints, Bools as bools) for concrete
    `inputs` (z3 constants, in order). A mechanical translation of z3's
    bit-vector semantics; checked against z3.simplify by the caller."""
    index = {v.decl().name(): i for i, v in enumerate(inputs)}
    lines, memo = [], {}
    sys.setrecursionlimit(100000)

    def signed(x, w):
        return "((%s ^ %d) - %d)" % (x, 1 << (w - 1), 1 << (w - 1))

    def emit(e):
        key = e.get_id()
        if key in memo:
            return memo[key]
        if z3.is_bv_value(e):
            return str(e.as_long())
        if z3.is_true(e):
            return "True"
        if z3.is_false(e):
            return "False"
        if z3.is_const(e) and e.decl().kind() == z3.Z3_OP_UNINTERPRETED:
            name = e.decl().name()
            if name not in index:
                raise ValueError("compile_fn: free variable %s" % name)
            return "x[%d]" % index[name]
        k = e.decl().kind()
        a = [emit(c) for c in e.children()]
        w = e.size() if z3.is_bv(e) else 0
        m = (1 << w) - 1
        if k in _BIN:
            code = "(%s) & %d" % (_BIN[k].join(a), m)
        elif k == z3.Z3_OP_BSUB:
            code = "(%s - %s) & %d" % (a[0], a[1], m)
        elif k == z3.Z3_OP_BNEG:
            code = "(-%s) & %d" % (a[0], m)
        elif k == z3.Z3_OP_BNOT:
            code = "%s ^ %d" % (a[0], m)
        elif k == z3.Z3_OP_BSHL:
            code = "((%s << %s) & %d if %s < %d else 0)" % (a[0], a[1], m, a[1], w)
        elif k == z3.Z3_OP_BLSHR:
            code = "(%s >> %s if %s < %d else 0)" % (a[0], a[1], a[1], w)
        elif k == z3.Z3_OP_BASHR:
            code = "(%s >> min(%s, %d)) & %d" % (signed(a[0], w), a[1], w - 1, m)
        elif k == z3.Z3_OP_EXTRACT:
            hi, lo = e.params()
            code = "(%s >> %d) & %d" % (a[0], lo, (1 << (hi - lo + 1)) - 1)
        elif k == z3.Z3_OP_CONCAT:
            code = a[0]
            for c, x in zip(e.children()[1:], a[1:]):
                code = "((%s << %d) | %s)" % (code, c.size(), x)
        elif k == z3.Z3_OP_ZERO_EXT:
            code = a[0]
        elif k == z3.Z3_OP_SIGN_EXT:
            code = "%s & %d" % (signed(a[0], e.children()[0].size()), m)
        elif k == z3.Z3_OP_ITE:
            code = "(%s if %s else %s)" % (a[1], a[0], a[2])
        elif k in (z3.Z3_OP_EQ, z3.Z3_OP_IFF):
            code = "(%s == %s)" % (a[0], a[1])
        elif k == z3.Z3_OP_DISTINCT and len(a) == 2:
            code = "(%s != %s)" % (a[0], a[1])
        elif k in _UCMP:
            code = "(%s %s %s)" % (a[0], _UCMP[k], a[1])
        elif k in _SCMP:
            cw = e.children()[0].size()
            code = "(%s %s %s)" % (signed(a[0], cw), _SCMP[k], signed(a[1], cw))
        elif k == z3.Z3_OP_AND:
            code = "(%s)" % " and ".join(a)
        elif k == z3.Z3_OP_OR:
            code = "(%s)" % " or ".join(a)
        elif k == z3.Z3_OP_NOT:
            code = "(not %s)" % a[0]
        elif k == z3.Z3_OP_XOR:
            code = "(%s != %s)" % (a[0], a[1])
        else:
            raise ValueError("compile_fn: unsupported z3 operator %s" % e.decl())
        name = "t%d" % len(memo)
        lines.append("    %s = %s" % (name, code))
        memo[key] = name
        return name

    results = [emit(o) for o in outputs]
    src = "def f(x):\n%s\n    return (%s,)\n" % ("\n".join(lines), ", ".join(results))
    env = {}
    exec(compile(src, "<compile_fn>", "exec"), env)
    return env["f"]


def z3_eval(inputs, outputs, values):
    """The reference evaluation of the same expressions by z3."""
    subst = []
    for v, x in zip(inputs, values):
        subst.append((v, z3.BoolVal(bool(x)) if z3.is_bool(v) else z3.BitVecVal(x, v.size())))
    out = []
    for o in outputs:
        r = z3.simplify(z3.substitute(o, *subst))
        out.append(z3.is_true(r) if z3.is_bool(r) else r.as_long())
    return tuple(out)


# ---------------------------------------------------------------- IA-64 forms

RAM_BASE = 0x100000
DATA = 0x108000
EDGES = [
    0,
    1,
    MASK64,
    2**63,
    2**63 - 1,
    2**31,
    2**31 - 1,
    2**32 - 1,
    2**32,
    2**32 + 7,
    0xFFFFFFFF00000000,
    0x80000000FFFFFFFF,
    63,
    64,
    65,
    127,
    128,
    255,
    256,
    0x7F,
    0x80,
    0xFF,
    0x7FFF,
    0x8000,
    0xFFFF,
    0x10000,
    0xFFFFFFFFFFFFFFF0,
    0x00000000FFFF8000,
]


def value(rng):
    """A random or edge 64-bit value (half/half), occasionally a small count."""
    r = rng.random()
    if r < 0.4:
        return rng.choice(EDGES)
    if r < 0.5:
        return rng.randrange(0, 300)
    if r < 0.6:
        return (rng.randrange(0, 2**32) | (MASK64 << 32 if rng.random() < 0.5 else 0)) & MASK64
    return rng.getrandbits(64)


class Form:
    """One IA-64 instruction form: asm text from immediate parameters, the
    lifted IR expression of every output (sem_ia64), the decoded opcode
    names for it. unit: (template, slot) of the bundle it is placed in."""

    def __init__(
        self,
        name,
        asm,
        params,
        expect,
        opcode,
        unit=("mii", 1),
        outputs=("r10",),
        preds=False,
        mem=False,
        qp=True,
        lc=False,
    ):
        self.name, self.asm, self.params, self.expect = name, asm, params, expect
        self.opcode, self.unit, self.outputs = opcode, unit, outputs
        self.preds, self.mem, self.qp, self.lc = preds, mem, qp, lc


def imm_range(lo, hi, extra=()):
    return [{"k": k} for k in range(lo, hi + 1)] + [{"k": k} for k in extra]


def imm_sample(bits, n, rng):
    lo, hi = -(2 ** (bits - 1)), 2 ** (bits - 1) - 1
    ks = {lo, hi, 0, 1, -1, lo + 1, hi - 1} | {rng.randrange(lo, hi + 1) for _ in range(n)}
    return [{"k": k} for k in sorted(ks)]


def fields():
    return [{"p": p, "l": l} for p in range(64) for l in range(1, 65)]


def sel(qp, new, old):
    return S.SELECT(qp, new, old)


def build_forms(rng):
    F = []

    def k_(e):
        return S.CONST(e["k"])

    def pure(name, asm, fn, opcode, params=None):
        F.append(
            Form(
                name,
                asm,
                params or [{}],
                lambda e, fn=fn: {"r10": sel(e["qp"], fn(e), e["d"])},
                opcode,
            )
        )

    # integer; lifted forms as in isel.rules' header
    pure("add", "add r10 = r2, r3", lambda e: S.ADD(e["a"], e["b"]), "ADD")
    pure(
        "add_one",
        "add r10 = r2, r3, 1",
        lambda e: S.ADD(S.ADD(e["a"], e["b"]), S.CONST(1)),
        "ADD_ONE",
    )
    pure("sub", "sub r10 = r2, r3", lambda e: S.SUB(e["a"], e["b"]), "SUB")
    pure(
        "sub_one",
        "sub r10 = r2, r3, 1",
        lambda e: S.SUB(S.SUB(e["a"], e["b"]), S.CONST(1)),
        "SUB_ONE",
    )
    pure(
        "sub_imm",
        "sub r10 = {k}, r3",
        lambda e: S.SUB(k_(e), e["b"]),
        "SUB_IMM",
        imm_range(-128, 127),
    )
    pure(
        "adds",
        "adds r10 = {k}, r3",
        lambda e: S.ADD(e["b"], k_(e)),
        "ADDS",
        imm_sample(14, 600, rng),
    )
    pure(
        "addl",
        "addl r10 = {k}, r3",
        lambda e: S.ADD(e["b"], k_(e)),
        "ADDL",
        imm_sample(22, 600, rng),
    )
    pure("mov_imm22", "mov r10 = {k}", lambda e: k_(e), "ADDL", imm_sample(22, 300, rng))
    for op in ("and", "or", "xor", "andcm"):
        fn = getattr(S, op.upper())
        pure(op, "%s r10 = r2, r3" % op, lambda e, fn=fn: fn(e["a"], e["b"]), op.upper())
        pure(
            op + "_imm",
            "%s r10 = {k}, r3" % op,
            lambda e, fn=fn: fn(k_(e), e["b"]),
            op.upper() + "_IMM",
            imm_range(-128, 127),
        )
    pure(
        "shladd",
        "shladd r10 = r2, {c}, r3",
        lambda e: S.ADD(S.SHL_IMM(e["a"], e["c"]), e["b"]),
        "SHLADD",
        [{"c": c} for c in (1, 2, 3, 4)],
    )
    # shift/bitfield (row 2)
    pure("shl", "shl r10 = r2, r3", lambda e: S.SHL(e["a"], e["b"]), "SHL")
    pure("shr", "shr r10 = r2, r3", lambda e: S.SHR(e["a"], e["b"]), "SHR")
    pure("shr_u", "shr.u r10 = r2, r3", lambda e: S.SHRU(e["a"], e["b"]), "SHRU")
    pure(
        "shl_imm",
        "shl r10 = r2, {c}",
        lambda e: S.SHL_IMM(e["a"], e["c"]),
        "DEPZ",
        [{"c": c} for c in range(64)],
    )
    pure(
        "shr_imm",
        "shr r10 = r2, {c}",
        lambda e: S.EXTR(e["c"] | (64 - e["c"]) << 8, e["a"]),
        "EXTR",
        [{"c": c} for c in range(64)],
    )
    pure(
        "shr_u_imm",
        "shr.u r10 = r2, {c}",
        lambda e: S.EXTRU(e["c"] | (64 - e["c"]) << 8, e["a"]),
        "EXTRU",
        [{"c": c} for c in range(64)],
    )
    pure(
        "extr",
        "extr r10 = r2, {p}, {l}",
        lambda e: S.EXTR(e["p"] | e["l"] << 8, e["a"]),
        "EXTR",
        fields(),
    )
    pure(
        "extr_u",
        "extr.u r10 = r2, {p}, {l}",
        lambda e: S.EXTRU(e["p"] | e["l"] << 8, e["a"]),
        "EXTRU",
        fields(),
    )
    pure(
        "dep_z",
        "dep.z r10 = r2, {p}, {l}",
        lambda e: S.DEPZ(e["p"] | e["l"] << 8, e["a"]),
        "DEPZ",
        fields(),
    )
    pure(
        "dep_z_imm",
        "dep.z r10 = {k}, {p}, {l}",
        lambda e: S.DEPZ(e["p"] | e["l"] << 8, S.CONST(e["k"])),
        "DEPZ_IMM",
        [dict(f, k=rng.randrange(-128, 128)) for f in fields() if rng.random() < 0.5],
    )
    for w, n in ((8, 1), (16, 2), (32, 4)):
        pure("sxt%d" % n, "sxt%d r10 = r2" % n, lambda e, w=w: S.SXT(w, e["a"]), "SXT%d" % n)
        pure("zxt%d" % n, "zxt%d r10 = r2" % n, lambda e, w=w: S.ZXT(w, e["a"]), "ZXT%d" % n)
    F.append(
        Form(
            "movl",
            "movl r10 = {k}",
            [
                {"k": k & MASK64}
                for k in [0, 1, -1, 2**63, 2**63 - 1, 2**32]
                + [rng.getrandbits(64) for _ in range(500)]
            ],
            lambda e: {"r10": sel(e["qp"], k_(e), e["d"])},
            "MOVL",
            unit=("mlx", 1),
        )
    )
    # compare (row 3): normal and .unc, register and imm8 forms
    for width, pre in ((64, "cmp"), (32, "cmp4")):
        for rel in ("eq", "lt", "ltu"):
            packed = rulelib.CMP_RELS[rel] | width << 8
            for imm in (False, True):
                for unc in (False, True):
                    asm = "%s.%s%s p6, p7 = %s, r3" % (
                        pre,
                        rel,
                        ".unc" if unc else "",
                        "{k}" if imm else "r2",
                    )

                    def expect(e, packed=packed, imm=imm, unc=unc):
                        c = S.CMP(packed, S.CONST(e["k"]) if imm else e["a"], e["b"])
                        if unc:  # unc: p1 = AND1(qp, c), p2 = AND1(qp, NOT1 c)
                            return {"p6": S.AND1(e["qp"], c), "p7": S.AND1(e["qp"], S.NOT1(c))}
                        return {
                            "p6": sel(e["qp"], c, e["p6"]),
                            "p7": sel(e["qp"], S.NOT1(c), e["p7"]),
                        }

                    F.append(
                        Form(
                            "%s_%s%s%s" % (pre, rel, "_imm" if imm else "", "_unc" if unc else ""),
                            asm,
                            imm_range(-128, 127) if imm else [{}],
                            expect,
                            "%s_%s%s" % (pre.upper(), rel.upper(), "_IMM" if imm else ""),
                            outputs=(),
                            preds=True,
                        )
                    )
    # branch: br.cloop ignores qp; slot 2 of an MIB bundle
    F.append(
        Form(
            "br_cloop",
            "br.cloop.sptk.few {label}",
            [{}],
            lambda e: dict(zip(("lc", "taken"), S.CLOOP(e["lc"]))),
            "BR_CLOOP",
            unit=("mib", 2),
            outputs=(),
            qp=False,
            lc=True,
        )
    )
    # load/store address paths (rows 4, 5): value transformation and post-increment
    for n in (1, 2, 4, 8):
        for acq in ("", ".acq"):
            for inc in ("", "imm", "reg"):
                asm = "ld%d%s r10 = [r2]%s" % (n, acq, {"": "", "imm": ", {k}", "reg": ", r3"}[inc])

                def expect(e, n=n, inc=inc):
                    out = {"r10": sel(e["qp"], S.LOAD_VALUE(n, e["mem"]), e["d"])}
                    if inc:
                        step = S.CONST(e["k"]) if inc == "imm" else e["b"]
                        out["r2"] = sel(e["qp"], S.ADD(e["a"], step), e["a"])
                    return out

                F.append(
                    Form(
                        "ld%d%s%s" % (n, acq.replace(".", "_"), "_" + inc if inc else ""),
                        asm,
                        imm_range(-256, 255) if inc == "imm" else [{}],
                        expect,
                        "LD%d" % n,
                        unit=("mmi", 0),
                        outputs=("r10", "r2"),
                        mem=True,
                    )
                )
        for rel in ("", ".rel"):
            for inc in ("", "imm"):
                asm = "st%d%s [r2] = r3%s" % (n, rel, ", {k}" if inc else "")

                def expect(e, n=n, inc=inc):
                    out = {"mem": sel(e["qp"], S.STORE_VALUE(n, e["mem"], e["b"]), e["mem"])}
                    if inc:
                        out["r2"] = sel(e["qp"], S.ADD(e["a"], S.CONST(e["k"])), e["a"])
                    return out

                F.append(
                    Form(
                        "st%d%s%s" % (n, rel.replace(".", "_"), "_imm" if inc else ""),
                        asm,
                        imm_range(-256, 255) if inc else [{}],
                        expect,
                        "ST%d" % n + ("REL" if rel else ""),
                        unit=("mmi", 0),
                        outputs=("r2",),
                        mem=True,
                    )
                )
    return F


BUNDLE = {
    "mii": "{ .mii\n  nop.m 0\n  %s\n  nop.i 0 ;;\n}",
    "mmi": "{ .mmi\n  %s\n  nop.m 0\n  nop.i 0 ;;\n}",
    "mib": "{ .mib\n  nop.m 0\n  nop.i 0\n  %s ;;\n}",
    "mlx": "{ .mlx\n  nop.m 0\n  %s ;;\n}",
}


def assemble_ia64(forms, cross, work):
    """Assembles every (form, parameter set, qp) variant, one per bundle.
    Returns {form name: [(params, qp, lo, hi)]}."""
    order, text = [], [".text", ".explicit"]
    for f in forms:
        for params in f.params:
            for qp in (False, True) if f.qp else (False,):
                label = ".Lb%d" % len(order)
                insn = f.asm.format(label=label, **params)
                if qp:
                    insn = "(p5) " + insn
                text.append("%s:\n%s" % (label, BUNDLE[f.unit[0]] % insn))
                order.append((f.name, params, qp))
    src = os.path.join(work, "ia64.s")
    obj = os.path.join(work, "ia64.o")
    binary = os.path.join(work, "ia64.bin")
    with open(src, "w") as fh:
        fh.write("\n".join(text) + "\n")
    for cmd in ([cross + "as", src, "-o", obj], [cross + "objcopy", "-O", "binary", obj, binary]):
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0 or r.stdout.strip():
            raise SystemExit("conformance.py: %s failed:\n%s" % (" ".join(cmd), r.stdout[:4000]))
    with open(binary, "rb") as fh:
        data = fh.read()
    if len(data) != 16 * len(order):
        raise SystemExit(
            "conformance.py: %d bundles expected, %d bytes assembled" % (len(order), len(data))
        )
    out = {}
    for i, (name, params, qp) in enumerate(order):
        lo, hi = struct.unpack_from("<QQ", data, 16 * i)
        out.setdefault(name, []).append((params, qp, lo, hi))
    return out


def ia64_symbols(f):
    """The form's z3 inputs (in compile order) and its expected outputs."""
    env = {n: z3.BitVec(n, 64) for n in ("a", "b", "d", "lc", "mem", "k", "c", "p", "l")}
    for n in ("qp", "p6", "p7"):
        env[n] = z3.Bool(n)
    names = ["a", "b", "d", "lc", "mem", "k", "c", "p", "l", "qp", "p6", "p7"]
    exp = f.expect(env)
    keys = sorted(exp)
    return [env[n] for n in names], names, keys, [exp[k] for k in keys]


FORMS = {}  # name -> Form; set before the (fork) pool starts: Forms hold lambdas


def ia64_worker(job):
    """Runs every case of the given forms; returns (form, cases, failures)."""
    names, encodings, runner, ncases, seed = job
    forms = [FORMS[n] for n in names]
    results = []
    for f in forms:
        rng = random.Random("%s:%s" % (seed, f.name))
        inputs, names, keys, outs = ia64_symbols(f)
        fn = compile_fn(inputs, outs)
        cases, lines = [], []
        variants = encodings[f.name]
        for i in range(ncases):
            params, qp, lo, hi = (
                variants[i % len(variants)] if i < len(variants) else rng.choice(variants)
            )
            env = {
                "a": value(rng),
                "b": value(rng),
                "d": value(rng),
                "lc": value(rng),
                "mem": value(rng),
                "k": params.get("k", 0) & MASK64,
                "c": params.get("c", 0),
                "p": params.get("p", 0),
                "l": params.get("l", 0),
                "qp": (rng.random() < 0.5) if qp else True,
                "p6": rng.random() < 0.5,
                "p7": rng.random() < 0.5,
            }
            if rng.random() < 0.1:
                env["b"] = env["a"]  # equal operands: eq, ltu, CF edges
            if f.mem:
                env["a"] = DATA + 8 * rng.randrange(256)
            pr = (
                (rng.getrandbits(64) & ~0xE0) | 1 | env["qp"] << 5 | env["p6"] << 6 | env["p7"] << 7
            )
            maddr = env["a"] if f.mem else DATA
            line = "%x %x %d %x %x %x %x 3 2 %x 3 %x 10 %x 3 2 3 10\n" % (
                lo,
                hi,
                f.unit[1],
                pr,
                env["lc"],
                maddr,
                env["mem"],
                env["a"],
                env["b"],
                env["d"],
            )
            cases.append((env, pr, params, qp))
            lines.append(line)
        r = subprocess.run(
            [runner, "ia64"],
            input="".join(lines),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if r.returncode != 0:
            results.append((f.name, 0, ["runner failed: %s" % r.stderr[:2000]], set()))
            continue
        got_lines = r.stdout.split("\n")
        failures, opcodes = [], set()
        for i, (env, pr, params, qp) in enumerate(cases):
            vals = [env[n] for n in names]
            exp = dict(zip(keys, fn(vals)))
            if i < SAMPLE_Z3:
                ref = dict(zip(keys, z3_eval(inputs, outs, vals)))
                if ref != exp:
                    failures.append(
                        "compile_fn disagrees with z3: %s %s vs %s" % (params, ref, exp)
                    )
            kind, opname, gpr, glc, gmem, g2, g3, g10 = got_lines[i].split()
            opcodes.add(opname)
            want = {
                "kind": "complete",
                "pr": pr,
                "lc": env["lc"],
                "mem": env["mem"],
                "r2": env["a"],
                "r3": env["b"],
                "r10": env["d"],
            }
            for key, v in exp.items():
                if key in ("p6", "p7"):
                    bit = 1 << int(key[1:])
                    want["pr"] = (want["pr"] & ~bit) | (bit if v else 0)
                elif key == "taken":
                    want["kind"] = "branch" if v else "complete"
                else:
                    want[key] = v
            got = {
                "kind": kind,
                "pr": int(gpr, 16),
                "lc": int(glc, 16),
                "mem": int(gmem, 16),
                "r2": int(g2, 16),
                "r3": int(g3, 16),
                "r10": int(g10, 16),
            }
            bad = [k for k in want if want[k] != got[k]]
            if opname != f.opcode:
                bad.append("opcode %s (expected %s)" % (opname, f.opcode))
            if bad and len(failures) < 8:
                failures.append(
                    "%s qp=%s %s: %s; inputs a=%x b=%x d=%x lc=%x mem=%x"
                    % (
                        f.asm.format(label="L", **params),
                        int(qp),
                        params,
                        ", ".join(
                            "%s want %s got %s" % (k, fmt(want.get(k)), fmt(got.get(k)))
                            if k in want
                            else k
                            for k in bad
                        ),
                        env["a"],
                        env["b"],
                        env["d"],
                        env["lc"],
                        env["mem"],
                    )
                )
            elif bad:
                failures.append(None)
        results.append((f.name, len(cases), failures, opcodes))
    return results


def fmt(v):
    return "%x" % v if isinstance(v, int) and not isinstance(v, bool) else str(v)


# ---------------------------------------------------------------- AMD64

GPR = [
    "rax",
    "rcx",
    "rdx",
    "rbx",
    "rsp",
    "rbp",
    "rsi",
    "rdi",
    "r8",
    "r9",
    "r10",
    "r11",
    "r12",
    "r13",
    "r14",
    "r15",
]
USABLE = [r for r in GPR if r not in ("rsp", "rdi")]  # rdi points at the image
FLAG_BITS = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7, "OF": 11}
MODELLED = re.compile(
    r"^(mov64|mov32|movabs64|movzx32_(8|16)|movsx64_(8|16|32)|lea64|xor32|not64|neg64|"
    r"imul64|adc64|inc64|(add|sub|and|or|xor|cmp|test)(64|32)|(shl|shr|sar)64|cmov\w+64|set\w+8|j\w+|"
    r"load(64|32|16zx|8zx)|store(64|32|16|8))$"
)
MEM_OFFSET = 144  # struct x86_image.mem in rule_runner.c


def is_reg(spec):
    return spec[0] == "R" and spec != "REL"


def reg_name(r, width):
    if width == 64:
        return r
    legacy = {"rax": "a", "rcx": "c", "rdx": "d", "rbx": "b"}
    if r in legacy:
        return {32: "e%sx", 16: "%sx", 8: "%sl"}[width] % legacy[r]
    if r in ("rbp", "rsi", "rdi", "rsp"):
        return {32: "e" + r[1:], 16: r[1:], 8: r[1:] + "l"}[width]
    return r + {32: "d", 16: "w", 8: "b"}[width]


def is_access(op):
    return op.mnemonic.startswith(("load", "store"))


def gas_mnemonic(op):
    m = op.mnemonic
    if m.startswith("load"):
        return "movzx" if m.endswith("zx") else "mov"
    if m.startswith("store"):
        return "mov"
    if m.startswith(("movzx", "movsx")):
        return "movsxd" if m == "movsx64_32" else m[:5]
    if m.startswith(("set", "cmov")) or m[0] == "j":
        return m.rstrip("0123456789")
    return m.rstrip("0123456789")


class Snippet:
    def __init__(self, op, regs, imm, mem):
        self.op, self.regs, self.imm, self.mem = op, regs, imm, mem


def x86_snippets(rng):
    ops, _, _ = rulelib.load_x86_ops()
    snippets, skipped = [], []
    for op in ops:
        if (
            not MODELLED.match(op.mnemonic)
            or op.mnemonic == "jmp"
            or (
                any(s.startswith("M") and s != "MA" for s in op.specs)
                and not (is_access(op) and not any(s.startswith("I") for s in op.specs))
            )
        ):
            skipped.append("%s (%s)" % (op.mnemonic, op.enum))
            continue
        nreg = sum(1 for s in op.specs if is_reg(s))
        imms = [None]
        if "I8" in op.specs:
            imms = list(range(64)) + [64, 65, 127, 128, 255]
        elif "I32" in op.specs:
            lo = -(2**31)
            imms = [0, 1, -1, 2**31 - 1, lo, 0x7F, -0x80, 0x80] + [
                rng.randrange(lo, 2**31) for _ in range(8)
            ]
            if op.specs[0].startswith("R32"):
                imms += [2**31, 2**32 - 1]
        elif "I64" in op.specs:
            imms = [0, 1, MASK64, 2**63, 2**32] + [rng.getrandbits(64) for _ in range(11)]
        mems = [None]
        if "MA" in op.specs:
            mems = []
            for _ in range(32):
                m = rulelib.Mem()
                shape = rng.randrange(4)
                m.base = None if shape == 3 else "$" + rng.choice(USABLE)
                m.index = "$" + rng.choice(USABLE) if shape >= 2 else None
                m.scale = rng.choice((1, 2, 4, 8)) if m.index else 1
                m.disp = rng.choice(
                    (0, 1, -1, 8, -8, 2**31 - 1, -(2**31), rng.randrange(-(2**31), 2**31))
                )
                mems.append(m)
        variants = []
        for imm in imms:
            for mem in mems:
                for alias in (
                    (False, True) if nreg >= 2 and imm is None and mem is None else (False,)
                ):
                    regs = []
                    for s in op.specs:
                        if s == "CL":
                            regs.append("rcx")
                        elif is_reg(s):
                            regs.append(regs[0] if alias and regs else rng.choice(USABLE))
                    variants.append((regs, imm, mem))
        if imms == [None] and mems == [None]:
            for _ in range(5):
                regs = [
                    rng.choice(USABLE) if s != "CL" else "rcx"
                    for s in op.specs
                    if is_reg(s) or s == "CL"
                ]
                variants.append((regs, None, None))
        for regs, imm, mem in variants:
            snippets.append(Snippet(op, regs, imm, mem))
    return snippets, skipped


def insn_text(sn):
    op = sn.op
    ops, regs = [], list(sn.regs)
    for s in op.specs:
        if is_reg(s):
            ops.append(reg_name(regs.pop(0), int(s[1:].split("_")[0])))
        elif s == "CL":
            regs.pop(0)
            ops.append("cl")
        elif s.startswith("I"):
            v = sn.imm
            if s == "I32" and v >= 2**31:
                v -= 2**32
            ops.append(str(v if s != "I64" else v & MASK64))
        elif s.startswith("M") and s != "MA":
            ops.append(
                "%s ptr [rdi+%d]"
                % (
                    {8: "byte", 16: "word", 32: "dword", 64: "qword"}[int(s[1:].split("_")[0])],
                    MEM_OFFSET,
                )
            )
        elif s == "MA":
            m = sn.mem
            text = " + ".join(
                ([m.base[1:]] if m.base else [])
                + (["%s*%d" % (m.index[1:], m.scale)] if m.index else [])
            )
            if not text:
                text = str(m.disp)
            elif m.disp:
                text += " %s %d" % ("-" if m.disp < 0 else "+", abs(m.disp))
            ops.append("qword ptr [%s]" % text)
        elif s == "REL":
            ops.append("1f")
    if "T1_XA_ZERO_IDIOM" in op.attrs:
        ops = ops * 2
    return "%s %s" % (gas_mnemonic(op), ", ".join(ops))


def snippet_asm(sn, i):
    op = sn.op
    body = [insn_text(sn)]
    if op.cc is not None and op.mnemonic.startswith("j"):
        body += ["mov qword ptr [rdi+136], 0", "jmp 2f", "1: mov qword ptr [rdi+136], 1", "2:"]
    load = ["mov %s, [rdi+%d]" % (r, 8 * GPR.index(r)) for r in USABLE]
    store = ["mov [rdi+%d], %s" % (8 * GPR.index(r), r) for r in USABLE]
    lines = (
        [".balign 512, 0xcc", "snip%d:" % i]
        + ["push %s" % r for r in ("rbx", "rbp", "r12", "r13", "r14", "r15", "rdi")]
        + load
        + ["push qword ptr [rdi+128]", "popfq"]
        + body
        + ["pushfq", "pop qword ptr [rdi+128]"]
        + store
        + ["pop %s" % r for r in ("rdi", "r15", "r14", "r13", "r12", "rbp", "rbx")]
        + ["ret"]
    )
    return "\n".join(
        "  " + l if not l.endswith(":") and not l.startswith(".") else l for l in lines
    )


def x86_model(sn):
    """The symbolic effect of the snippet's instruction under sem_amd64."""
    regs = {r: z3.BitVec(r, 64) for r in USABLE}
    fl = {f: z3.Bool("f_" + f) for f in flags.FLAGS}
    imm = z3.BitVec("imm", 64)
    insn = rulelib.Insn("x86 conformance", "x86 conformance")
    insn.op, insn.mnemonic = sn.op, sn.op.mnemonic
    word = z3.BitVec("mem", 64)
    rlist = list(sn.regs)
    for s in sn.op.specs:
        if s == "CL":
            rlist.pop(0)
            insn.operands.append(("cl", "$rcx"))
        elif is_reg(s):
            insn.operands.append(("reg", "$" + rlist.pop(0)))
        elif s.startswith("I"):
            insn.operands.append(("imm", "#imm"))
        elif s == "MA":
            insn.operands.append(("mem", sn.mem))
        elif s.startswith("M"):
            insn.operands.append(("mem", rulelib.Mem()))  # [rdi + MEM_OFFSET]: address 0 here
        elif s == "REL":
            insn.operands.append(("label", "$T"))
    alias = {"$" + r: r for r in USABLE}
    st = sem_amd64.State(regs, fl, z3.BitVecVal(0, 64), word)
    res = sem_amd64.step(
        insn, st, alias, lambda x: imm if x == "#imm" else z3.BitVecVal(x & MASK64, 64), flags
    )
    taken = res[2] if res else z3.BoolVal(False)
    inputs = [regs[r] for r in USABLE] + [fl[f] for f in flags.FLAGS] + [imm, word]
    outs = [st.regs[r] for r in USABLE] + [st.flags[f] for f in flags.FLAGS] + [taken, st.mem_word]
    havocs = sorted(
        {
            v.decl().name(): v
            for o in outs
            for v in free_vars(o)
            if v.decl().name().startswith("havoc_")
        }.values(),
        key=lambda v: v.decl().name(),
    )
    return inputs + havocs, outs, len(havocs)


def free_vars(e, seen=None):
    seen = set() if seen is None else seen
    out = []
    stack = [e]
    while stack:
        x = stack.pop()
        if x.get_id() in seen:
            continue
        seen.add(x.get_id())
        if z3.is_const(x) and x.decl().kind() == z3.Z3_OP_UNINTERPRETED:
            out.append(x)
        stack.extend(x.children())
    return out


def x86_worker(job):
    snippets, first, blob, runner, nstates, seed = job
    results = []
    lines, cases = [], []
    compiled = []
    for j, sn in enumerate(snippets):
        inputs, outs, nh = x86_model(sn)
        compiled.append((compile_fn(inputs, outs), inputs, outs, nh))
        rng = random.Random("%s:x86:%d" % (seed, first + j))
        for _ in range(nstates):
            gpr = [value(rng) for _ in GPR]
            for k in range(len(GPR)):  # equal operands: ZF/CF/OF edges
                if rng.random() < 0.2:
                    gpr[k] = rng.choice(gpr)
            fbits = {f: rng.random() < 0.5 for f in flags.FLAGS}
            status = sum(1 << FLAG_BITS[f] for f in flags.FLAGS if fbits[f])
            mem = value(rng)
            lines.append(
                "%d %x %s %x\n" % (first + j, status, " ".join("%x" % g for g in gpr), mem)
            )
            cases.append((j, gpr, fbits, mem))
    r = subprocess.run(
        [runner, "x86", blob],
        input="".join(lines),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if r.returncode != 0:
        return [("x86 runner", 0, ["runner failed: %s" % r.stderr[:2000]])]
    got_lines = r.stdout.split("\n")
    per_op = {}
    for i, (j, gpr, fbits, mem) in enumerate(cases):
        sn = snippets[j]
        fn, inputs, outs, nh = compiled[j]
        base = (
            [gpr[GPR.index(r)] for r in USABLE]
            + [fbits[f] for f in flags.FLAGS]
            + [(sn.imm or 0) & MASK64, mem]
        )
        e0, e1 = fn(base + [False] * nh), fn(base + [True] * nh)
        if i % 50 == 0:
            ref = z3_eval(inputs, outs, base + [False] * nh)
            if ref != e0:
                per_op.setdefault(sn.op.mnemonic, [0, []])[1].append(
                    "compile_fn disagrees with z3 for %s" % insn_text(sn)
                )
        g = got_lines[i].split()
        taken, gflags, ggpr, gmem = (
            int(g[0]),
            int(g[1], 16),
            [int(x, 16) for x in g[2:18]],
            int(g[18], 16),
        )
        bad = []
        nreg = len(USABLE)
        for k, rname in enumerate(USABLE):
            if e0[k] != e1[k]:
                bad.append("%s depends on an undefined flag" % rname)
            elif e0[k] != ggpr[GPR.index(rname)]:
                bad.append("%s want %x got %x" % (rname, e0[k], ggpr[GPR.index(rname)]))
        for k, f in enumerate(flags.FLAGS):
            if e0[nreg + k] == e1[nreg + k]:  # defined (not havoc) on this input
                if bool(e0[nreg + k]) != bool(gflags >> FLAG_BITS[f] & 1):
                    bad.append("%s want %d got %d" % (f, e0[nreg + k], gflags >> FLAG_BITS[f] & 1))
        if sn.op.cc is not None and sn.op.mnemonic.startswith("j") and bool(e0[-2]) != bool(taken):
            bad.append("taken want %d got %d" % (e0[-2], taken))
        if e0[-1] != gmem:
            bad.append("memory want %x got %x" % (e0[-1], gmem))
        entry = per_op.setdefault(sn.op.mnemonic, [0, []])
        entry[0] += 1
        if bad and len(entry[1]) < 6:
            entry[1].append(
                "%s: %s; in %s flags %s"
                % (
                    insn_text(sn),
                    "; ".join(bad),
                    " ".join("%s=%x" % (r, gpr[GPR.index(r)]) for r in USABLE),
                    "".join(f for f in flags.FLAGS if fbits[f]),
                )
            )
    for m, (n, fails) in per_op.items():
        results.append(("x86 " + m, n, fails))
    return results


def assemble_x86(snippets, work):
    src = os.path.join(work, "x86.s")
    obj = os.path.join(work, "x86.o")
    blob = os.path.join(work, "x86.bin")
    with open(src, "w") as fh:
        fh.write(".intel_syntax noprefix\n.text\n")
        for i, sn in enumerate(snippets):
            fh.write(snippet_asm(sn, i) + "\n")
        fh.write(".balign 512, 0xcc\n")
    for cmd in (
        ["as", "--64", src, "-o", obj],
        ["objcopy", "-O", "binary", "-j", ".text", obj, blob],
    ):
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0 or r.stdout.strip():
            raise SystemExit("conformance.py: %s failed:\n%s" % (" ".join(cmd), r.stdout[:4000]))
    size = os.path.getsize(blob)
    if size != 512 * len(snippets):
        raise SystemExit(
            "conformance.py: x86 snippets exceed 512 bytes (%d bytes for %d)"
            % (size, len(snippets))
        )
    return blob


# ---------------------------------------------------------------- driver


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", required=True)
    ap.add_argument("--cross", required=True, help="CROSS_IA64 prefix")
    ap.add_argument("--cases", type=int, default=100000, help="IA-64 cases per decoded opcode")
    ap.add_argument("--x86-states", type=int, default=256, help="machine states per x86 snippet")
    ap.add_argument("--seed", default="tier1-conformance")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", help="regex on form / mnemonic names")
    ap.add_argument("--keep", help="keep the generated sources in this directory")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    forms = build_forms(rng)
    if args.only:
        forms = [f for f in forms if re.search(args.only, f.name)]
    work = args.keep or tempfile.mkdtemp(prefix="tier1-conformance-")
    os.makedirs(work, exist_ok=True)
    ok = True
    try:
        encodings = assemble_ia64(forms, args.cross, work)
        # >= `cases` per decoded opcode: split among the forms of an opcode.
        per_opcode = {}
        for f in forms:
            per_opcode.setdefault(f.opcode, []).append(f)
        jobs = []
        for f in forms:
            n = -(-args.cases // len(per_opcode[f.opcode]))
            jobs.append(
                ([f.name], {f.name: encodings[f.name]}, os.path.abspath(args.runner), n, args.seed)
            )
            FORMS[f.name] = f
        snippets, skipped = x86_snippets(random.Random(args.seed + ":x86"))
        if args.only:
            snippets = [s for s in snippets if re.search(args.only, s.op.mnemonic)]
        xjobs = []
        if snippets:
            blob = assemble_x86(snippets, work)
            chunk = max(1, len(snippets) // max(1, args.jobs))
            for i in range(0, len(snippets), chunk):
                xjobs.append(
                    (
                        snippets[i : i + chunk],
                        i,
                        blob,
                        os.path.abspath(args.runner),
                        args.x86_states,
                        args.seed,
                    )
                )
        with ProcessPoolExecutor(
            max_workers=args.jobs, mp_context=multiprocessing.get_context("fork")
        ) as pool:
            ia64_results = [r for rs in pool.map(ia64_worker, jobs) for r in rs]
            x86_results = [r for rs in pool.map(x86_worker, xjobs) for r in rs]
        if not args.only or re.search(args.only, "fpi"):
            fp_image = os.path.join(work, "fpi.bin")
            fp_obj = os.path.join(work, "fpi.o")
            subprocess.run(
                [args.cross + "as", "-o", fp_obj, os.path.join(ROOT, "tests/tier1/frag_fpi.S")],
                check=True,
            )
            subprocess.run(
                [args.cross + "objcopy", "-O", "binary", "-j", ".text", fp_obj, fp_image],
                check=True,
            )
            subprocess.run(
                [
                    sys.executable,
                    "-I",
                    os.path.join(HERE, "fpi_conformance.py"),
                    "--runner",
                    args.runner,
                    "--image",
                    fp_image,
                    "--cases",
                    str(args.cases),
                ],
                check=True,
            )
        totals = {}
        for name, n, fails, opcodes in ia64_results:
            f = [x for x in forms if x.name == name][0]
            totals[f.opcode] = totals.get(f.opcode, 0) + n
            status = "ok" if not fails else "FAILED (%d)" % len(fails)
            print(
                "conformance: ia64 %-18s %7d cases %-12s %s"
                % (name, n, ",".join(sorted(opcodes)), status)
            )
            for x in fails[:8]:
                if x:
                    print("    " + x)
            ok &= not fails
        short = [op for op, n in totals.items() if n < args.cases]
        if short:
            print("conformance: fewer than %d cases for %s" % (args.cases, ", ".join(short)))
            ok = False
        merged = {}
        for name, n, fails in x86_results:
            m = merged.setdefault(name, [0, []])
            m[0] += n
            m[1] += fails
        for name in sorted(merged):
            n, fails = merged[name]
            print(
                "conformance: %-28s %7d cases %s"
                % (name, n, "ok" if not fails else "FAILED (%d)" % len(fails))
            )
            for x in fails[:6]:
                print("    " + x)
            ok &= not fails
        print(
            "conformance: x86 rows not modelled (unused by rule templates): %s" % ", ".join(skipped)
        )
        print(
            "conformance: %d IA-64 forms over %d opcodes, %d cases; %d x86 snippets, %d cases: %s"
            % (
                len(forms),
                len(totals),
                sum(totals.values()),
                len(snippets),
                sum(m[0] for m in merged.values()),
                "passed" if ok else "FAILED",
            )
        )
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
