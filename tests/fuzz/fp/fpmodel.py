# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent exact reference model of IA-64 floating point.

Written from the Intel IA-64 Architecture Software Developer's Manual
Rev. 2.3 (2008/2010):
  Vol.1 ch.5 (register format Table 5-2, FPSR Tables 5-3..5-6, Figures 5-11
  and 5-12, sections 5.4.2-5.4.6), Vol.2 ch.8 (ISR.code for FP fault/trap
  vectors 0x5c00/0x5d00) and the Vol.3 instruction pages.
Arithmetic is exact: values are integer significands scaled by powers of two.
No host floating point is used anywhere in this file.

Historical audit hypotheses are selectable through Knobs. Rev.2.3 resolves
the enabled-underflow I flag, NaN signs and unsigned rounded-zero cases;
the defaults follow that revision. AMBIGUITY retains any unresolved choices.
"""

from dataclasses import dataclass

BIAS = 0xFFFF
SPECIAL = 0x1FFFF
NAT_EXP = 0x1FFFE
INT_EXP = 0x1003E
IBIT = 1 << 63
QBIT = 1 << 62
M64 = (1 << 64) - 1
RN, RM, RP, RZ = 0, 1, 2, 3
V, D, Z, O, U, I = 1, 2, 4, 8, 16, 32
PSR_MFL, PSR_MFH, PSR_DFL, PSR_DFH = 1 << 4, 1 << 5, 1 << 18, 1 << 19


@dataclass
class Knobs:
    # Rev.2.3 Fig.5-12 unambiguously says FLAGS.i |= tmp_i. The historical
    # knob is retained to replay older audit hypotheses; False is required.
    u_trap_sets_i: bool = False
    # Vol.3 fms/fnma: the NaN default bypasses the arithmetic sign negation.
    # False is required; True replays the earlier audit hypothesis.
    fms_nan_flip: bool = False
    # fcvt.fxu of a negative value that rounds to zero: valid (result 0)?
    fxu_neg_round_to_zero_valid: bool = True
    # Comparisons involving an unsupported operand are unordered.
    unsupported_unordered: bool = True
    # Emulation of confirmed implementation bugs (never the default); used
    # by the driver only to group further divergences behind a known cause.
    bug_parallel_wrap_regbits: bool = False


AMBIGUITY = {}


@dataclass(frozen=True)
class FR:
    sign: int
    exp: int
    sig: int

    def fmt(self):
        return "%u:%05x:%016x" % (self.sign, self.exp, self.sig)


ZERO = FR(0, 0, 0)
ONE = FR(0, BIAS, IBIT)
NATVAL = FR(0, NAT_EXP, 0)
QNAN_INDEF = FR(1, SPECIAL, IBIT | QBIT)
INT_INDEF = FR(0, INT_EXP, IBIT)


def integer(v):
    return FR(0, INT_EXP, v & M64)


# --- Table 5-2 classification -------------------------------------------
def is_natval(r):
    return r.sign == 0 and r.exp == NAT_EXP and r.sig == 0


def is_unsupported(r):
    return r.exp == SPECIAL and not (r.sig & IBIT)


def is_inf(r):
    return r.exp == SPECIAL and r.sig == IBIT


def is_nan(r):
    return r.exp == SPECIAL and bool(r.sig & IBIT) and r.sig != IBIT


def is_snan(r):
    return is_nan(r) and not (r.sig & QBIT)


def is_qnan(r):
    return is_nan(r) and bool(r.sig & QBIT)


def is_zero(r):
    return r.exp == 0 and r.sig == 0  # pseudo-zeros excluded


def is_finite(r):
    return r.exp != SPECIAL  # caller excludes NaTVal


def is_zero_valued(r):
    return is_finite(r) and r.sig == 0  # zero or pseudo-zero


def is_unorm(r):
    if r.exp == SPECIAL or is_natval(r):
        return False
    if r.exp == 0:
        return r.sig != 0
    return not (r.sig & IBIT)


def is_normal(r):
    return r.exp not in (0, SPECIAL) and bool(r.sig & IBIT)


def mag(r):
    """|value| = m * 2**e for a finite register (Vol1 5.1.2)."""
    e = (r.exp if r.exp else 0xC001) - BIAS - 63
    return r.sig, e


def quiet(r):
    return FR(r.sign, r.exp, r.sig | QBIT)


def cmp_mag(m1, e1, m2, e2):
    if m1 == 0 or m2 == 0:
        return (m1 != 0) - (m2 != 0)
    if e1 > e2:
        m1 <<= e1 - e2
    else:
        m2 <<= e2 - e1
    return (m1 > m2) - (m1 < m2)


def compare(a, b):
    """Exact signed comparison of two finite registers: -1/0/1."""
    ma, ea = mag(a)
    mb, eb = mag(b)
    if ma == 0 and mb == 0:
        return 0
    sa = -1 if a.sign else 1
    sb = -1 if b.sign else 1
    if ma == 0:
        return -sb
    if mb == 0:
        return sa
    if sa != sb:
        return sa
    return sa * cmp_mag(ma, ea, mb, eb)


# --- rounding -------------------------------------------------------------
def rnd(m, shift, sign, rc):
    """Round m / 2**shift to an integer. Returns (q, inexact, rounded_up)."""
    if shift <= 0:
        return m << -shift, False, False
    q = m >> shift
    rem = m & ((1 << shift) - 1)
    if rem == 0:
        return q, False, False
    half = 1 << (shift - 1)
    if rc == RN:
        up = rem > half or (rem == half and (q & 1) == 1)
    elif rc == RM:
        up = bool(sign)
    elif rc == RP:
        up = not sign
    else:
        up = False
    return q + int(up), True, up


@dataclass
class Env:
    p: int  # significand precision
    emin: int  # unbiased
    emax: int
    rng: int  # exponent width (8, 11, 15, 17)
    rc: int
    ftz: bool
    traps: int  # 1 bits = disabled (FPSR.traps or forced by sfx.td)
    parallel: bool = False

    def enabled(self, flag):
        return not (self.traps & flag)


def model_env(pc_completer, controls, traps, parallel=False):
    """Vol1 Table 5-6. Returns None for the reserved dynamic pc=01."""
    ftz = bool(controls & 1)
    wre = (controls >> 1) & 1
    pc = (controls >> 2) & 3
    rc = (controls >> 4) & 3
    if parallel:
        p, rng = 24, 8
    elif pc_completer == "x":  # no .pc completer, non-parallel (e.g. frcpa)
        p, rng = 64, 17
    elif pc_completer == "s":
        p, rng = 24, 17 if wre else 8
    elif pc_completer == "d":
        p, rng = 53, 17 if wre else 11
    else:
        if pc == 1:
            return None
        p = {0: 24, 2: 53, 3: 64}[pc]
        rng = 17 if wre else 15
    emax = (1 << (rng - 1)) - 1
    emin = 2 - (1 << (rng - 1))
    return Env(p, emin, emax, rng, rc, ftz, traps, parallel)


@dataclass
class Rounded:
    sign: int
    kind: str  # 'normal', 'zero', 'inf', 'max', 'denormal', 'wrapped'
    E: int = 0  # unbiased exponent of q's leading bit (normal/wrapped)
    q: int = 0  # p-bit significand (normal/wrapped) or denormal count
    flags: int = 0
    trap: int = 0  # O/U/I bits that trap (ISR code order)
    fpa: bool = False
    tags: set = None


def ieee_round(sign, m, e, env, knobs):
    """Vol1 Fig.5-12 + 5.4.2/5.4.3 for the nonzero exact value (-1)^sign*m*2^e."""
    assert m > 0
    n = m.bit_length()
    E = e + n - 1
    q, inexact, up = rnd(m, n - env.p, sign, env.rc)
    tags = {"p%d/r%d/rc%d" % (env.p, env.rng, env.rc)}
    sh = n - env.p
    if sh > 0 and (m & ((1 << sh) - 1)) == 1 << (sh - 1):
        tags.add("tie")
    if q == 1 << env.p:
        q >>= 1
        E += 1
        tags.add("carry-out")
    r = _ieee_round(sign, m, e, env, knobs, n, E, q, inexact, up)
    if r.kind == "normal" and E < env.emin:
        tags.add("tiny-to-minnormal")
    tags.add(
        "res:%s%s%s"
        % (r.kind, "+trap" if r.trap else "", "+ftz" if r.kind == "zero" and env.ftz else "")
    )
    r.tags = tags
    return r


def _ieee_round(sign, m, e, env, knobs, n, E, q, inexact, up):
    if E > env.emax:
        if env.enabled(O):
            fl = O | (I if inexact else 0)
            return Rounded(sign, "wrapped", E, q, fl, O | (I if inexact else 0), up)
        to_inf = env.rc == RN or (env.rc == RM and sign) or (env.rc == RP and not sign)
        r = Rounded(sign, "inf" if to_inf else "max", flags=O | I, fpa=to_inf)
        if env.enabled(I):
            r.trap = I
        return r
    if E < env.emin:
        if env.enabled(U):
            fl = U | (I if (inexact or knobs.u_trap_sets_i) else 0)
            return Rounded(sign, "wrapped", E, q, fl, U | (I if inexact else 0), up)
        if env.ftz:
            r = Rounded(sign, "zero", flags=U | I, fpa=False)
            if env.enabled(I):
                r.trap = I
            return r
        qe = env.emin - env.p + 1
        dq, dinexact, dup = rnd(m, qe - e, sign, env.rc)
        if dq == 0:
            r = Rounded(sign, "zero")
        elif dq == 1 << (env.p - 1):
            r = Rounded(sign, "normal", env.emin, dq)
        else:
            r = Rounded(sign, "denormal", q=dq)
        r.fpa = dup
        if dinexact:
            r.flags = U | I
            if env.enabled(I):
                r.trap = I
        return r
    r = Rounded(sign, "normal", E, q, fpa=up)
    if inexact:
        r.flags = I
        if env.enabled(I):
            r.trap = I
    return r


def to_register(r, env):
    """Register encoding of a rounded scalar result (Vol1 Table 5-2, 5.1.3)."""
    sh = 64 - env.p
    if r.kind == "zero":
        return FR(r.sign, 0, 0)
    if r.kind == "inf":
        return FR(r.sign, SPECIAL, IBIT)
    if r.kind == "max":
        return FR(r.sign, env.emax + BIAS, ((1 << env.p) - 1) << sh)
    if r.kind == "normal":
        return FR(r.sign, r.E + BIAS, r.q << sh)
    if r.kind == "wrapped":  # true biased exponent MOD 2^17 (5.4.2/5.4.3)
        return FR(r.sign, (r.E + BIAS) & 0x1FFFF, r.q << sh)
    # denormal: "Double-extended real denormal results are mapped to the
    # register format exponent of 0x00000" (15-bit range, Table 5-2 also
    # lists IA-32 stack single/double denormals at 0x00000).
    exp = 0 if env.rng == 15 else env.emin + BIAS
    return FR(r.sign, exp, r.q << sh)


def to_single_bits(r, knobs=None):
    """Single memory image of a parallel lane result (Vol1 Table 5-6 row a)."""
    s = r.sign << 31
    if r.kind == "wrapped" and knobs and knobs.bug_parallel_wrap_regbits:
        x = r.E + BIAS
        return s | (((((x >> 16) & 1) << 7) | (x & 0x7F)) << 23) | (r.q & 0x7FFFFF)
    if r.kind == "zero":
        return s
    if r.kind == "inf":
        return s | 0x7F800000
    if r.kind == "max":
        return s | 0x7F7FFFFF
    if r.kind == "normal":
        return s | ((r.E + 127) << 23) | (r.q & 0x7FFFFF)
    if r.kind == "wrapped":  # true biased exponent MOD 2^8 for Parallel FP
        return s | (((r.E + 127) & 0xFF) << 23) | (r.q & 0x7FFFFF)
    return s | r.q  # denormal


def fr_to_single(r):
    """fp_fr_to_mem_format(.., 4): Vol1 Figure 5-7 (bit transfer)."""
    e = ((r.exp >> 16) & 1) << 7 | (r.exp & 0x7F)
    if not (r.sig & IBIT):
        e = 0
    return (r.sign << 31) | (e << 23) | ((r.sig >> 40) & 0x7FFFFF)


def fr_to_double(r):
    e = ((r.exp >> 16) & 1) << 10 | (r.exp & 0x3FF)
    if not (r.sig & IBIT):
        e = 0
    return (r.sign << 63) | (e << 52) | ((r.sig >> 11) & ((1 << 52) - 1))


def single_to_fr(bits):
    """fp_mem_to_fr_format(.., 4): Vol1 Figure 5-4."""
    s = (bits >> 31) & 1
    e = (bits >> 23) & 0xFF
    f = bits & 0x7FFFFF
    if e == 0xFF:
        return FR(s, SPECIAL, IBIT | (f << 40))
    if e == 0:
        return FR(s, 0xFF81, f << 40) if f else FR(s, 0, 0)
    return FR(s, e - 127 + BIAS, IBIT | (f << 40))


def double_to_fr(bits):
    s = (bits >> 63) & 1
    e = (bits >> 52) & 0x7FF
    f = bits & ((1 << 52) - 1)
    if e == 0x7FF:
        return FR(s, SPECIAL, IBIT | (f << 11))
    if e == 0:
        return FR(s, 0xFC01, f << 11) if f else FR(s, 0, 0)
    return FR(s, e - 1023 + BIAS, IBIT | (f << 11))


# --- per-operation outcomes ----------------------------------------------
class Fault(Exception):
    def __init__(self, bits):
        self.bits = bits


@dataclass
class Out:
    """Result of one scalar computation (before state commit)."""

    value: object = None  # FR, int (lane bits) or bool (predicate)
    flags: int = 0
    trap: int = 0  # O/U/I that trap
    fpa: bool = False
    tags: set = None


def check_v(env, cond, out):
    if cond:
        if env.enabled(V):
            raise Fault(V)
        out.flags |= V


def fma_core(op, a, b, c, mul_only, env, knobs, lane_nan=None):
    """fma/fms/fnma (Vol3 2-61, 2-69, 2-72; Vol1 Fig.5-11/5-12).
    a=FR[f2] (addend), b=FR[f3], c=FR[f4]. Returns Out with an FR value
    (scalar) or a Rounded (parallel, via env.parallel)."""
    out = Out()
    ops = (a, b, c)
    if any(is_unsupported(x) for x in ops):
        check_v(env, True, out)
        out.value = QNAN_INDEF
        return out
    if any(is_nan(x) for x in ops):
        check_v(env, any(is_snan(x) for x in ops), out)
        n = c if is_nan(c) else a if is_nan(a) else b
        flip = (op == "fms" and n is a) or (op == "fnma" and n is not a)
        n = quiet(n)
        if knobs.fms_nan_flip and flip:
            n = FR(n.sign ^ 1, n.exp, n.sig)
        out.value = n
        return out
    ps = b.sign ^ c.sign ^ (op == "fnma")
    asg = a.sign ^ (op == "fms")
    prod_inf = is_inf(b) or is_inf(c)
    invalid = (
        (is_inf(b) and is_zero(c))
        or (is_zero(b) and is_inf(c))
        or (not mul_only and prod_inf and is_inf(a) and ps != asg)
    )
    if invalid:
        check_v(env, True, out)
        out.value = QNAN_INDEF
        return out
    if any(is_unorm(x) for x in ops):
        if env.enabled(D):
            raise Fault(D)
        out.flags |= D
    if prod_inf:
        out.value = FR(ps, SPECIAL, IBIT)
        return out
    if not mul_only and is_inf(a):
        out.value = FR(asg, SPECIAL, IBIT)
        return out
    mb, eb = mag(b)
    mc, ec = mag(c)
    pm, pe = mb * mc, eb + ec
    if mul_only:
        sm, se, ss = pm, pe, ps
        zero_sign = ps
    else:
        am, ae = mag(a)
        if pm == 0 and am == 0:
            sm, se, ss = 0, 0, 0
            zero_sign = ps if ps == asg else int(env.rc == RM)
        else:
            e0 = min(pe, ae)
            x = (pm << (pe - e0)) * (-1 if ps else 1) + (am << (ae - e0)) * (-1 if asg else 1)
            ss = int(x < 0)
            sm, se = abs(x), e0
            zero_sign = int(env.rc == RM)
    if sm == 0:
        out.value = ("zero", zero_sign)
        return out
    r = ieee_round(ss, sm, se, env, knobs)
    out.tags = r.tags
    out.value = r
    out.flags |= r.flags
    out.trap = r.trap
    out.fpa = r.fpa
    return out


def finish_scalar(out, env):
    v = out.value
    if isinstance(v, tuple):
        return FR(v[1], 0, 0)
    if isinstance(v, Rounded):
        return to_register(v, env)
    return v


def finish_lane(out, knobs=None):
    v = out.value
    if isinstance(v, tuple):
        return v[1] << 31
    if isinstance(v, Rounded):
        return to_single_bits(v, knobs)
    return fr_to_single(v)  # NaN / inf / indefinite


def relation(rel, a, b, knobs):
    """fp_equal / fp_less_than / fp_lesser_or_equal / fp_unordered."""
    unord = (
        is_nan(a)
        or is_nan(b)
        or (knobs.unsupported_unordered and (is_unsupported(a) or is_unsupported(b)))
    )
    if rel == "unord":
        return unord
    if unord:
        return False
    c = compare(a, b)
    return {"eq": c == 0, "lt": c < 0, "le": c <= 0}[rel]


def cmp_flags(a, b, signaling, env, out):
    """fcmp_exception_fault_check as described by Vol3 Table 2-26 and Fig.5-11."""
    if is_unsupported(a) or is_unsupported(b):
        check_v(env, True, out)
        return
    if is_nan(a) or is_nan(b):
        check_v(env, is_snan(a) or is_snan(b) or signaling, out)
        return
    if is_unorm(a) or is_unorm(b):
        if env.enabled(D):
            raise Fault(D)
        out.flags |= D


def minmax(kind, a, b, env, knobs):
    """fmin/fmax/famin/famax (Vol3 2-43, 2-44, 2-62, 2-65)."""
    out = Out()
    cmp_flags(a, b, True, env, out)
    if kind in ("famin", "famax"):
        aa, bb = FR(0, a.exp, a.sig), FR(0, b.exp, b.sig)
    else:
        aa, bb = a, b
    if kind in ("fmin", "famin"):
        take_a = relation("lt", aa, bb, knobs)
    else:
        take_a = relation("lt", bb, aa, knobs)
    out.value = a if take_a else b
    return out


def to_int(a, signed, trunc, width, env, knobs):
    """fcvt.fx/fxu (Vol3 2-54) and fpcvt lanes (Vol3 2-85), Vol1 5.4.4."""
    out = Out()
    indef = 1 << (width - 1)
    if is_unsupported(a) or is_nan(a) or is_inf(a):
        check_v(env, True, out)
        out.value = indef
        return out
    m, e = mag(a)
    rc = RZ if trunc else env.rc
    q, inexact, up = rnd(m, -e, a.sign, rc)
    if signed:
        ok = q <= ((1 << (width - 1)) if a.sign else (1 << (width - 1)) - 1)
    elif a.sign and q != 0:
        ok = False
    elif a.sign and m != 0:
        ok = knobs.fxu_neg_round_to_zero_valid
    else:
        ok = q <= (1 << width) - 1
    if not ok:
        check_v(env, True, out)
        out.value = indef
        return out
    if is_unorm(a):
        if env.enabled(D):
            raise Fault(D)
        out.flags |= D
    if inexact:
        out.flags |= I
        if env.enabled(I):
            out.trap = I
            out.fpa = up
    out.value = (-q if a.sign else q) & ((1 << width) - 1)
    return out


def normalize(r):
    """fp_normalize of a finite nonzero register: (sign, sig<<k, exp-k)."""
    m, e = mag(r)
    k = 64 - m.bit_length()
    return r.sign, m << k, e + 63 - k + BIAS  # biased exponent, may be <= 0


RECIP = [
    0x3FC,
    0x3F4,
    0x3EC,
    0x3E4,
    0x3DD,
    0x3D5,
    0x3CD,
    0x3C6,
    0x3BE,
    0x3B7,
    0x3AF,
    0x3A8,
    0x3A1,
    0x399,
    0x392,
    0x38B,
    0x384,
    0x37D,
    0x376,
    0x36F,
    0x368,
    0x361,
    0x35B,
    0x354,
    0x34D,
    0x346,
    0x340,
    0x339,
    0x333,
    0x32C,
    0x326,
    0x320,
    0x319,
    0x313,
    0x30D,
    0x307,
    0x300,
    0x2FA,
    0x2F4,
    0x2EE,
    0x2E8,
    0x2E2,
    0x2DC,
    0x2D7,
    0x2D1,
    0x2CB,
    0x2C5,
    0x2BF,
    0x2BA,
    0x2B4,
    0x2AF,
    0x2A9,
    0x2A3,
    0x29E,
    0x299,
    0x293,
    0x28E,
    0x288,
    0x283,
    0x27E,
    0x279,
    0x273,
    0x26E,
    0x269,
    0x264,
    0x25F,
    0x25A,
    0x255,
    0x250,
    0x24B,
    0x246,
    0x241,
    0x23C,
    0x237,
    0x232,
    0x22E,
    0x229,
    0x224,
    0x21F,
    0x21B,
    0x216,
    0x211,
    0x20D,
    0x208,
    0x204,
    0x1FF,
    0x1FB,
    0x1F6,
    0x1F2,
    0x1ED,
    0x1E9,
    0x1E5,
    0x1E0,
    0x1DC,
    0x1D8,
    0x1D4,
    0x1CF,
    0x1CB,
    0x1C7,
    0x1C3,
    0x1BF,
    0x1BB,
    0x1B6,
    0x1B2,
    0x1AE,
    0x1AA,
    0x1A6,
    0x1A2,
    0x19E,
    0x19A,
    0x197,
    0x193,
    0x18F,
    0x18B,
    0x187,
    0x183,
    0x17F,
    0x17C,
    0x178,
    0x174,
    0x171,
    0x16D,
    0x169,
    0x166,
    0x162,
    0x15E,
    0x15B,
    0x157,
    0x154,
    0x150,
    0x14D,
    0x149,
    0x146,
    0x142,
    0x13F,
    0x13B,
    0x138,
    0x134,
    0x131,
    0x12E,
    0x12A,
    0x127,
    0x124,
    0x120,
    0x11D,
    0x11A,
    0x117,
    0x113,
    0x110,
    0x10D,
    0x10A,
    0x107,
    0x103,
    0x100,
    0x0FD,
    0x0FA,
    0x0F7,
    0x0F4,
    0x0F1,
    0x0EE,
    0x0EB,
    0x0E8,
    0x0E5,
    0x0E2,
    0x0DF,
    0x0DC,
    0x0D9,
    0x0D6,
    0x0D3,
    0x0D0,
    0x0CD,
    0x0CA,
    0x0C8,
    0x0C5,
    0x0C2,
    0x0BF,
    0x0BC,
    0x0B9,
    0x0B7,
    0x0B4,
    0x0B1,
    0x0AE,
    0x0AC,
    0x0A9,
    0x0A6,
    0x0A4,
    0x0A1,
    0x09E,
    0x09C,
    0x099,
    0x096,
    0x094,
    0x091,
    0x08E,
    0x08C,
    0x089,
    0x087,
    0x084,
    0x082,
    0x07F,
    0x07C,
    0x07A,
    0x077,
    0x075,
    0x073,
    0x070,
    0x06E,
    0x06B,
    0x069,
    0x066,
    0x064,
    0x061,
    0x05F,
    0x05D,
    0x05A,
    0x058,
    0x056,
    0x053,
    0x051,
    0x04F,
    0x04C,
    0x04A,
    0x048,
    0x045,
    0x043,
    0x041,
    0x03F,
    0x03C,
    0x03A,
    0x038,
    0x036,
    0x033,
    0x031,
    0x02F,
    0x02D,
    0x02B,
    0x029,
    0x026,
    0x024,
    0x022,
    0x020,
    0x01E,
    0x01C,
    0x01A,
    0x018,
    0x015,
    0x013,
    0x011,
    0x00F,
    0x00D,
    0x00B,
    0x009,
    0x007,
    0x005,
    0x003,
    0x001,
]
RSQRT = [
    0x1A5,
    0x1A0,
    0x19A,
    0x195,
    0x18F,
    0x18A,
    0x185,
    0x180,
    0x17A,
    0x175,
    0x170,
    0x16B,
    0x166,
    0x161,
    0x15D,
    0x158,
    0x153,
    0x14E,
    0x14A,
    0x145,
    0x140,
    0x13C,
    0x138,
    0x133,
    0x12F,
    0x12A,
    0x126,
    0x122,
    0x11E,
    0x11A,
    0x115,
    0x111,
    0x10D,
    0x109,
    0x105,
    0x101,
    0x0FD,
    0x0FA,
    0x0F6,
    0x0F2,
    0x0EE,
    0x0EA,
    0x0E7,
    0x0E3,
    0x0DF,
    0x0DC,
    0x0D8,
    0x0D5,
    0x0D1,
    0x0CE,
    0x0CA,
    0x0C7,
    0x0C3,
    0x0C0,
    0x0BD,
    0x0B9,
    0x0B6,
    0x0B3,
    0x0B0,
    0x0AD,
    0x0A9,
    0x0A6,
    0x0A3,
    0x0A0,
    0x09D,
    0x09A,
    0x097,
    0x094,
    0x091,
    0x08E,
    0x08B,
    0x088,
    0x085,
    0x082,
    0x07F,
    0x07D,
    0x07A,
    0x077,
    0x074,
    0x071,
    0x06F,
    0x06C,
    0x069,
    0x067,
    0x064,
    0x061,
    0x05F,
    0x05C,
    0x05A,
    0x057,
    0x054,
    0x052,
    0x04F,
    0x04D,
    0x04A,
    0x048,
    0x045,
    0x043,
    0x041,
    0x03E,
    0x03C,
    0x03A,
    0x037,
    0x035,
    0x033,
    0x030,
    0x02E,
    0x02C,
    0x029,
    0x027,
    0x025,
    0x023,
    0x020,
    0x01E,
    0x01C,
    0x01A,
    0x018,
    0x016,
    0x014,
    0x011,
    0x00F,
    0x00D,
    0x00B,
    0x009,
    0x007,
    0x005,
    0x003,
    0x001,
    0x3FC,
    0x3F4,
    0x3EC,
    0x3E5,
    0x3DD,
    0x3D5,
    0x3CE,
    0x3C7,
    0x3BF,
    0x3B8,
    0x3B1,
    0x3AA,
    0x3A3,
    0x39C,
    0x395,
    0x38E,
    0x388,
    0x381,
    0x37A,
    0x374,
    0x36D,
    0x367,
    0x361,
    0x35A,
    0x354,
    0x34E,
    0x348,
    0x342,
    0x33C,
    0x336,
    0x330,
    0x32B,
    0x325,
    0x31F,
    0x31A,
    0x314,
    0x30F,
    0x309,
    0x304,
    0x2FE,
    0x2F9,
    0x2F4,
    0x2EE,
    0x2E9,
    0x2E4,
    0x2DF,
    0x2DA,
    0x2D5,
    0x2D0,
    0x2CB,
    0x2C6,
    0x2C1,
    0x2BD,
    0x2B8,
    0x2B3,
    0x2AE,
    0x2AA,
    0x2A5,
    0x2A1,
    0x29C,
    0x298,
    0x293,
    0x28F,
    0x28A,
    0x286,
    0x282,
    0x27D,
    0x279,
    0x275,
    0x271,
    0x26D,
    0x268,
    0x264,
    0x260,
    0x25C,
    0x258,
    0x254,
    0x250,
    0x24C,
    0x249,
    0x245,
    0x241,
    0x23D,
    0x239,
    0x235,
    0x232,
    0x22E,
    0x22A,
    0x227,
    0x223,
    0x220,
    0x21C,
    0x218,
    0x215,
    0x211,
    0x20E,
    0x20A,
    0x207,
    0x204,
    0x200,
    0x1FD,
    0x1F9,
    0x1F6,
    0x1F3,
    0x1F0,
    0x1EC,
    0x1E9,
    0x1E6,
    0x1E3,
    0x1DF,
    0x1DC,
    0x1D9,
    0x1D6,
    0x1D3,
    0x1D0,
    0x1CD,
    0x1CA,
    0x1C7,
    0x1C4,
    0x1C1,
    0x1BE,
    0x1BB,
    0x1B8,
    0x1B5,
    0x1B2,
    0x1AF,
    0x1AC,
    0x1AA,
]


def recip_approx(den):
    """fp_ieee_recip (Vol3 2-106) on a normalized (sign, sig, biased exp)."""
    s, sig, e = den
    return FR(s, (SPECIAL - 2 - e) & 0x1FFFF, IBIT | (RECIP[(sig >> 55) & 0xFF] << 53))


def rsqrt_approx(root):
    """fp_ieee_recip_sqrt (Vol3 2-108)."""
    s, sig, e = root
    idx = ((e & 1) << 7) | ((sig >> 56) & 0x7F)
    return FR(0, (0xFFFE - ((e - BIAS) >> 1)) & 0x1FFFF, IBIT | (RSQRT[idx] << 53))


def frcpa_core(a, b, env):
    """frcpa (Vol3 2-105). Returns (Out with FR value, pred, special)."""
    out = Out()
    if is_unsupported(a) or is_unsupported(b):
        check_v(env, True, out)
        out.value = QNAN_INDEF
        return out, False, True
    if is_nan(a) or is_nan(b):
        check_v(env, is_snan(a) or is_snan(b), out)
        out.value = quiet(a if is_nan(a) else b)
        return out, False, True
    az, bz = is_zero_valued(a), is_zero_valued(b)
    if (az and bz) or (is_inf(a) and is_inf(b)):
        check_v(env, True, out)
        out.value = QNAN_INDEF
        return out, False, True
    if bz and not is_inf(a):  # finite nonzero / 0 (pseudo-zero included)
        if env.enabled(Z):
            raise Fault(Z)
        out.flags |= Z
        out.value = FR(a.sign ^ b.sign, SPECIAL, IBIT)
        return out, False, True
    special = is_inf(a) or is_inf(b) or az
    return out, None, special


def frsqrta_core(b, env):
    out = Out()
    if is_unsupported(b):
        check_v(env, True, out)
        out.value = QNAN_INDEF
        return out, True
    if is_nan(b):
        check_v(env, is_snan(b), out)
        out.value = quiet(b)
        return out, True
    if b.sign and not is_zero_valued(b):
        check_v(env, True, out)
        out.value = QNAN_INDEF
        return out, True
    return out, False


# FCLASS (Vol3 2-50)
def fclass_member(r, c9):
    t = ((c9 & 1 and not r.sign) or (c9 & 2 and r.sign)) and (
        (c9 & 4 and is_zero(r))
        or (c9 & 8 and is_unorm(r))
        or (c9 & 16 and is_normal(r))
        or (c9 & 32 and is_inf(r))
    )
    return bool(
        t or (c9 & 64 and is_snan(r)) or (c9 & 128 and is_qnan(r)) or (c9 & 256 and is_natval(r))
    )
