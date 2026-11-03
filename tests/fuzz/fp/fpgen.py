# SPDX-License-Identifier: GPL-2.0-or-later
"""Structured and seeded-random operand/state generation for the FP audit."""

from fpmodel import FR, BIAS, SPECIAL, NAT_EXP, INT_EXP, IBIT, QBIT, NATVAL
from fpexec import new_state

M64 = (1 << 64) - 1
# (precision, emin, emax) of every computation model of Table 5-6
FORMATS = {
    "s": (24, -126, 127),
    "d": (53, -1022, 1023),
    "e": (64, -16382, 16383),
    "r": (64, -65534, 65535),
}


def fr(sign, exp, sig):
    return FR(sign & 1, exp & 0x1FFFF, sig & M64)


def special_values():
    v = []
    for s in (0, 1):
        v += [
            fr(s, 0, 0),  # zero
            fr(s, 1, 0),
            fr(s, 0x1003E, 0),  # pseudo-zeros
            fr(s, SPECIAL, IBIT),  # infinity
            fr(s, SPECIAL, IBIT | QBIT),  # QNaN
            fr(s, SPECIAL, IBIT | QBIT | 0x123456789),
            fr(s, SPECIAL, IBIT | 1),  # SNaN
            fr(s, SPECIAL, IBIT | (0x5A5A << 40)),
            fr(s, SPECIAL, 0),
            fr(s, SPECIAL, 1 << 40),  # pseudo-inf / pseudo-NaN
            fr(s, BIAS, IBIT),
            fr(s, BIAS + 1, IBIT | QBIT),
            fr(s, 0, 1),
            fr(s, 0, IBIT),  # ext denormal / pseudo-denormal
            fr(s, 0, IBIT >> 1),
            fr(s, 1, 1),
            fr(s, 1, IBIT),
            fr(s, 0x1FFFE, IBIT),
            fr(s, 0x1FFFE, M64),
            fr(s, 0x1FFFE, IBIT >> 1),  # unnormal at max exponent
            fr(s, 0x1003E, 1),
            fr(s, 0x1003E, 0x7FFFFFFFFFFFFFFF),
        ]
    v += [NATVAL, fr(1, NAT_EXP, 0)]
    for f, (p, emin, emax) in FORMATS.items():
        sh = 64 - p
        for s in (0, 1):
            v += [
                fr(s, emin + BIAS, IBIT),
                fr(s, emax + BIAS, (M64 >> sh) << sh),
                fr(s, emin + BIAS, 1 << sh),
                fr(s, emin + BIAS, (M64 >> (sh + 1)) << sh),
                fr(s, emin + BIAS - 1, IBIT),
                fr(s, emax + BIAS + 1, IBIT),
                fr(s, emin + BIAS - p, IBIT),
                fr(s, emin + BIAS - p + 1, IBIT | QBIT),
            ]
    return v


SPECIALS = special_values()


def rand_sig(rng, p=None, normal=True):
    k = rng.random()
    if k < 0.25:
        sig = rng.getrandbits(64)
    elif k < 0.4:  # sparse: a few bits
        sig = 0
        for _ in range(rng.randint(1, 4)):
            sig |= 1 << rng.randint(0, 63)
    elif k < 0.55:  # runs of ones (carry propagation)
        a, b = sorted((rng.randint(0, 64), rng.randint(0, 64)))
        sig = ((1 << b) - 1) ^ ((1 << a) - 1)
    elif k < 0.8 and p:  # exactly representable at precision p, maybe +-1 ulp-ish
        sig = rng.getrandbits(p) << (64 - p)
        if rng.random() < 0.5:
            sig ^= 1 << rng.randint(max(0, 64 - p - 3), min(63, 64 - p + 1))
    else:
        sig = rng.getrandbits(rng.choice((8, 24, 32, 53, 64)))
        sig <<= rng.randint(0, 64 - sig.bit_length()) if sig else 0
    sig &= M64
    if normal:
        sig |= IBIT
    return sig


EXP_BANDS = [
    (BIAS - 70, BIAS + 70),
    (0xFF81 - 40, 0xFF81 + 30),
    (0x1007E - 30, 0x1007E + 30),
    (0xFC01 - 70, 0xFC01 + 60),
    (0x103FE - 60, 0x103FE + 60),
    (0xC001 - 80, 0xC001 + 80),
    (0x13FFE - 70, 0x13FFE + 70),
    (1, 140),
    (0x1FF00, 0x1FFFE),
    (1, 0x1FFFE),
]


def rand_value(rng, p=None):
    k = rng.random()
    if k < 0.10:
        return rng.choice(SPECIALS)
    s = rng.getrandbits(1)
    lo, hi = rng.choice(EXP_BANDS)
    e = rng.randint(lo, hi)
    if k < 0.17:  # unnormal / pseudo-denormal
        e = rng.choice((0, e))
        return fr(s, e, rand_sig(rng, p, normal=False) >> rng.randint(1, 63))
    return fr(s, e, rand_sig(rng, p))


def rand_single_bits(rng):
    k = rng.random()
    s = rng.getrandbits(1) << 31
    if k < 0.15:
        return s | rng.choice(
            (
                0,
                0x7F800000,
                0x7FC00000,
                0x7F800001,
                0x7FA00000,
                0x00000001,
                0x007FFFFF,
                0x00800000,
                0x7F7FFFFF,
                0x3F800000,
                0x3F800001,
                0x33800000,
                0x34000000,
            )
        )
    if k < 0.35:
        return s | rng.getrandbits(23)  # denormal
    e = rng.choice(
        (rng.randint(1, 254), rng.randint(1, 30), rng.randint(100, 154), rng.randint(225, 254))
    )
    return s | (e << 23) | (rand_sig(rng, 24) >> 40 & 0x7FFFFF)


def packed(rng):
    if rng.random() < 0.04:
        return rng.choice(SPECIALS)
    return fr(0, INT_EXP, (rand_single_bits(rng) << 32) | rand_single_bits(rng))


def rand_controls(rng):
    ftz = int(rng.random() < 0.3)
    wre = int(rng.random() < 0.35)
    pc = rng.choice((0, 2, 3, 3, 3, 1)) if rng.random() < 0.97 else 1
    if pc == 1 and rng.random() < 0.9:
        pc = 3
    rc = rng.randint(0, 3)
    return ftz | (wre << 1) | (pc << 2) | (rc << 4)


def rand_fpsr(rng, sf):
    k = rng.random()
    if k < 0.45:
        traps = 0x3F
    elif k < 0.75:
        traps = 0x3F & ~(1 << rng.randint(0, 5))
    else:
        traps = rng.getrandbits(6)
    fpsr = traps
    for s in range(4):
        ctl = rand_controls(rng)
        if s and rng.random() < 0.15:
            ctl |= 0x40
        flags = rng.getrandbits(6) if rng.random() < 0.3 else 0
        fpsr |= (ctl | (flags << 7)) << (6 + 13 * s)
    return fpsr


def tie_triple(rng, p):
    """b*c + a landing on/near a rounding boundary at precision p."""
    s = rng.getrandbits(1)
    e = rng.choice(
        (
            BIAS,
            BIAS + rng.randint(-60, 60),
            rng.choice((0xFF81, 0xFC01, 0xC001, 1)) + rng.randint(-3, 3 + p),
            rng.choice((0x1007E, 0x103FE, 0x13FFE, 0x1FFFE)) - rng.randint(0, 2),
        )
    )
    e = max(2, min(e, 0x1FFFD))
    b = fr(s, e, rand_sig(rng, p))
    c = fr(0, BIAS, IBIT)
    ulp_e = e - (p - 1)
    sticky = rng.choice((0, 0, 1, -1))
    a_e = ulp_e - 1 + rng.choice((0, 0, 0, 1, -1))
    a_sig = IBIT
    if sticky:
        a_sig = IBIT | (1 << rng.randint(0, 20))
    a = fr(s ^ (rng.random() < 0.3), a_e, a_sig)
    if rng.random() < 0.5:  # move the factor 2^k between b and c
        k = rng.randint(-40, 40)
        b = fr(b.sign, b.exp + k, b.sig)
        c = fr(c.sign, c.exp - k, c.sig)
    return a, b, c


def tiny_triple(rng, fmt):
    """Products landing just around 2^emin of a format (tininess/denormals)."""
    p, emin, emax = FORMATS[fmt]
    target = emin + rng.randint(-p - 2, 2)
    eb = rng.randint(-200, 200)
    if rng.random() < 0.2:
        target = emax + rng.randint(-1, 1)  # overflow side
    b = fr(rng.getrandbits(1), BIAS + eb, rand_sig(rng, p))
    c = fr(rng.getrandbits(1), BIAS + target - eb, rand_sig(rng, p))
    if rng.random() < 0.5:
        a = fr(rng.getrandbits(1), BIAS + target + rng.randint(-p - 2, 1), rand_sig(rng, p))
    else:
        a = fr(0, 0, 0)
    return a, b, c


def gen_arith(rng, n):
    """fma/fms/fnma and their pseudo-ops."""
    out = []
    for _ in range(n):
        st = new_state()
        op = rng.choice(("fma", "fma", "fms", "fnma"))
        pc = rng.choice((None, None, "s", "d"))
        sf = rng.choice((0, 0, 1, 2, 3))
        st["fpsr"] = rand_fpsr(rng, sf)
        p = {"s": 24, "d": 53}.get(pc, rng.choice((24, 53, 64)))
        k = rng.random()
        if k < 0.25:
            a, b, c = tie_triple(rng, p)
        elif k < 0.5:
            a, b, c = tiny_triple(rng, rng.choice("sder"))
        elif k < 0.6:  # cancellation
            b, c = rand_value(rng, p), rand_value(rng, p)
            a = fr(
                b.sign ^ c.sign ^ 1, b.exp + c.exp - BIAS + rng.randint(-2, 2), rand_sig(rng, 64)
            )
        else:
            a, b, c = rand_value(rng, p), rand_value(rng, p), rand_value(rng, p)
        f2, f3, f4 = 7, 8, 9
        v = rng.random()
        if v < 0.12:
            f2 = 0  # fmpy / fnmpy
        elif v < 0.24:
            f4 = 1  # fadd / fsub
        elif v < 0.30:
            f2, f4 = 0, 1  # fnorm / fcvt.xuf
        elif v < 0.33:
            f3 = 0  # zero multiplicand register
        st["fr"] = {7: a, 8: b, 9: c, 6: fr(0, BIAS + 5, IBIT | 0x1234)}
        f1 = 6
        if rng.random() < 0.03:
            f1 = rng.choice((7, 8, 9))  # aliasing destination
        if rng.random() < 0.02:
            f1, st["fr"][32] = 32, fr(0, BIAS, IBIT)
        out.append(({"op": op, "pc": pc, "sf": sf, "f1": f1, "f2": f2, "f3": f3, "f4": f4}, st))
    return out


def gen_parallel(rng, n):
    out = []
    for _ in range(n):
        st = new_state()
        sf = rng.choice((0, 0, 1, 2, 3))
        st["fpsr"] = rand_fpsr(rng, sf)
        st["fr"] = {7: packed(rng), 8: packed(rng), 9: packed(rng), 6: fr(0, BIAS, IBIT)}
        k = rng.random()
        if k < 0.45:
            op = rng.choice(("fpma", "fpms", "fpnma"))
            f2 = 0 if rng.random() < 0.15 else 7
            f4 = 1 if rng.random() < 0.05 else 9
            if rng.random() < 0.3:  # tiny / overflow lanes
                for r in (8, 9):
                    hi, lo = [
                        rng.choice((rng.randint(1, 50), rng.randint(200, 254))) for _ in range(2)
                    ]
                    st["fr"][r] = fr(
                        0,
                        INT_EXP,
                        (
                            (rng.getrandbits(1) << 63)
                            | (hi << 55)
                            | (rng.getrandbits(23) << 32)
                            | (rng.getrandbits(1) << 31)
                            | (lo << 23)
                            | rng.getrandbits(23)
                        ),
                    )
            insn = {"op": op, "sf": sf, "f1": 6, "f2": f2, "f3": 8, "f4": f4}
        elif k < 0.6:
            insn = {
                "op": rng.choice(("fpmin", "fpmax", "fpamin", "fpamax")),
                "sf": sf,
                "f1": 6,
                "f2": 7,
                "f3": 8,
            }
        elif k < 0.75:
            insn = {
                "op": "fpcmp",
                "rel": rng.choice(("eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord")),
                "sf": sf,
                "f1": 6,
                "f2": 7,
                "f3": 8,
            }
        elif k < 0.88:
            insn = {
                "op": "fpcvt",
                "form": rng.choice(("fx", "fxu", "fx.trunc", "fxu.trunc")),
                "sf": sf,
                "f1": 6,
                "f2": 7,
            }
        else:
            insn = {
                "op": rng.choice(("fprcpa", "fprsqrta")),
                "sf": sf,
                "f1": 6,
                "f2": 7,
                "f3": 8,
                "p2": 10,
            }
        out.append((insn, st))
    return out


def gen_compare(rng, n):
    out = []
    for _ in range(n):
        st = new_state()
        sf = rng.choice((0, 1, 2, 3))
        st["fpsr"] = rand_fpsr(rng, sf)
        a = rand_value(rng)
        b = a if rng.random() < 0.1 else rand_value(rng)
        if rng.random() < 0.1:
            b = fr(b.sign ^ 1, b.exp, b.sig)
        st["fr"] = {7: a, 8: b, 6: fr(1, BIAS, IBIT)}
        st["pr"] = rng.getrandbits(16) | 1
        k = rng.random()
        if k < 0.4:
            insn = {
                "op": "fcmp",
                "rel": rng.choice(("eq", "lt", "le", "unord")),
                "unc": rng.random() < 0.3,
                "sf": sf,
                "p1": 6,
                "p2": 7,
                "f2": 7,
                "f3": 8,
            }
        elif k < 0.75:
            insn = {
                "op": rng.choice(("fmin", "fmax", "famin", "famax")),
                "sf": sf,
                "f1": 6,
                "f2": 7,
                "f3": 8,
            }
        else:
            c9 = (
                rng.getrandbits(9)
                if rng.random() < 0.7
                else rng.choice(
                    (0x1FF, 0x100, 0x80, 0x40, 0x7, 0xB, 0x13, 0x23, 0x3C | 1, 0x3C | 2)
                )
            )
            insn = {"op": "fclass", "c9": c9, "unc": rng.random() < 0.3, "p1": 6, "p2": 7, "f2": 7}
        out.append((insn, st))
    return out


def int_value(rng):
    k = rng.random()
    if k < 0.3:
        return rng.choice(
            (
                0,
                1,
                2,
                M64,
                1 << 63,
                (1 << 63) - 1,
                1 << 31,
                (1 << 32) - 1,
                (1 << 53) + 1,
                (1 << 24) + 1,
                (1 << 64) - (1 << 10),
            )
        )
    return rng.getrandbits(rng.choice((8, 16, 32, 53, 63, 64)))


def gen_convert(rng, n):
    out = []
    for _ in range(n):
        st = new_state()
        sf = rng.choice((0, 1, 2, 3))
        st["fpsr"] = rand_fpsr(rng, sf)
        k = rng.random()
        if k < 0.55:
            # around integer boundaries and halves
            if rng.random() < 0.5:
                e = BIAS + rng.choice((-2, -1, 0, 1, 31, 32, 52, 53, 62, 63, 64, 65))
                a = fr(rng.getrandbits(1), e, rand_sig(rng, rng.choice((24, 53, 64))))
                if rng.random() < 0.3:
                    a = fr(a.sign, BIAS + 63, IBIT | rng.choice((0, 1, M64 >> 1)))
            else:
                a = rand_value(rng)
            st["fr"] = {7: a, 6: fr(0, BIAS, IBIT)}
            insn = {
                "op": "fcvt",
                "form": rng.choice(("fx", "fxu", "fx.trunc", "fxu.trunc")),
                "sf": sf,
                "f1": 6,
                "f2": 7,
            }
        elif k < 0.7:
            st["fr"] = {
                7: fr(
                    rng.getrandbits(1),
                    rng.choice((INT_EXP, rng.randint(0, 0x1FFFF))),
                    int_value(rng),
                ),
                6: fr(0, BIAS, IBIT),
            }
            if rng.random() < 0.05:
                st["fr"][7] = NATVAL
            insn = {"op": "fcvt.xf", "f1": 6, "f2": 7}
        elif k < 0.85:
            # fcvt.xuf = fma.pc f1 = f3, f1, f0 on an integer
            st["fr"] = {8: fr(0, INT_EXP, int_value(rng)), 6: fr(0, BIAS, IBIT)}
            insn = {
                "op": "fma",
                "pc": rng.choice((None, "s", "d")),
                "sf": sf,
                "f1": 6,
                "f2": 0,
                "f3": 8,
                "f4": 1,
            }
        else:
            a = rand_value(rng)
            b = rand_value(rng)
            if rng.random() < 0.5:  # ordinary operands: the swa limits must not apply
                a = fr(rng.getrandbits(1), BIAS + rng.randint(-500, 500), rand_sig(rng))
                b = fr(rng.getrandbits(1), BIAS + rng.randint(-500, 500), rand_sig(rng))
            st["fr"] = {7: a, 8: b, 6: fr(0, BIAS, IBIT)}
            st["pr"] = 1 | (1 << 10)
            insn = {
                "op": rng.choice(("frcpa", "frsqrta")),
                "sf": sf,
                "f1": 6,
                "f2": 7,
                "f3": 8,
                "p2": 10,
            }
        out.append((insn, st))
    return out


def gen_bits(rng, n):
    ops = (
        "fmerge.s",
        "fmerge.ns",
        "fmerge.se",
        "fmix.lr",
        "fmix.r",
        "fmix.l",
        "fsxt.r",
        "fsxt.l",
        "fpack",
        "fswap",
        "fswap.nl",
        "fswap.nr",
        "fand",
        "fandcm",
        "for",
        "fxor",
        "fpmerge.s",
        "fpmerge.ns",
        "fpmerge.se",
    )
    out = []
    for _ in range(n):
        st = new_state()
        st["fr"] = {
            7: rand_value(rng),
            8: rand_value(rng),
            9: rand_value(rng),
            6: fr(0, BIAS, IBIT),
        }
        k = rng.random()
        if k < 0.45:
            op = rng.choice(ops)
            f2 = rng.choice((7, 7, 7, 0, 8))
            insn = {"op": op, "f1": 6, "f2": f2, "f3": 8}
        elif k < 0.6:
            insn = {
                "op": rng.choice(("xma.l", "xma.h", "xma.hu")),
                "f1": 6,
                "f2": rng.choice((7, 0)),
                "f3": 8,
                "f4": rng.choice((9, 1)),
            }
            for r in (7, 8, 9):
                if rng.random() < 0.8:
                    st["fr"][r] = fr(rng.getrandbits(1), rng.randint(0, 0x1FFFF), int_value(rng))
        elif k < 0.67:
            insn = {"op": "fselect", "f1": 6, "f2": 7, "f3": 8, "f4": 9}
        elif k < 0.82:
            form = rng.choice(("s", "d", "sig", "exp"))
            st["gr"] = {20: int_value(rng) if rng.random() < 0.5 else rng.getrandbits(64)}
            if form == "s" and rng.random() < 0.5:
                st["gr"][20] = rand_single_bits(rng) | (rng.getrandbits(32) << 32)
            st["nat"] = {20: rng.random() < 0.05}
            insn = {"op": "setf", "form": form, "f1": 6, "r2": 20}
        else:
            form = rng.choice(("s", "d", "sig", "exp"))
            st["gr"] = {8: 0x5555}
            insn = {"op": "getf", "form": form, "r1": 8, "f2": 7}
        out.append((insn, st))
    return out


def gen_status(rng, n):
    out = []
    for _ in range(n):
        st = new_state()
        sf = rng.randint(0, 3)
        st["fpsr"] = rand_fpsr(rng, sf)
        k = rng.random()
        if k < 0.4:
            insn = {
                "op": "fsetc",
                "sf": sf,
                "amask": rng.getrandbits(7),
                "omask": rng.getrandbits(7) & rng.choice((0x3F, 0x7F, 0x33)),
            }
        elif k < 0.6:
            insn = {"op": "fclrf", "sf": sf}
        else:
            insn = {"op": "fchkf", "sf": sf, "imm21": rng.getrandbits(21)}
        out.append((insn, st))
    return out


def gen_misc(rng, n):
    """Predication, disabled registers, illegal targets."""
    out = []
    pool = (
        gen_arith(rng, n)
        + gen_compare(rng, n // 2)
        + gen_convert(rng, n // 2)
        + gen_parallel(rng, n // 2)
        + gen_bits(rng, n // 2)
    )
    rng.shuffle(pool)
    for insn, st in pool[:n]:
        k = rng.random()
        if k < 0.35:
            insn = dict(insn, qp=rng.randint(1, 63))
            st["pr"] = rng.getrandbits(64)
        elif k < 0.7:
            st["psr"] = rng.choice((1 << 18, 1 << 19, 3 << 18))
            if rng.random() < 0.5:
                for key in ("f1", "f2", "f3", "f4"):
                    if key in insn and insn[key] >= 2 and rng.random() < 0.5:
                        old = insn[key]
                        insn = dict(insn, **{key: 32 if key == "f1" else 32 + old})
                        st["fr"][insn[key]] = st["fr"].get(old, FR(0, 0, 0))
        else:
            if "f1" in insn:
                insn = dict(insn, f1=rng.choice((0, 1)))
            elif "p1" in insn:
                insn = dict(insn, p2=insn["p1"])
        out.append((insn, st))
    return out


FAMILIES = {
    "arith": gen_arith,
    "parallel": gen_parallel,
    "compare": gen_compare,
    "convert": gen_convert,
    "bits": gen_bits,
    "status": gen_status,
    "misc": gen_misc,
}


def exact_reg(sign, m, E):
    """Register encoding of (-1)^sign * m * 2^E when representable, else None."""
    if m == 0:
        return fr(sign, 0, 0)
    n = m.bit_length()
    if n > 64:
        return None
    exp = E + n - 1 + BIAS
    sig = m << (64 - n)
    if 1 <= exp <= 0x1FFFE:
        return fr(sign, exp, sig)
    if exp < 1 and 1 - exp < 64 and (sig & ((1 << (1 - exp)) - 1)) == 0:
        return fr(sign, 1, sig >> (1 - exp))  # register-format denormal
    return None


ENVS = [
    ("s", 0, None),
    ("s", 1, None),
    ("d", 0, None),
    ("d", 1, None),
    (None, 0, 0),
    (None, 0, 2),
    (None, 0, 3),
    (None, 1, 0),
    (None, 1, 2),
    (None, 1, 3),
]


def env_params(pc, wre, pcf):
    p = {"s": 24, "d": 53}.get(pc) or {0: 24, 2: 53, 3: 64}[pcf]
    rng = 17 if wre else {"s": 8, "d": 11}.get(pc, 15)
    emax = (1 << (rng - 1)) - 1
    return p, 2 - (1 << (rng - 1)), emax


def gen_boundary(rng, n):
    """Exact values placed on rounding/tininess/overflow boundaries of every
    computation model: x = b*c + a with b*c exact and a a sticky/tie term."""
    out = []
    while len(out) < n:
        pc, wre, pcf = rng.choice(ENVS)
        p, emin, emax = env_params(pc, wre, pcf)
        kind = rng.choice(
            ("den-tie", "den-tie", "minnorm", "minnorm", "overflow", "mindenorm", "normal-tie")
        )
        s = rng.getrandbits(1)
        if kind == "den-tie":
            j = rng.randint(1, p - 1)  # denormal with p-j significant bits
            # odd multiple of half the denormal quantum 2^(emin-p+1): an exact tie
            m = (rng.getrandbits(p - j) | (1 << (p - j - 1))) * 2 + 1
            E = emin - p
        elif kind == "minnorm":
            k = rng.choice((p - 1, p, p + 1, p + 2, rng.randint(1, 63)))
            m, E = (1 << 64) - (1 << (64 - min(k, 64))), emin - 64
        elif kind == "mindenorm":
            m, E = rng.choice((1, 2, 3, 5, 7)), emin - p
        elif kind == "overflow":
            k = rng.choice((p - 1, p, p + 1, p + 2, 64))
            m, E = (1 << 64) - (1 << (64 - min(k, 64))), emax + 1 - 64
            if rng.random() < 0.3:
                m, E = 1, emax + 1
        else:
            E0 = rng.randint(emin, emax)
            m = (rng.getrandbits(p) | (1 << (p - 1))) * 2 + 1
            E = E0 - p
        while m and m.bit_length() > 64:
            m >>= 1
            E += 1
        split = rng.randint(-60, 60)
        b = exact_reg(s, m, E - split)
        c = exact_reg(rng.getrandbits(1), 1, split)
        if b is None or c is None:
            continue
        if c.sign:
            b = fr(b.sign ^ 1, b.exp, b.sig)
        # sticky / anti-sticky addend far below the boundary
        t = rng.random()
        if t < 0.4:
            a, f2 = fr(0, 0, 0), 0
        else:
            d = rng.randint(3, 70)
            a = exact_reg(rng.getrandbits(1), 1, E - d)
            if a is None:
                a = fr(0, 0, 0)
            f2 = 7
        st = new_state()
        sf = rng.choice((0, 1, 2, 3))
        st["fpsr"] = rand_fpsr(rng, sf)
        sh = 6 + 13 * sf
        ctl = (st["fpsr"] >> sh) & 0x7F
        ctl = (
            (ctl & ~0x0E) | (wre << 1) | ((pcf if pcf is not None else rng.choice((0, 2, 3))) << 2)
        )
        st["fpsr"] = (st["fpsr"] & ~(0x7F << sh)) | (ctl << sh)
        st["fr"] = {7: a, 8: b, 9: c, 6: fr(0, BIAS, IBIT)}
        out.append(
            (
                {
                    "op": rng.choice(("fma", "fms", "fnma")),
                    "pc": pc,
                    "sf": sf,
                    "f1": 6,
                    "f2": f2,
                    "f3": 8,
                    "f4": 9,
                },
                st,
            )
        )
    return out


def gen_specials(rng, n):
    """Special-operand triples for fma/fms/fnma (NaN priority, invalid,
    pseudo-zero * inf, unsupported, NaTVal) under random environments."""
    vals = [
        fr(0, 0, 0),
        fr(1, 0, 0),
        fr(0, 0x1003E, 0),
        fr(1, 1, 0),
        fr(0, SPECIAL, IBIT),
        fr(1, SPECIAL, IBIT),
        fr(0, SPECIAL, IBIT | QBIT | 7),
        fr(1, SPECIAL, IBIT | QBIT | 9),
        fr(0, SPECIAL, IBIT | 5),
        fr(1, SPECIAL, IBIT | 3),
        fr(0, SPECIAL, 0),
        fr(1, SPECIAL, 77),
        NATVAL,
        fr(0, BIAS, IBIT),
        fr(1, BIAS + 3, IBIT | 1),
        fr(0, 0, 5),
        fr(1, 0xC001, 1 << 40),
        fr(0, 1, 1),
        fr(0, 0x1FFFE, M64),
        fr(1, 0x1FFFE, IBIT >> 2),
    ]
    out = []
    for _ in range(n):
        st = new_state()
        sf = rng.choice((0, 1, 2, 3))
        st["fpsr"] = rand_fpsr(rng, sf)
        st["fr"] = {
            7: rng.choice(vals),
            8: rng.choice(vals),
            9: rng.choice(vals),
            6: fr(0, BIAS, IBIT),
        }
        f2 = 0 if rng.random() < 0.15 else 7
        f4 = 1 if rng.random() < 0.1 else 9
        out.append(
            (
                {
                    "op": rng.choice(("fma", "fms", "fnma")),
                    "pc": rng.choice((None, "s", "d")),
                    "sf": sf,
                    "f1": 6,
                    "f2": f2,
                    "f3": 8,
                    "f4": f4,
                },
                st,
            )
        )
    return out


FAMILIES["boundary"] = gen_boundary
FAMILIES["specials"] = gen_specials


def gen_memory(rng, n):
    from fpexec import MEMBASE

    out = []
    for _ in range(n):
        st = new_state()
        st["m0"], st["m1"] = rng.getrandbits(64), rng.getrandbits(64)
        k = rng.random()
        if k < 0.2:  # put interesting single/double/extended images in memory
            st["m0"] = (rand_single_bits(rng) << 32) | rand_single_bits(rng)
        elif k < 0.3:
            st["m1"] = (st["m1"] & ~0x7FFF) | rng.choice((0, 0x7FFF, 0x3FFF, 0x4000, 1, 0x7FFE))
        st["gr"] = {20: MEMBASE}
        st["fr"] = {6: fr(0, BIAS, IBIT), 7: rand_value(rng), 8: fr(1, BIAS, IBIT)}
        if rng.random() < 0.05:
            st["fr"][7] = NATVAL
        if rng.random() < 0.03:
            st["nat"] = {20: True}
        k = rng.random()
        if k < 0.45:
            op = rng.choice(("ldfe", "ldf8", "ldfs", "ldfd", "ldf.fill", "ldfs.s", "ldfd.s"))
            insn = {"op": op, "f1": rng.choice((6, 6, 6, 32, 1))}
            if op in ("ldfs", "ldfd", "ldf8") and rng.random() < 0.3:
                st["gr"][20] = MEMBASE + 8
            if rng.random() < 0.3:
                insn["imm"] = rng.randint(-256, 255)
        elif k < 0.85:
            op = rng.choice(("stfe", "stf8", "stfs", "stfd", "stf.spill"))
            insn = {"op": op, "f2": 7}
            if op in ("stfs", "stfd", "stf8") and rng.random() < 0.3:
                st["gr"][20] = MEMBASE + 8
            if rng.random() < 0.3:
                insn["imm"] = rng.randint(-256, 255)
        else:
            insn = {
                "op": rng.choice(("ldfp8", "ldfps", "ldfpd")),
                "f1": 6,
                "f2": rng.choice((7, 7, 7, 8)),
                "post": rng.random() < 0.3,
            }
        out.append((insn, st))
    return out


FAMILIES["memory"] = gen_memory


def single_bits(sign, m, E):
    """IEEE single image of (-1)^sign * m * 2^E if exactly representable."""
    if m == 0:
        return sign << 31
    while m % 2 == 0 and m.bit_length() > 24:
        m >>= 1
        E += 1
    n = m.bit_length()
    if n > 24:
        return None
    m <<= 24 - n
    E -= 24 - n
    be = E + 23 + 127
    if be >= 255:
        return None
    if be >= 1:
        return (sign << 31) | (be << 23) | (m & 0x7FFFFF)
    sh = 1 - be
    if sh > 23 or m & ((1 << sh) - 1):
        return None
    return (sign << 31) | (m >> sh)


def lane_case(rng):
    for _ in range(100):
        kind = rng.choice(("tie", "dentie", "minnorm", "overflow", "cancel", "tiny"))
        s = rng.getrandbits(1)
        if kind == "tie":
            E = rng.randint(-149, 127)
            k = rng.randint(-120, 120)
            b, c = single_bits(s, 1, k), single_bits(0, 1, E - k)
            a = single_bits(
                rng.getrandbits(1),
                rng.choice((1, 3, (1 << 23) + 1, (1 << 24) - 1)),
                E - 24 - rng.choice((0, 0, 1, 23)),
            )
        elif kind == "dentie":
            m = rng.getrandbits(rng.randint(1, 23)) | 1
            k = rng.randint(-100, -50)
            b, c = single_bits(s, m, k), single_bits(0, 1, -150 - k)
            a = 0 if rng.random() < 0.6 else single_bits(rng.getrandbits(1), 1, -149)
        elif kind == "minnorm":
            k = rng.randint(-100, -27)
            m = rng.choice(((1 << 24) - 1, (1 << 24) - 3, (1 << 23) + 1))
            b, c = single_bits(s, m, k - 23), single_bits(0, 1, -127 - k + rng.choice((0, 1, -1)))
            a = 0
        elif kind == "overflow":
            k = rng.randint(1, 127)
            m = rng.choice(((1 << 24) - 1, (1 << 23) + 1, 1 << 23))
            b, c = single_bits(s, m, k - 23), single_bits(0, 1, 128 - k + rng.choice((0, -1, 1)))
            a = 0 if rng.random() < 0.5 else single_bits(rng.getrandbits(1), 1, 104)
        elif kind == "cancel":
            m1, m2 = rng.getrandbits(12) | 1, rng.getrandbits(12) | 1
            k, j = rng.randint(-60, 60), rng.randint(-60, 60)
            b, c = single_bits(s, m1, k), single_bits(0, m2, j)
            a = single_bits(s ^ 1, m1 * m2, k + j)
        else:
            k, j = rng.randint(-149, -60), rng.randint(-80, 10)
            b, c = (
                single_bits(s, rng.getrandbits(24) | 1, k),
                single_bits(0, rng.getrandbits(24) | 1, j),
            )
            a = 0 if rng.random() < 0.5 else single_bits(1, rng.getrandbits(5) | 1, -149)
        if None not in (a, b, c):
            return a, b, c
    return 0, 0, 0


def gen_pboundary(rng, n):
    out = []
    for _ in range(n):
        (a0, b0, c0), (a1, b1, c1) = lane_case(rng), lane_case(rng)
        st = new_state()
        sf = rng.choice((0, 1, 2, 3))
        st["fpsr"] = rand_fpsr(rng, sf)
        st["fr"] = {
            7: fr(0, INT_EXP, (a0 << 32) | a1),
            8: fr(0, INT_EXP, (b0 << 32) | b1),
            9: fr(0, INT_EXP, (c0 << 32) | c1),
            6: fr(0, BIAS, IBIT),
        }
        out.append(
            (
                {
                    "op": rng.choice(("fpma", "fpms", "fpnma")),
                    "sf": sf,
                    "f1": 6,
                    "f2": 7,
                    "f3": 8,
                    "f4": 9,
                },
                st,
            )
        )
    return out


def gen_cancel(rng, n):
    """Exact cancellation and signed-zero sum rules in every rounding mode."""
    out = []
    for _ in range(n):
        m1, m2 = rng.getrandbits(rng.randint(1, 32)) | 1, rng.getrandbits(rng.randint(1, 32)) | 1
        k, j = rng.randint(-200, 200), rng.randint(-200, 200)
        s = rng.getrandbits(1)
        b, c = exact_reg(s, m1, k), exact_reg(0, m2, j)
        a = exact_reg(s ^ 1, m1 * m2, k + j)
        if rng.random() < 0.3:  # zero + zero
            b, c, a = fr(rng.getrandbits(1), 0, 0), rand_value(rng), fr(rng.getrandbits(1), 0, 0)
        if None in (a, b, c):
            continue
        st = new_state()
        sf = rng.choice((0, 1, 2, 3))
        st["fpsr"] = rand_fpsr(rng, sf)
        st["fr"] = {7: a, 8: b, 9: c, 6: fr(0, BIAS, IBIT)}
        op = rng.choice(("fma", "fms", "fnma"))
        if op != "fma":  # keep the cancellation for fms/fnma
            st["fr"][7] = fr(a.sign ^ 1, a.exp, a.sig) if op == "fms" else a
            if op == "fnma":
                st["fr"][8] = fr(b.sign ^ 1, b.exp, b.sig)
        out.append(
            (
                {
                    "op": op,
                    "pc": rng.choice((None, "s", "d")),
                    "sf": sf,
                    "f1": 6,
                    "f2": 7,
                    "f3": 8,
                    "f4": 9,
                },
                st,
            )
        )
    return out


FAMILIES["pboundary"] = gen_pboundary
FAMILIES["cancel"] = gen_cancel
