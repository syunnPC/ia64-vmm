# SPDX-License-Identifier: GPL-2.0-or-later
"""Instruction encoders (Vol3 ch.4 F1-F14, M18/M19) and architectural state
transitions (Vol3 ch.2 pseudo-code) on top of the exact model in fpmodel.py."""

import copy
from fpmodel import (
    FR,
    ZERO,
    ONE,
    NATVAL,
    INT_EXP,
    IBIT,
    SPECIAL,
    D,
    O,
    U,
    I,
    PSR_MFL,
    PSR_MFH,
    PSR_DFL,
    PSR_DFH,
    Knobs,
    Fault,
    Out,
    model_env,
    fma_core,
    finish_scalar,
    finish_lane,
    is_natval,
    is_inf,
    is_unorm,
    is_unsupported,
    is_zero_valued,
    is_finite,
    relation,
    cmp_flags,
    minmax,
    to_int,
    normalize,
    recip_approx,
    rsqrt_approx,
    frcpa_core,
    frsqrta_core,
    fclass_member,
    fr_to_single,
    fr_to_double,
    single_to_fr,
    double_to_fr,
    integer,
)

# ---------------------------------------------------------------- encoders
FMA_MAJOR = {
    ("fma", None): (8, 0),
    ("fma", "s"): (8, 1),
    ("fma", "d"): (9, 0),
    ("fms", None): (0xA, 0),
    ("fms", "s"): (0xA, 1),
    ("fms", "d"): (0xB, 0),
    ("fnma", None): (0xC, 0),
    ("fnma", "s"): (0xC, 1),
    ("fnma", "d"): (0xD, 0),
    ("fpma", None): (9, 1),
    ("fpms", None): (0xB, 1),
    ("fpnma", None): (0xD, 1),
}
F8_X6 = {"fmin": 0x14, "fmax": 0x15, "famin": 0x16, "famax": 0x17}
FPCMP_X6 = {
    "eq": 0x30,
    "lt": 0x31,
    "le": 0x32,
    "unord": 0x33,
    "neq": 0x34,
    "nlt": 0x35,
    "nle": 0x36,
    "ord": 0x37,
}
F9_X6 = {
    "fmerge.s": (0, 0x10),
    "fmerge.ns": (0, 0x11),
    "fmerge.se": (0, 0x12),
    "fmix.lr": (0, 0x39),
    "fmix.r": (0, 0x3A),
    "fmix.l": (0, 0x3B),
    "fsxt.r": (0, 0x3C),
    "fsxt.l": (0, 0x3D),
    "fpack": (0, 0x28),
    "fswap": (0, 0x34),
    "fswap.nl": (0, 0x35),
    "fswap.nr": (0, 0x36),
    "fand": (0, 0x2C),
    "fandcm": (0, 0x2D),
    "for": (0, 0x2E),
    "fxor": (0, 0x2F),
    "fpmerge.s": (1, 0x10),
    "fpmerge.ns": (1, 0x11),
    "fpmerge.se": (1, 0x12),
}
FCVT_X6 = {"fx": 0x18, "fxu": 0x19, "fx.trunc": 0x1A, "fxu.trunc": 0x1B}
XMA_X2 = {"xma.l": 0, "xma.hu": 2, "xma.h": 3}
SETF_X6 = {"sig": 0x1C, "exp": 0x1D, "s": 0x1E, "d": 0x1F}


def encode(i):
    """Returns (unit, raw41) for an instruction description dict."""
    op = i["op"]
    qp = i.get("qp", 0)
    sf = i.get("sf", 0)
    if op in ("fma", "fms", "fnma", "fpma", "fpms", "fpnma"):
        major, x = FMA_MAJOR[(op, i.get("pc"))]
        return "F", (major << 37) | (x << 36) | (sf << 34) | (i["f4"] << 27) | (i["f3"] << 20) | (
            i["f2"] << 13
        ) | (i["f1"] << 6) | qp
    if op in XMA_X2:
        return "F", (0xE << 37) | (1 << 36) | (XMA_X2[op] << 34) | (i["f4"] << 27) | (
            i["f3"] << 20
        ) | (i["f2"] << 13) | (i["f1"] << 6) | qp
    if op == "fselect":
        return "F", (0xE << 37) | (i["f4"] << 27) | (i["f3"] << 20) | (i["f2"] << 13) | (
            i["f1"] << 6
        ) | qp
    if op == "fcmp":
        ra, rb = {"eq": (0, 0), "lt": (0, 1), "le": (1, 0), "unord": (1, 1)}[i["rel"]]
        return "F", (4 << 37) | (rb << 36) | (sf << 34) | (ra << 33) | (i["p2"] << 27) | (
            i["f3"] << 20
        ) | (i["f2"] << 13) | (int(i.get("unc", 0)) << 12) | (i["p1"] << 6) | qp
    if op == "fclass":
        c9 = i["c9"]
        return "F", (5 << 37) | ((c9 & 3) << 33) | (i["p2"] << 27) | ((c9 >> 2) << 20) | (
            i["f2"] << 13
        ) | (int(i.get("unc", 0)) << 12) | (i["p1"] << 6) | qp
    if op in ("frcpa", "fprcpa"):
        return "F", ((op == "fprcpa") << 37) | (sf << 34) | (1 << 33) | (i["p2"] << 27) | (
            i["f3"] << 20
        ) | (i["f2"] << 13) | (i["f1"] << 6) | qp
    if op in ("frsqrta", "fprsqrta"):
        return "F", ((op == "fprsqrta") << 37) | (1 << 36) | (sf << 34) | (1 << 33) | (
            i["p2"] << 27
        ) | (i["f3"] << 20) | (i["f1"] << 6) | qp
    if op in F8_X6 or op in ("fpmin", "fpmax", "fpamin", "fpamax") or op == "fpcmp":
        if op == "fpcmp":
            major, x6 = 1, FPCMP_X6[i["rel"]]
        elif op in F8_X6:
            major, x6 = 0, F8_X6[op]
        else:
            major, x6 = 1, F8_X6["f" + op[2:]]
        return "F", (major << 37) | (sf << 34) | (x6 << 27) | (i["f3"] << 20) | (i["f2"] << 13) | (
            i["f1"] << 6
        ) | qp
    if op in F9_X6:
        major, x6 = F9_X6[op]
        return "F", (major << 37) | (x6 << 27) | (i["f3"] << 20) | (i["f2"] << 13) | (
            i["f1"] << 6
        ) | qp
    if op in ("fcvt", "fpcvt"):
        return "F", ((op == "fpcvt") << 37) | (sf << 34) | (FCVT_X6[i["form"]] << 27) | (
            i["f2"] << 13
        ) | (i["f1"] << 6) | qp
    if op == "fcvt.xf":
        return "F", (0x1C << 27) | (i["f2"] << 13) | (i["f1"] << 6) | qp
    if op == "fsetc":
        return "F", (sf << 34) | (0x04 << 27) | (i["omask"] << 20) | (i["amask"] << 13) | qp
    if op == "fclrf":
        return "F", (sf << 34) | (0x05 << 27) | qp
    if op == "fchkf":
        imm = i["imm21"] & ((1 << 21) - 1)
        return "F", ((imm >> 20) << 36) | (sf << 34) | (0x08 << 27) | ((imm & 0xFFFFF) << 6) | qp
    if op == "setf":
        return "M", (6 << 37) | (SETF_X6[i["form"]] << 30) | (1 << 27) | (i["r2"] << 13) | (
            i["f1"] << 6
        ) | qp
    if op == "getf":
        return "M", (4 << 37) | (SETF_X6[i["form"]] << 30) | (1 << 27) | (i["f2"] << 13) | (
            i["r1"] << 6
        ) | qp
    raise ValueError(op)


# ---------------------------------------------------------------- state
OUT_FRS = (6, 7, 8, 9, 32)


def new_state():
    return {"fpsr": 0, "psr": 0, "pr": 1, "fr": {}, "gr": {}, "nat": {}, "m0": 0, "m1": 0}


def rd(st, n):
    if n == 0:
        return ZERO
    if n == 1:
        return ONE
    return st["fr"].get(n, ZERO)


def snapshot(st):
    o = {
        "kind": "complete",
        "fault": "none",
        "info": 0,
        "exit": 0,
        "fpsr": st["fpsr"],
        "psr": st["psr"],
        "pr": st["pr"] | 1,
    }
    for n in OUT_FRS:
        o["f%d" % n] = rd(st, n).fmt()
    o["r8"] = st["gr"].get(8, 0)
    o["n8"] = int(st["nat"].get(8, False))
    o["r20"] = st["gr"].get(20, 0)
    o["n20"] = int(st["nat"].get(20, False))
    o["m0"], o["m1"] = st["m0"], st["m1"]
    return o


def to_line(insn, st):
    unit, raw = encode(insn)
    toks = [
        "u=" + unit,
        "raw=%x" % raw,
        "fpsr=%x" % st["fpsr"],
        "psr=%x" % st["psr"],
        "pr=%x" % st["pr"],
    ]
    for n, v in sorted(st["fr"].items()):
        toks.append("f%d=%s" % (n, v.fmt()))
    for n, v in sorted(st["gr"].items()):
        toks.append("r%d=%x" % (n, v))
    for n, v in sorted(st["nat"].items()):
        toks.append("n%d=%d" % (n, int(v)))
    if st["m0"] or st["m1"]:
        toks.append("m0=%x" % st["m0"])
        toks.append("m1=%x" % st["m1"])
    return " ".join(toks)


def controls(fpsr, sf):
    return (fpsr >> (6 + 13 * sf)) & 0x7F


def trap_mask(fpsr, sf):
    """1 bits = disabled. sfx.td forces all disabled; sf0.td is reserved."""
    if sf and controls(fpsr, sf) & 0x40:
        return 0x3F
    return fpsr & 0x3F


def env_for(st, sf, pc, parallel=False):
    return model_env(pc, controls(st["fpsr"], sf), trap_mask(st["fpsr"], sf), parallel)


class Result:
    def __init__(self, st):
        self.st = copy.deepcopy(st)
        self.kind = "complete"
        self.fault = "none"
        self.info = 0
        self.notes = set()  # tags for coverage / ambiguity accounting
        self.swa_ok = False  # an SWA exit is architecturally permissible here
        self.unspecified = set()  # output fields whose value Rev1.1 leaves open

    def write_fr(self, n, v):
        self.st["fr"][n] = v
        if 2 <= n <= 31:
            self.st["psr"] |= PSR_MFL
        elif n >= 32:
            self.st["psr"] |= PSR_MFH

    def write_pr(self, n, v):
        if n:
            if v:
                self.st["pr"] |= 1 << n
            else:
                self.st["pr"] &= ~(1 << n)

    def flags(self, sf, f):
        self.st["fpsr"] |= f << (13 + 13 * sf)

    def output(self, orig):
        if self.kind == "fault" and self.fault != "fp_trap":
            o = snapshot(orig)
        else:
            o = snapshot(self.st)
        o["kind"], o["fault"], o["info"] = self.kind, self.fault, self.info
        return o


def fault(res, name, info=0):
    res.kind, res.fault, res.info = "fault", name, info
    return res


def disabled(st, regs):
    code = 0
    for r in regs:
        if 2 <= r <= 31 and st["psr"] & PSR_DFL:
            code |= 1
        if r >= 32 and st["psr"] & PSR_DFH:
            code |= 2
    return code


def pred(st, n):
    return n == 0 or bool((st["pr"] >> n) & 1)


def trap_code(trap, fpa, lane_hi=True):
    """Vol2 Table 8-3: O/U/I/FPA at bits 11-14 (normal / FP-HI), 7-10 (FP-LO)."""
    base = 11 if lane_hi else 7
    c = 0
    if trap & O:
        c |= 1 << base
    if trap & U:
        c |= 1 << (base + 1)
    if trap & I:
        c |= 1 << (base + 2)
    if trap and fpa:
        c |= 1 << (base + 3)
    return c


def commit_scalar(res, sf, f1, out, value):
    res.flags(sf, out.flags)
    res.write_fr(f1, value)
    if out.trap:
        fault(res, "fp_trap", 1 | trap_code(out.trap, out.fpa))


# ---------------------------------------------------------------- execute
def execute(i, st, knobs=None):
    knobs = knobs or Knobs()
    res = Result(st)
    op = i["op"]
    qp_true = pred(st, i.get("qp", 0))
    sf = i.get("sf", 0)
    if not qp_true:
        # Vol3: frcpa/frsqrta clear p2; fcmp/fclass .unc clear p1 and p2.
        if op in ("frcpa", "fprcpa", "frsqrta", "fprsqrta"):
            res.write_pr(i["p2"], False)
        if op in ("fcmp", "fclass") and i.get("unc"):
            res.write_pr(i["p1"], False)
            res.write_pr(i["p2"], False)
        return res
    try:
        return EXEC[op](i, st, res, sf, knobs)
    except Fault as f:
        return fault(res, "fp", f.bits)


def ex_fma(i, st, res, sf, knobs):
    f1, f2, f3, f4 = i["f1"], i["f2"], i["f3"], i["f4"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2, f3, f4)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2, f3, f4)))
    a, b, c = rd(st, f2), rd(st, f3), rd(st, f4)
    if is_natval(a) or is_natval(b) or is_natval(c):
        res.write_fr(f1, NATVAL)
        return res
    env = env_for(st, sf, i.get("pc"))
    if env is None:
        # Dynamic pc=01 is "Reserved" in Table 5-6; Rev 1.1 defines no
        # behaviour for arithmetic executed with it, so nothing is compared.
        res.notes.add("reserved-pc")
        res.unspecified.add("*")
        return fault(res, "reserved", 0x30)  # dynamic pc=01 (Table 5-6: reserved)
    out = fma_core(i["op"], a, b, c, f2 == 0, env, knobs)
    if out.tags:
        res.notes |= out.tags
    commit_scalar(res, sf, f1, out, finish_scalar(out, env))
    return res


def lanes(r):
    return [single_to_fr((r.sig >> 32) & 0xFFFFFFFF), single_to_fr(r.sig & 0xFFFFFFFF)]


def parallel_commit(res, sf, f1, lane_outs, values):
    """lane_outs[0] = HI, [1] = LO."""
    faults = 0
    for k, lo in enumerate(lane_outs):
        if isinstance(lo, Fault):
            faults |= lo.bits << (0 if k == 0 else 4)
    if faults:
        return fault(res, "fp", faults)
    for lo in lane_outs:
        res.flags(sf, lo.flags)
    res.write_fr(f1, integer((values[0] << 32) | values[1]))
    code = 0
    for k, lo in enumerate(lane_outs):
        code |= trap_code(lo.trap, lo.fpa, lane_hi=(k == 0))
    if code:
        fault(res, "fp_trap", 1 | code)
    return res


def run_lane(fn):
    try:
        return fn()
    except Fault as f:
        return f


def ex_fpma(i, st, res, sf, knobs):
    f1, f2, f3, f4 = i["f1"], i["f2"], i["f3"], i["f4"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2, f3, f4)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2, f3, f4)))
    ra, rb, rc = rd(st, f2), rd(st, f3), rd(st, f4)
    if is_natval(ra) or is_natval(rb) or is_natval(rc):
        res.write_fr(f1, NATVAL)
        return res
    env = env_for(st, sf, None, parallel=True)
    op = {"fpma": "fma", "fpms": "fms", "fpnma": "fnma"}[i["op"]]
    A, B, C = lanes(ra), lanes(rb), lanes(rc)
    outs = [
        run_lane(lambda k=k: fma_core(op, A[k], B[k], C[k], f2 == 0, env, knobs)) for k in range(2)
    ]
    for o in outs:
        if isinstance(o, Out) and o.tags:
            res.notes |= {"lane-" + t for t in o.tags}
    vals = [0 if isinstance(o, Fault) else finish_lane(o, knobs) for o in outs]
    return parallel_commit(res, sf, f1, outs, vals)


def ex_xma(i, st, res, sf, knobs):
    f1, f2, f3, f4 = i["f1"], i["f2"], i["f3"], i["f4"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2, f3, f4)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2, f3, f4)))
    a, b, c = rd(st, f2), rd(st, f3), rd(st, f4)
    if is_natval(a) or is_natval(b) or is_natval(c):
        res.write_fr(f1, NATVAL)
        return res
    x, y = b.sig, c.sig
    if i["op"] in ("xma.l", "xma.h"):
        x = x - (1 << 64) if x >> 63 else x
        y = y - (1 << 64) if y >> 63 else y
    t = (x * y + a.sig) & ((1 << 128) - 1)
    res.write_fr(f1, integer(t >> 64 if i["op"] in ("xma.h", "xma.hu") else t))
    return res


def ex_fselect(i, st, res, sf, knobs):
    f1, f2, f3, f4 = i["f1"], i["f2"], i["f3"], i["f4"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2, f3, f4)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2, f3, f4)))
    a, b, c = rd(st, f2), rd(st, f3), rd(st, f4)
    if is_natval(a) or is_natval(b) or is_natval(c):
        res.write_fr(f1, NATVAL)
        return res
    res.write_fr(f1, integer((b.sig & a.sig) | (c.sig & ~a.sig)))
    return res


def ex_fcmp(i, st, res, sf, knobs):
    p1, p2, f2, f3 = i["p1"], i["p2"], i["f2"], i["f3"]
    if p1 == p2:
        return fault(res, "illegal")
    if disabled(st, (f2, f3)):
        return fault(res, "fp_disabled", disabled(st, (f2, f3)))
    a, b = rd(st, f2), rd(st, f3)
    if is_natval(a) or is_natval(b):
        res.write_pr(p1, False)
        res.write_pr(p2, False)
        return res
    env = env_for(st, sf, "x")
    out = Out()
    cmp_flags(a, b, i["rel"] in ("lt", "le"), env, out)
    r = relation(i["rel"], a, b, knobs)
    if is_unsupported(a) or is_unsupported(b):
        res.notes.add("fcmp-unsupported")
    res.flags(sf, out.flags)
    res.write_pr(p1, r)
    res.write_pr(p2, not r)
    return res


def ex_fclass(i, st, res, sf, knobs):
    p1, p2, f2 = i["p1"], i["p2"], i["f2"]
    if p1 == p2:
        return fault(res, "illegal")
    if disabled(st, (f2,)):
        return fault(res, "fp_disabled", disabled(st, (f2,)))
    a = rd(st, f2)
    r = fclass_member(a, i["c9"])
    if is_natval(a) and not (i["c9"] & 0x100):
        res.write_pr(p1, False)
        res.write_pr(p2, False)
    else:
        res.write_pr(p1, r)
        res.write_pr(p2, not r)
    return res


def ex_minmax(i, st, res, sf, knobs):
    f1, f2, f3 = i["f1"], i["f2"], i["f3"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2, f3)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2, f3)))
    a, b = rd(st, f2), rd(st, f3)
    if is_natval(a) or is_natval(b):
        res.write_fr(f1, NATVAL)
        return res
    env = env_for(st, sf, "x")
    out = minmax(i["op"], a, b, env, knobs)
    res.flags(sf, out.flags)
    res.write_fr(f1, out.value)
    return res


def ex_pminmax(i, st, res, sf, knobs):
    f1, f2, f3 = i["f1"], i["f2"], i["f3"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2, f3)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2, f3)))
    ra, rb = rd(st, f2), rd(st, f3)
    if is_natval(ra) or is_natval(rb):
        res.write_fr(f1, NATVAL)
        return res
    env = env_for(st, sf, None, parallel=True)
    A, B = lanes(ra), lanes(rb)
    if i["op"] == "fpcmp":
        rel = i["rel"]
        base = {"neq": "eq", "nlt": "lt", "nle": "le", "ord": "unord"}.get(rel, rel)
        neg = base != rel

        def lane(k):
            out = Out()
            cmp_flags(A[k], B[k], base in ("lt", "le"), env, out)
            out.value = 0xFFFFFFFF if relation(base, A[k], B[k], knobs) != neg else 0
            return out
    else:

        def lane(k):
            return minmax("f" + i["op"][2:], A[k], B[k], env, knobs)

    outs = [run_lane(lambda k=k: lane(k)) for k in range(2)]
    vals = []
    for o in outs:
        if isinstance(o, Fault):
            vals.append(0)
        elif isinstance(o.value, FR):
            vals.append(fr_to_single(o.value))
        else:
            vals.append(o.value)
    return parallel_commit(res, sf, f1, outs, vals)


def ex_f9(i, st, res, sf, knobs):
    f1, f2, f3 = i["f1"], i["f2"], i["f3"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2, f3)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2, f3)))
    a, b = rd(st, f2), rd(st, f3)
    if is_natval(a) or is_natval(b):
        res.write_fr(f1, NATVAL)
        return res
    op = i["op"]
    A, B = a.sig, b.sig
    hi = lambda x: (x >> 32) & 0xFFFFFFFF
    lo = lambda x: x & 0xFFFFFFFF
    if op == "fmerge.s":
        v = FR(a.sign, b.exp, b.sig)
    elif op == "fmerge.ns":
        v = FR(a.sign ^ 1, b.exp, b.sig)
    elif op == "fmerge.se":
        v = FR(a.sign, a.exp, b.sig)
    else:
        if op == "fmix.l":
            r = (hi(A) << 32) | hi(B)
        elif op == "fmix.r":
            r = (lo(A) << 32) | lo(B)
        elif op == "fmix.lr":
            r = (hi(A) << 32) | lo(B)
        elif op == "fsxt.l":
            r = ((0xFFFFFFFF if A >> 63 else 0) << 32) | hi(B)
        elif op == "fsxt.r":
            r = ((0xFFFFFFFF if (A >> 31) & 1 else 0) << 32) | lo(B)
        elif op == "fpack":
            r = (fr_to_single(a) << 32) | fr_to_single(b)
        elif op == "fswap":
            r = (lo(B) << 32) | hi(A)
        elif op == "fswap.nl":
            r = ((lo(B) ^ 0x80000000) << 32) | hi(A)
        elif op == "fswap.nr":
            r = (lo(B) << 32) | (hi(A) ^ 0x80000000)
        elif op == "fand":
            r = A & B
        elif op == "fandcm":
            r = A & ~B
        elif op == "for":
            r = A | B
        elif op == "fxor":
            r = A ^ B
        elif op in ("fpmerge.s", "fpmerge.ns"):
            m = 0x8000000080000000
            r = (A & m) | (B & ~m)
            if op == "fpmerge.ns":
                r ^= m
        elif op == "fpmerge.se":
            m = 0xFF800000FF800000
            r = (A & m) | (B & ~m)
        v = integer(r)
    res.write_fr(f1, v)
    return res


def ex_fcvt(i, st, res, sf, knobs):
    f1, f2 = i["f1"], i["f2"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2)))
    a = rd(st, f2)
    if is_natval(a):
        res.write_fr(f1, NATVAL)
        return res
    form = i["form"]
    signed, trunc = form.startswith("fx.") or form == "fx", form.endswith("trunc")
    if i["op"] == "fcvt":
        env = env_for(st, sf, "x")
        out = to_int(a, signed, trunc, 64, env, knobs)
        commit_scalar(res, sf, f1, out, integer(out.value))
        return res
    env = env_for(st, sf, None, parallel=True)
    A = lanes(a)
    outs = [run_lane(lambda k=k: to_int(A[k], signed, trunc, 32, env, knobs)) for k in range(2)]
    vals = [0 if isinstance(o, Fault) else o.value for o in outs]
    return parallel_commit(res, sf, f1, outs, vals)


def ex_fcvt_xf(i, st, res, sf, knobs):
    f1, f2 = i["f1"], i["f2"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2)))
    a = rd(st, f2)
    if is_natval(a):
        res.write_fr(f1, NATVAL)
        return res
    s = a.sig >> 63
    m = ((~a.sig + 1) & ((1 << 64) - 1)) if s else a.sig
    if m == 0:
        v = FR(0, 0, 0)
    else:
        k = 64 - m.bit_length()
        v = FR(s, INT_EXP - k, m << k)
    res.write_fr(f1, v)
    return res


def ex_frcpa(i, st, res, sf, knobs):
    f1, f2, f3, p2 = i["f1"], i["f2"], i["f3"], i["p2"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2, f3)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2, f3)))
    a, b = rd(st, f2), rd(st, f3)
    if is_natval(a) or is_natval(b):
        res.write_fr(f1, NATVAL)
        res.write_pr(p2, False)
        return res
    env = env_for(st, sf, "x")
    out, p, special = frcpa_core(a, b, env)
    if out.value is None:
        if not special and (outside_ext(a) or outside_ext(b)):
            # Vol1 12.3.3: frcpa raises SWA only "when the operand values are
            # well outside the ... double-extended precision ranges".
            res.swa_ok = True  # limits check precedes the D check
        if is_unorm(a) or is_unorm(b):
            if env.enabled(D):
                raise Fault(D)
            out.flags |= D
        if is_inf(a):
            out.value, p = FR(a.sign ^ b.sign, SPECIAL, IBIT), False
        elif is_inf(b) or is_zero_valued(a):
            out.value, p = FR(a.sign ^ b.sign, 0, 0), False
        else:
            out.value, p = recip_approx(normalize(b)), True
            res.notes.add("frcpa-approx")
    res.flags(sf, out.flags)
    res.write_fr(f1, out.value)
    res.write_pr(p2, p)
    return res


def ex_frsqrta(i, st, res, sf, knobs):
    f1, f3, p2 = i["f1"], i["f3"], i["p2"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f3)):
        return fault(res, "fp_disabled", disabled(st, (f1, f3)))
    b = rd(st, f3)
    if is_natval(b):
        res.write_fr(f1, NATVAL)
        res.write_pr(p2, False)
        return res
    env = env_for(st, sf, "x")
    out, handled = frsqrta_core(b, env)
    p = False
    if not handled:
        if not (is_zero_valued(b) or is_inf(b)) and outside_ext(b):
            res.swa_ok = True
        if is_unorm(b):
            if env.enabled(D):
                raise Fault(D)
            out.flags |= D
        if is_zero_valued(b):
            out.value = FR(b.sign, 0, 0)
        elif is_inf(b):
            out.value = b
        else:
            out.value, p = rsqrt_approx(normalize(b)), True
            res.notes.add("frsqrta-approx")
    res.flags(sf, out.flags)
    res.write_fr(f1, out.value)
    res.write_pr(p2, p)
    return res


def outside_ext(r):
    """Finite nonzero value outside the double-extended range (incl. denormals)."""
    if not is_finite(r) or is_zero_valued(r):
        return False
    e = normalize(r)[2]
    return not (0xC001 - 63 <= e <= 0x13FFE)


def lane_frcpa(a, b, env, res, k):
    out, p, special = frcpa_core(a, b, env)
    if out.value is not None:
        return out, fr_to_single(out.value), False
    if is_unorm(a) or is_unorm(b):
        if env.enabled(D):
            raise Fault(D)
        out.flags |= D
    if is_inf(a):
        return out, ((a.sign ^ b.sign) << 31) | 0x7F800000, False
    if is_inf(b) or is_zero_valued(a):
        return out, (a.sign ^ b.sign) << 31, False
    res.notes.add("fprcpa-approx")
    r = recip_approx(normalize(b))
    if not (0xFF81 <= r.exp <= 0x1007E):
        # Reciprocal outside the single range: fprcpa's limits_check.fr3
        # selects a default result that Rev 1.1 does not define, and the
        # predicate limits are not given either.
        res.unspecified.add("f%d" % res.cur_f1)
        res.notes.add("fprcpa-recip-out-of-single-range")
        return out, 0, None
    return out, fr_to_single(r), None  # None: predicate limits not specified


def lane_frsqrta(b, env, res):
    out, handled = frsqrta_core(b, env)
    if handled:
        return out, fr_to_single(out.value), False
    if is_unorm(b):
        if env.enabled(D):
            raise Fault(D)
        out.flags |= D
    if is_zero_valued(b):
        return out, (b.sign << 31) | 0x7F800000, False
    if is_inf(b):
        return out, 0, False
    res.notes.add("fprsqrta-approx")
    return out, fr_to_single(rsqrt_approx(normalize(b))), None


def ex_fprcpa(i, st, res, sf, knobs):
    sq = i["op"] == "fprsqrta"
    f1, f3, p2 = i["f1"], i["f3"], i["p2"]
    f2 = 0 if sq else i["f2"]
    regs = (f1, f3) if sq else (f1, f2, f3)
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, regs):
        return fault(res, "fp_disabled", disabled(st, regs))
    ra, rb = rd(st, f2), rd(st, f3)
    if (not sq and is_natval(ra)) or is_natval(rb):
        res.write_fr(f1, NATVAL)
        res.write_pr(p2, False)
        return res
    res.cur_f1 = f1
    env = env_for(st, sf, None, parallel=True)
    A, B = lanes(ra), lanes(rb)
    # Any lane input that is a single denormal: SWA is permitted
    # (Vol1 5.4.1.2: SWA may be signalled at any time without NaTVal input).
    res.swa_ok = True
    outs, vals, preds = [], [], []
    for k in range(2):
        try:
            o, v, p = lane_frsqrta(B[k], env, res) if sq else lane_frcpa(A[k], B[k], env, res, k)
        except Fault as f:
            o, v, p = f, 0, False
        outs.append(o)
        vals.append(v)
        preds.append(p)
    r = parallel_commit(res, sf, f1, outs, vals)
    if r.kind == "fault" and r.fault == "fp":
        return r
    if False in preds:
        res.write_pr(p2, False)
    else:
        res.unspecified.add("pr")  # limits for predicate=1 not given in Rev1.1
        res.write_pr(p2, True)
    return res


def ex_fsetc(i, st, res, sf, knobs):
    val = (controls(st["fpsr"], 0) & i["amask"]) | i["omask"]
    # is_reserved_field(FSETC, sf, controls): sf0.td reserved; pc=01 reserved
    if (sf == 0 and val & 0x40) or ((val >> 2) & 3) == 1:
        res.notes.add("fsetc-reserved")
        return fault(res, "reserved", 0x30)  # Vol2: ISR.code{7:4}=3
    sh = 6 + 13 * sf
    res.st["fpsr"] = (st["fpsr"] & ~(0x7F << sh)) | (val << sh)
    return res


def ex_fclrf(i, st, res, sf, knobs):
    res.st["fpsr"] = st["fpsr"] & ~(0x3F << (13 + 13 * sf))
    return res


def ex_fchkf(i, st, res, sf, knobs):
    fl = (st["fpsr"] >> (13 + 13 * sf)) & 0x3F
    if (fl & ~(st["fpsr"] & 0x3F)) or (fl & ~((st["fpsr"] >> 13) & 0x3F)):
        imm = i["imm21"] & ((1 << 21) - 1)
        if imm >> 20:
            imm -= 1 << 21
        res.kind, res.info = "branch", (0x100000 + (imm << 4)) & ((1 << 64) - 1)
    return res


def ex_setf(i, st, res, sf, knobs):
    f1, r2 = i["f1"], i["r2"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1,)):
        return fault(res, "fp_disabled", disabled(st, (f1,)))
    g = st["gr"].get(r2, 0) if r2 else 0
    if r2 and st["nat"].get(r2, False):
        v = NATVAL
    elif i["form"] == "s":
        v = single_to_fr(g & 0xFFFFFFFF)
    elif i["form"] == "d":
        v = double_to_fr(g)
    elif i["form"] == "sig":
        v = integer(g)
    else:
        v = FR((g >> 17) & 1, g & 0x1FFFF, IBIT)
    res.write_fr(f1, v)
    return res


def ex_getf(i, st, res, sf, knobs):
    r1, f2 = i["r1"], i["f2"]
    if r1 == 0:
        return fault(res, "illegal")
    if disabled(st, (f2,)):
        return fault(res, "fp_disabled", disabled(st, (f2,)))
    a = rd(st, f2)
    form = i["form"]
    if form == "s":
        g = fr_to_single(a)
    elif form == "d":
        g = fr_to_double(a)
    elif form == "exp":
        g = (a.sign << 17) | a.exp
    else:
        g = a.sig
    res.st["gr"][r1] = g
    res.st["nat"][r1] = is_natval(a)
    return res


EXEC = {
    "fma": ex_fma,
    "fms": ex_fma,
    "fnma": ex_fma,
    "fpma": ex_fpma,
    "fpms": ex_fpma,
    "fpnma": ex_fpma,
    "xma.l": ex_xma,
    "xma.h": ex_xma,
    "xma.hu": ex_xma,
    "fselect": ex_fselect,
    "fcmp": ex_fcmp,
    "fclass": ex_fclass,
    "fmin": ex_minmax,
    "fmax": ex_minmax,
    "famin": ex_minmax,
    "famax": ex_minmax,
    "fpmin": ex_pminmax,
    "fpmax": ex_pminmax,
    "fpamin": ex_pminmax,
    "fpamax": ex_pminmax,
    "fpcmp": ex_pminmax,
    "fcvt": ex_fcvt,
    "fpcvt": ex_fcvt,
    "fcvt.xf": ex_fcvt_xf,
    "frcpa": ex_frcpa,
    "frsqrta": ex_frsqrta,
    "fprcpa": ex_fprcpa,
    "fprsqrta": ex_fprcpa,
    "fsetc": ex_fsetc,
    "fclrf": ex_fclrf,
    "fchkf": ex_fchkf,
    "setf": ex_setf,
    "getf": ex_getf,
}
for _k in F9_X6:
    EXEC[_k] = ex_f9


# ---------------------------------------------------------------- memory
MEMBASE = 0x108000
LDF_X6 = {
    "ldfe": 0x00,
    "ldf8": 0x01,
    "ldfs": 0x02,
    "ldfd": 0x03,
    "ldf.fill": 0x1B,
    "ldfe.s": 0x04,
    "ldf8.s": 0x05,
    "ldfs.s": 0x06,
    "ldfd.s": 0x07,
}
STF_X6 = {"stfe": 0x30, "stf8": 0x31, "stfs": 0x32, "stfd": 0x33, "stf.spill": 0x3B}
LDFP_X6 = {"ldfp8": 0x01, "ldfps": 0x02, "ldfpd": 0x03}
MEM_SIZE = {"e": 10, "8": 8, "s": 4, "d": 8, "fill": 16, "spill": 16}


def encode_mem(i):
    op, qp = i["op"], i.get("qp", 0)
    imm = i.get("imm")
    if op in LDF_X6:
        if imm is None:
            return "M", (6 << 37) | (LDF_X6[op] << 30) | (20 << 20) | (i["f1"] << 6) | qp
        imm &= 0x1FF
        return "M", (7 << 37) | ((imm >> 8) << 36) | (LDF_X6[op] << 30) | (
            ((imm >> 7) & 1) << 27
        ) | (20 << 20) | ((imm & 0x7F) << 13) | (i["f1"] << 6) | qp
    if op in STF_X6:
        if imm is None:
            return "M", (6 << 37) | (STF_X6[op] << 30) | (20 << 20) | (i["f2"] << 13) | qp
        imm &= 0x1FF
        return "M", (7 << 37) | ((imm >> 8) << 36) | (STF_X6[op] << 30) | (
            ((imm >> 7) & 1) << 27
        ) | (20 << 20) | (i["f2"] << 13) | ((imm & 0x7F) << 6) | qp
    if op in LDFP_X6:
        return "M", (6 << 37) | (int(bool(i.get("post"))) << 36) | (LDFP_X6[op] << 30) | (
            1 << 27
        ) | (20 << 20) | (i["f2"] << 13) | (i["f1"] << 6) | qp
    raise ValueError(op)


_encode_fp = encode


def encode(i):  # noqa: F811
    if i["op"] in LDF_X6 or i["op"] in STF_X6 or i["op"] in LDFP_X6:
        return encode_mem(i)
    return _encode_fp(i)


def mem_bytes(st):
    return (st["m0"] | (st["m1"] << 64)).to_bytes(16, "little")


def set_mem(res, data):
    v = int.from_bytes(data, "little")
    res.st["m0"], res.st["m1"] = v & ((1 << 64) - 1), v >> 64


def fr_from_ext(lo, se):
    s, e = (se >> 15) & 1, se & 0x7FFF
    return FR(s, SPECIAL if e == 0x7FFF else (e + 0xC000 if e else 0), lo)


def ex_ldf(i, st, res, sf, knobs):
    op = i["op"]
    f1 = i["f1"]
    if f1 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1,)):
        return fault(res, "fp_disabled", disabled(st, (f1,)))
    spec = op.endswith(".s")
    base = op[:-2] if spec else op
    if st["nat"].get(20) and not spec:
        return fault(res, "nat")
    addr = st["gr"].get(20, 0)
    if st["nat"].get(20):
        v = NATVAL
    else:
        off = addr - MEMBASE
        data = mem_bytes(st)
        lo = int.from_bytes(data[off : off + 8], "little")
        if base == "ldfs":
            v = single_to_fr(int.from_bytes(data[off : off + 4], "little"))
        elif base == "ldfd":
            v = double_to_fr(lo)
        elif base == "ldf8":
            v = integer(lo)
        elif base == "ldfe":
            v = fr_from_ext(lo, int.from_bytes(data[off + 8 : off + 10], "little"))
        else:  # ldf.fill
            hi = int.from_bytes(data[off + 8 : off + 16], "little")
            v = FR((hi >> 17) & 1, hi & 0x1FFFF, lo)
    res.write_fr(f1, v)
    if i.get("imm") is not None:
        res.st["gr"][20] = (addr + i["imm"]) & ((1 << 64) - 1)
    return res


def ex_stf(i, st, res, sf, knobs):
    op, f2 = i["op"], i["f2"]
    if disabled(st, (f2,)):
        return fault(res, "fp_disabled", disabled(st, (f2,)))
    if st["nat"].get(20):
        return fault(res, "nat")
    a = rd(st, f2)
    if op != "stf.spill" and is_natval(a):
        return fault(res, "nat")
    addr = st["gr"].get(20, 0)
    off = addr - MEMBASE
    data = bytearray(mem_bytes(st))
    if op == "stfs":
        data[off : off + 4] = fr_to_single(a).to_bytes(4, "little")
    elif op == "stfd":
        data[off : off + 8] = fr_to_double(a).to_bytes(8, "little")
    elif op == "stf8":
        data[off : off + 8] = a.sig.to_bytes(8, "little")
    elif op == "stfe":
        e15 = (((a.exp >> 16) & 1) << 14) | (a.exp & 0x3FFF)
        data[off : off + 10] = a.sig.to_bytes(8, "little") + ((a.sign << 15) | e15).to_bytes(
            2, "little"
        )
    else:
        data[off : off + 16] = a.sig.to_bytes(8, "little") + (a.exp | (a.sign << 17)).to_bytes(
            8, "little"
        )
    set_mem(res, bytes(data))
    if i.get("imm") is not None:
        res.st["gr"][20] = (addr + i["imm"]) & ((1 << 64) - 1)
    return res


def ex_ldfp(i, st, res, sf, knobs):
    op, f1, f2 = i["op"], i["f1"], i["f2"]
    if (f1 & 1) == (f2 & 1):  # fp_reg_bank_conflict (odd/even pair rule)
        return fault(res, "illegal")
    if f1 < 2 or f2 < 2:
        return fault(res, "illegal")
    if disabled(st, (f1, f2)):
        return fault(res, "fp_disabled", disabled(st, (f1, f2)))
    if st["nat"].get(20):
        return fault(res, "nat")
    data = mem_bytes(st)
    w0 = int.from_bytes(data[0:8], "little")
    w1 = int.from_bytes(data[8:16], "little")
    if op == "ldfps":
        v1, v2 = single_to_fr(w0 & 0xFFFFFFFF), single_to_fr(w0 >> 32)
    elif op == "ldfpd":
        v1, v2 = double_to_fr(w0), double_to_fr(w1)
    else:
        v1, v2 = integer(w0), integer(w1)
    res.write_fr(f1, v1)
    res.write_fr(f2, v2)
    if i.get("post"):
        res.st["gr"][20] = (st["gr"].get(20, 0) + (8 if op == "ldfps" else 16)) & ((1 << 64) - 1)
    return res


for _k in LDF_X6:
    EXEC[_k] = ex_ldf
for _k in STF_X6:
    EXEC[_k] = ex_stf
for _k in LDFP_X6:
    EXEC[_k] = ex_ldfp
