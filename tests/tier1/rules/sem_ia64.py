# SPDX-License-Identifier: GPL-2.0-or-later
"""IA-64 reference semantics of the Tier1 IR ops for the rule prover.

Each function follows the Operation pseudo-code of the IA-64 instruction
the op is lifted from (Intel Itanium SDM Rev 2.3, Vol.3 Chapter 2, cited
per function). NaT bits are tracked separately by the IR and
are not part of these values. Values are 64-bit z3 bit-vectors; I1 results
are z3 Bools, and I1 operands may be Bools or 0/1 bit-vectors (a register
holding an I1 value, rules.h). Immediates are the packed `imm` of
ir_ops.def, as Python ints or z3 bit-vectors.

conformance.py checks every function against ia64_execute() on decoded
instructions, including the qp = 0 lowerings.
"""

import z3

MASK64 = 2**64 - 1


def bv(x):
    return x if isinstance(x, z3.BitVecRef) else z3.BitVecVal(x & MASK64, 64)


def b1(x):
    """An I1 operand as a Bool."""
    return x if z3.is_bool(x) else x != 0


def as_bv(x):
    """An I1 Bool as the 0/1 register value; bit-vectors unchanged."""
    return z3.If(x, z3.BitVecVal(1, 64), z3.BitVecVal(0, 64)) if z3.is_bool(x) else bv(x)


def low_mask(n):
    """ones in bits n-1..0 (n = 1..64; SDM zero_ext/sign_ext widths)."""
    n = bv(n)
    return z3.If(z3.UGE(n, 64), z3.BitVecVal(MASK64, 64), (z3.BitVecVal(1, 64) << n) - 1)


def zero_ext(x, n):
    """SDM Vol.3 §2.2 pseudo-code zero_ext(value, pos): bits pos-1..0."""
    return x & low_mask(n)


def sign_ext(x, n):
    """SDM sign_ext(value, pos): bit pos-1 copied upwards (n = 1..64)."""
    s = z3.BitVecVal(64, 64) - bv(n)
    return (x << s) >> s


def ADD(a, b):
    """add r1 = r2, r3: GR[r1] = GR[r2] + GR[r3] (Vol.3 p.3:14). adds/addl
    (p.3:14, imm14/imm22 sign-extended) lift to ADD(r3, CONST imm); the
    `add r1 = r2, r3, 1` form to ADD(ADD(r2, r3), CONST 1)."""
    return a + b


def SUB(a, b):
    """sub r1 = r2, r3: GR[r1] = GR[r2] - GR[r3] (Vol.3 p.3:256); `sub r1 =
    r2, r3, 1` lifts to SUB(SUB(r2, r3), CONST 1), `sub r1 = imm8, r3` to
    SUB(CONST imm8, r3)."""
    return a - b


def AND(a, b):
    """and r1 = r2, r3 / imm8, r3 (Vol.3 p.3:18)."""
    return a & b


def ANDCM(a, b):
    """andcm r1 = r2, r3 / imm8, r3: first & ~GR[r3] (Vol.3 p.3:19)."""
    return a & ~b


def OR(a, b):
    """or r1 = r2, r3 / imm8, r3 (Vol.3 p.3:194)."""
    return a | b


def XOR(a, b):
    """xor r1 = r2, r3 / imm8, r3 (Vol.3 p.3:279)."""
    return a ^ b


def SHL_IMM(a, count):
    """shladd's shift and shl's immediate form (a pseudo-op of dep.z):
    GR[r2] << count (Vol.3 p.3:245 shladd, p.3:244 shl), count < 64."""
    return a << bv(count)


def SHL(a, count):
    """shl r1 = r2, r3: GR[r1] = (count > 63) ? 0 : GR[r2] << count, the count
    unsigned (Vol.3 p.3:244)."""
    return z3.If(z3.UGT(count, 63), z3.BitVecVal(0, 64), a << count)


def SHR(a, count):
    """shr r1 = r3, r2: arithmetic; count > 63 shifts by 63, i.e. sign fill
    (Vol.3 p.3:247)."""
    return a >> z3.If(z3.UGT(count, 63), z3.BitVecVal(63, 64), count)


def SHRU(a, count):
    """shr.u r1 = r3, r2: count > 63 gives 0 (Vol.3 p.3:247)."""
    return z3.If(z3.UGT(count, 63), z3.BitVecVal(0, 64), z3.LShR(a, count))


def field(packed):
    """FIELD layout (ir_ops.def): pos | len << 8, and the effective length
    of extr/dep: `if (pos6 + tmp_len u> 64) tmp_len = 64 - pos6`."""
    packed = bv(packed)
    pos, length = packed & 255, z3.LShR(packed, 8) & 255
    eff = z3.If(z3.UGT(pos + length, 64), z3.BitVecVal(64, 64) - pos, length)
    return pos, eff


def EXTRU(packed, a):
    """extr.u r1 = r3, pos6, len6 (Vol.3 p.3:54): zero_ext(shift_right_
    unsigned(GR[r3], pos6), tmp_len). shr.u r1 = r3, count (p.3:247) is
    extr.u r1 = r3, count, 64 - count."""
    pos, eff = field(packed)
    return zero_ext(z3.LShR(a, pos), eff)


def EXTR(packed, a):
    """extr r1 = r3, pos6, len6 (Vol.3 p.3:54): sign_ext(shift_right_
    unsigned(GR[r3], pos6), tmp_len); shr r1 = r3, count is extr r1 = r3,
    count, 64 - count."""
    pos, eff = field(packed)
    return sign_ext(z3.LShR(a, pos), eff)


def DEPZ(packed, a):
    """dep.z r1 = r2, pos6, len6 (Vol.3 p.3:51-52): GR[r1] = 0;
    GR[r1]{pos6 + tmp_len - 1:pos6} = tmp_src{tmp_len - 1:0}. The imm8 form
    lifts to DEPZ(CONST sign_ext(imm8, 8)); shl r1 = r2, count (p.3:244) is
    dep.z r1 = r2, count, 64 - count."""
    pos, eff = field(packed)
    return zero_ext(a, eff) << pos


def SXT(width, a):
    """sxt{1,2,4} r1 = r3 (Vol.3 p.3:258): sign_ext(GR[r3], xsz * 8)."""
    return sign_ext(a, width)


def ZXT(width, a):
    """zxt{1,2,4} r1 = r3 (Vol.3 p.3:280): zero_ext(GR[r3], xsz * 8)."""
    return zero_ext(a, width)


def CONST(value):
    """A constant: movl (Vol.3 p.3:187), mov r1 = imm22 (addl r1 = imm22,
    r0), folded contract state. An I1 constant is 0 or 1."""
    return bv(value)


def CMP(packed, a, b):
    """cmp/cmp4 crel (Vol.3 p.3:39-41 cmp, p.3:43-44 cmp4): rel = packed &
    255 (0 eq, 1 lt signed, 2 ltu unsigned), width = packed >> 8. cmp4
    compares bits 31:0, sign-extended for lt and zero-extended for ltu.
    The immediate forms lift to CMP(CONST imm8, r3) (the immediate is the
    first operand). Pseudo-op relations arrive normalized."""
    rel, width = packed & 255, packed >> 8
    if width == 32:
        a, b = z3.Extract(31, 0, a), z3.Extract(31, 0, b)
    elif width != 64:
        raise ValueError("CMP width %d" % width)
    return {0: a == b, 1: a < b, 2: z3.ULT(a, b)}[rel]


def NOT1(a):
    """Predicate complement: the p2 target of a normal compare (Vol.3 p.3:41
    `PR[p2] = !tmp_rel` for ctype none/unc)."""
    return z3.Not(b1(a))


def AND1(a, b):
    """Predicate conjunction: .unc compares `p1 = AND1(qp, c)` (Vol.3
    p.3:41: unc clears both targets when qp = 0), NaT masking."""
    return z3.And(b1(a), b1(b))


def OR1(a, b):
    """Predicate disjunction: NaT bits of the sources."""
    return z3.Or(b1(a), b1(b))


def SELECT(c, t, f):
    """c ? t : f: a predicated pure result and its previous value (Vol.1
    §4.3.3: qp = 0 leaves the target unchanged)."""
    if z3.is_bool(t) and z3.is_bool(f):
        return z3.If(b1(c), t, f)
    return z3.If(b1(c), as_bv(t), as_bv(f))


def CLOOP(lc):
    """br.cloop (Vol.3 p.3:26-27): tmp_taken = (AR[LC] != 0); if (AR[LC] != 0)
    AR[LC]--. Returns (next, taken)."""
    taken = lc != 0
    return z3.If(taken, lc - 1, lc), taken


def LOAD_VALUE(nbytes, word):
    """ld{1,2,4,8} (Vol.3 p.3:151-152): GR[r1] = zero_ext(mem_read(paddr,
    size), size * 8); word is the little-endian 64-bit memory image at the
    address (PSR.be = 0). .acq and the post-increment forms load the same
    value; the post-increment is ADD(base, imm9 | GR[r2]) after the access."""
    return zero_ext(word, 8 * nbytes)


def STORE_VALUE(nbytes, word, value):
    """st{1,2,4,8}[.rel] (Vol.3 p.3:251-252): mem_write(GR[r2], paddr, size)
    writes the low size * 8 bits; the other bytes of word are unchanged.
    The post-increment form is ADD(base, imm9) after the store."""
    mask = low_mask(8 * nbytes)
    return (word & ~mask) | (value & mask)


def SETF_SIG(a):
    """SDM Vol.3 setf.sig: copy GR significand; NaT is a separate tag."""
    return a


def GETF_SIG(a):
    """SDM Vol.3 getf.sig: copy FR significand; NaTVal is a separate tag."""
    return a


def XMA_L(a, b, c):
    """SDM Vol.3 xma.l: low 64 bits of the 128-bit product plus addend."""
    wide = z3.ZeroExt(64, a) * z3.ZeroExt(64, b) + z3.ZeroExt(64, c)
    return z3.Extract(63, 0, wide)


def XMA_HU(a, b, c):
    return z3.Extract(127, 64, z3.ZeroExt(64, a) * z3.ZeroExt(64, b) + z3.ZeroExt(64, c))


def XMA_H(a, b, c):
    return z3.Extract(127, 64, z3.SignExt(64, a) * z3.SignExt(64, b) + z3.ZeroExt(64, c))


def FRNATTEST(sig, meta):
    """SDM Vol.1 NaTVal has zero significand, exponent 0x1fffe, sign 0."""
    return z3.And(sig == 0, meta == 0x1FFFE)


def evaluate(op, packed, kids):
    """Evaluates one pattern node; returns a tuple of its results in
    ir_ops.def order."""
    if op in (
        "ADD",
        "SUB",
        "AND",
        "ANDCM",
        "OR",
        "XOR",
        "SHL",
        "SHR",
        "SHRU",
        "NOT1",
        "AND1",
        "OR1",
        "SELECT",
        "SETF_SIG",
        "GETF_SIG",
        "XMA_L",
        "XMA_H",
        "XMA_HU",
        "FRNATTEST",
    ):
        return (globals()[op](*kids),)
    if op in ("SHL_IMM",):
        return (SHL_IMM(kids[0], packed),)
    if op in ("EXTR", "EXTRU", "DEPZ", "SXT", "ZXT"):
        return (globals()[op](packed, kids[0]),)
    if op == "CONST":
        return (CONST(packed),)
    if op == "CMP":
        return (CMP(packed, *kids),)
    if op == "CLOOP":
        return CLOOP(*kids)
    raise NotImplementedError("sem_ia64: %s is not modelled" % op)


# ---------------------------------------------------------------- FP expansions
#
# The promise of the exact host FP mode (ia64/STATUS.md, Host FP execution)
# for the Tier1 expansions of engine/tier1/rules/fpfast.rules: under the
# conditions below the IA-64 result register is the IEEE 754 result of the
# operation in the instruction's format, converted to the register format,
# and FPSR is unchanged. IEEE operations use the SMT floating-point theory
# (https://smt-lib.org/papers/BTRW14.pdf). The expansion proof can abstract
# these operations by arbitrary functions, a stronger obligation. This
# model does not verify the C soft-float implementation; native/rational
# and soft-float conformance tests check that separate boundary.
#
# Inputs are the register images the expansion reads: a = FR[f2] (the
# operand field source1), b = FR[f3] (source2), c = FR[f4] (auxiliary1),
# each (significand, meta = sign << 17 | 17-bit exponent); qp; FPSR.

FP_FORMATS = {
    # SDM Vol.1 Table 5-2 and IEEE 754 binary32/binary64: width, fraction
    # bits, the register exponent of the format's exponent 0 (bias of
    # the register format minus the format's bias).
    "s": (32, 23, 0xFF80),
    "d": (64, 52, 0xFC00),
}


def fp_op(name):
    """IEEE 754 operation `name` (mul|add|fma)_(s|d), round to nearest even,
    on encodings (Intel SDM Vol.1 §4.8, §11.5: MXCSR all masked, FTZ and DAZ
    clear)."""
    op, fmt = name.split("_")
    sort = {"s": z3.Float32(), "d": z3.Float64()}[fmt]
    operation = {"mul": z3.fpMul, "add": z3.fpAdd, "fma": z3.fpFMA}[op]

    def ieee(*args):
        # NaN payloads are unspecified by SMT-LIB. Guards exclude NaN
        # operands and results, so no payload is part of the promise.
        values = [z3.fpBVToFP(a, sort) for a in args]
        return z3.fpToIEEEBV(operation(z3.RNE(), *values))

    return ieee


def fp_reg_encoding(fmt, sig, meta):
    """(ok, encoding): ok when the register value (SDM Vol.1 Table 5-2) is a
    zero (exponent 0, significand 0) or a normal value exactly
    representable in the format, encoding its IEEE image (Vol.1 §5.1)."""
    width, frac, base = FP_FORMATS[fmt]
    exp, sign = meta & 0x1FFFF, z3.Extract(17, 17, meta)
    lo, hi = base + 1, base + (1 << (width - frac - 1)) - 2
    normal = z3.And(
        z3.UGE(exp, lo),
        z3.ULE(exp, hi),
        z3.Extract(63, 63, sig) == 1,
        z3.Extract(62 - frac, 0, sig) == 0,
    )
    zero = z3.And(exp == 0, sig == 0)
    e = z3.Extract(width - frac - 2, 0, exp - base)
    image = z3.Concat(
        sign, z3.If(zero, z3.BitVecVal(0, width - frac - 1), e), z3.Extract(62, 62 - frac + 1, sig)
    )
    return z3.And(z3.ULT(meta, 1 << 18), z3.Or(normal, zero)), image


def fp_neg(x):
    return x ^ (z3.BitVecVal(1, x.size()) << (x.size() - 1))


def fp_result(fmt, r):
    """(kind, sig, meta) of IEEE encoding r: kind 'normal' above the min
    normal, 'zero', else None-like (False). The register image of a normal
    value (Table 5-2: explicit integer bit) and of a zero."""
    width, frac, base = FP_FORMATS[fmt]
    ebits = width - frac - 1
    e, f, s = (
        z3.Extract(width - 2, frac, r),
        z3.Extract(frac - 1, 0, r),
        z3.Extract(width - 1, width - 1, r),
    )
    normal = z3.And(e != 0, e != (1 << ebits) - 1, z3.Or(e != 1, f != 0))
    zero = z3.And(e == 0, f == 0)
    sig = z3.If(
        zero, z3.BitVecVal(0, 64), z3.Concat(z3.BitVecVal(1, 1), f, z3.BitVecVal(0, 63 - frac))
    )
    meta = z3.Concat(
        z3.BitVecVal(0, 46), s, z3.If(zero, z3.BitVecVal(0, 17), z3.ZeroExt(17 - ebits, e) + base)
    )
    return normal, zero, sig, meta


def fp_controls(fpsr, sf, exact, pc_wide=False):
    """FPSR conditions under which FPSR stays unchanged and nothing traps
    (Vol.1 §5.2, Table 5-6): traps.id set; the status field (sf: z3 or int)
    rounds to nearest and has i set already; exact: wre clear (the format's
    own exponent range); pc_wide: pc 10 or 11 (precision none, fp_approx)."""
    field = z3.Extract(12, 0, z3.LShR(fpsr, 6 + 13 * bv(sf)))
    conds = [
        z3.Extract(5, 5, fpsr) == 1,
        z3.Extract(5, 4, field) == 0,
        z3.Extract(12, 12, field) == 1,
    ]
    if exact:
        conds.append(z3.Extract(1, 1, field) == 0)
    if pc_wide:
        conds.append(z3.Extract(3, 3, field) == 1)
    return z3.And(conds)


def fp_mxcsr(fpsr, sf):
    """Intel SDM Vol.1 Table 4-9 / IA-64 Vol.1 Table 5-5: same RC
    encoding. Mask all exceptions, clear flags, DAZ and FTZ."""
    rc = z3.LShR(fpsr, 10 + 13 * bv(sf)) & 3
    return z3.BitVecVal(0x1F80, 64) | (rc << 13)


def fp_apply_flags(fpsr, sf, mxcsr):
    """IA-64 Vol.1 Table 5-6: IE/DE/ZE/OE/UE/PE map to V/D/Z/O/U/I,
    sticky bits 7..12 in the selected 13-bit status field."""
    return fpsr | ((mxcsr & 63) << (13 + 13 * bv(sf)))


def fp_traps(fpsr, sf, mxcsr):
    """An unmasked FP exception; sf1..3.td disables local traps."""
    disabled = z3.And(bv(sf) != 0, z3.LShR(fpsr, 12 + 13 * bv(sf)) & 1 != 0)
    masks = z3.If(disabled, z3.BitVecVal(63, 64), fpsr & 63)
    return mxcsr & 63 & ~masks != 0


def fp_round_normal(mant, tail, precision):
    """ia64/fp_round.c ia64_fp_round_right, RNE, shift 64-precision.

    Only the normal .s/.d branch (precision 24/53, shift 40/11):
    (rounded integer, inexact, rounded_up), before carry renormalization
    or exponent-range handling. tail summarizes a nonzero exact fraction
    below mant's least significant bit.
    """
    if precision not in (24, 53):
        raise ValueError("fp_round_normal: precision must be 24 or 53")
    shift = 64 - precision
    upper = z3.LShR(mant, shift)
    discarded = mant & ((1 << shift) - 1)
    half = 1 << (shift - 1)
    inexact = z3.Or(discarded != 0, tail)
    up = z3.Or(z3.UGT(discarded, half), z3.And(discarded == half, z3.Or(tail, upper & 1 != 0)))
    # The C result is __uint128_t, so a carry to bit precision is retained.
    rounded = z3.ZeroExt(64, upper) + z3.If(up, z3.BitVecVal(1, 128), z3.BitVecVal(0, 128))
    return rounded, inexact, up


FP_SCALAR = ("FMA", "FMS", "FNMA", "FMPY", "FADD", "FSUB", "FNORM")
FP_PACKED = ("FPMA", "FPMS", "FPNMA")
ONE = {"s": 0x3F800000, "d": 0x3FF0000000000000}


def fp_scalar(opcode, fmt, addend_f0, fused, v, fp_ops=fp_op):
    """(roles, value, zero_exact) of a scalar instruction (ia64/fp.c
    ia64_fp_compute rewrites, SDM Vol.3 fma, fms, fnma and pseudo-ops):
    v maps role -> encoding; fused False computes b * c + a as two
    roundings (fp_approx without FMA3)."""
    mul, add, fma = fp_ops("mul_" + fmt), fp_ops("add_" + fmt), fp_ops("fma_" + fmt)
    one = z3.BitVecVal(ONE[fmt], FP_FORMATS[fmt][0])

    def fused_op(x, y, z):
        return fma(x, y, z) if fused else add(mul(x, y), z)

    if opcode in ("FMA", "FMS", "FNMA"):
        b = fp_neg(v["b"]) if opcode == "FNMA" else v["b"]
        if addend_f0:
            return ("b", "c"), mul(b, v["c"]), ("b", "c")
        a = fp_neg(v["a"]) if opcode == "FMS" else v["a"]
        return ("a", "b", "c"), fused_op(b, v["c"], a), ("b", "c")
    if opcode == "FMPY":
        return ("a", "b"), mul(v["a"], v["b"]), ("a", "b")
    if opcode == "FADD":
        return ("a", "b"), add(v["b"], v["a"]), None
    if opcode == "FSUB":
        return ("a", "b"), add(v["a"], fp_neg(v["b"])), None
    if opcode == "FNORM":
        return ("b",), mul(v["b"], one), ("b",)
    raise NotImplementedError(opcode)


def FP_EXPAND(kind, inp, sf, mf, fp_ops=fp_op):
    """The expansion's IA-64 result when qp is 1: (defined, sig, meta, mf).
    kind: dict opcode, precision ('s'|'d'|'none'), addend ('f0'|'reg'|'any'),
    mode ('exact'|'approx'), fused (host FMA3). defined: the conditions of
    the promise hold, so FR[f1] = (sig, meta), PSR.mfl/mfh = mf and FPSR
    and every other state is unchanged."""
    op = kind["opcode"]
    if op in FP_PACKED:
        return _fp_packed(kind, inp, sf, mf, fp_ops)
    fmt = "d" if kind["precision"] == "none" else kind["precision"]
    ok, enc = {}, {}
    for role in "abc":
        ok[role], enc[role] = fp_reg_encoding(fmt, inp[role + "_sig"], inp[role + "_meta"])
    roles, r, zero_roles = fp_scalar(op, fmt, kind["addend"] == "f0", kind["fused"], enc, fp_ops)
    normal, zero, sig, meta = fp_result(fmt, r)
    if zero_roles is None:
        zero_exact = z3.BoolVal(True)  # an IEEE sum that rounds to zero is exact
    else:
        zero_exact = z3.Or([inp[x + "_sig"] == 0 for x in zero_roles])
    defined = z3.And(
        fp_controls(inp["fpsr"], sf, kind["mode"] == "exact", kind["precision"] == "none"),
        *[ok[x] for x in roles],
        z3.Or(normal, z3.And(zero, zero_exact)),
    )
    return defined, sig, meta, mf


def _lane(x, k):
    return z3.Extract(32 * k + 31, 32 * k, x)


def _fp_packed(kind, inp, sf, mf, fp_ops=fp_op):
    """fpma, fpms, fpnma (SDM Vol.3; ia64/fp_parallel.c): each 32-bit half
    of the significands is a single; f0 as f2 multiplies; NaTVal sources
    give NaTVal (excluded); the result has exponent 0x1003e, sign 0."""
    op = kind["opcode"]
    roles = ("b", "c") if kind["addend"] == "f0" else ("a", "b", "c")
    conds = [fp_controls(inp["fpsr"], sf, False)]
    for x in roles:
        conds.append(z3.Not(z3.And(inp[x + "_meta"] == 0x1FFFE, inp[x + "_sig"] == 0)))
    lanes = []
    for k in range(2):
        v = {x: _lane(inp[x + "_sig"], k) for x in "abc"}
        for x in roles:
            e, f = z3.Extract(30, 23, v[x]), z3.Extract(22, 0, v[x])
            conds.append(z3.And(e != 0xFF, z3.Or(e != 0, f == 0)))
        b = fp_neg(v["b"]) if op == "FPNMA" else v["b"]
        if kind["addend"] == "f0":
            r = fp_ops("mul_s")(b, v["c"])
        else:
            a = fp_neg(v["a"]) if op == "FPMS" else v["a"]
            r = fp_ops("fma_s")(b, v["c"], a)
        e, f = z3.Extract(30, 23, r), z3.Extract(22, 0, r)
        normal = z3.And(e != 0, e != 0xFF, z3.Or(e != 1, f != 0))
        zero = z3.And(e == 0, f == 0)
        factor_zero = z3.Or(z3.Extract(30, 0, v["b"]) == 0, z3.Extract(30, 0, v["c"]) == 0)
        conds.append(z3.Or(normal, z3.And(zero, factor_zero)))
        lanes.append(r)
    return z3.And(conds), z3.Concat(lanes[1], lanes[0]), z3.BitVecVal(0x1003E, 64), mf
