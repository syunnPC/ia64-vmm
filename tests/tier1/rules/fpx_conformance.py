# SPDX-License-Identifier: GPL-2.0-or-later
"""Conformance of the generated FP expansion table (docs/floating-point.md), with
build/tier1-table-runner fpx.

Every entry of build/tier1/fpfast_rules.h, emitted by the product's walker
(t1_fpx_emit) and executed natively, against sem_ia64.FP_EXPAND on random
and edge inputs, with an independent IEEE 754 evaluator implemented here
using rational arithmetic and round to nearest even: a
path to $DONE stores the destination unchanged and no modified bit for
qp = 0, else the conditions of the promise hold and the results are
FP_EXPAND's; a path to $BAIL stores nothing. This also checks the host's
SSE/FMA3 arithmetic under the VMM's MXCSR against IEEE 754. This checks
the arithmetic boundary abstracted by the proofs. Every rule must reach $DONE with
qp = 1 on part of its cases.

    $T1_Z3_VENV/bin/python -I tests/tier1/rules/fpx_conformance.py
        --runner build/tier1-table-runner [--cases N] [--seed S] [--only REGEX]
"""

import argparse
import importlib.util
import os
import random
import re
import subprocess
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


try:
    import z3
except ImportError:
    sys.stderr.write(
        "fpx_conformance.py: z3 is not importable; run it with $T1_Z3_VENV/bin/python\n"
    )
    sys.exit(1)
rulelib = load("rulelib", os.path.join(ROOT, "engine", "tier1", "rules", "rulelib.py"))
sem_ia64 = load("sem_ia64", os.path.join(HERE, "sem_ia64.py"))
prove = load("prove", os.path.join(HERE, "prove.py"))
POISON = 0xA5A5A5A5A5A5A5A5
IEEE = {"s": (32, 23, 8), "d": (64, 52, 11)}  # width, fraction bits, exponent bits

# ---------------------------------------------------------------- IEEE 754


def decode(bits, fmt):
    """('nan',) | ('inf', sign) | ('num', sign, Fraction magnitude)."""
    width, frac, ebits = IEEE[fmt]
    sign, e, f = bits >> (width - 1), (bits >> frac) & ((1 << ebits) - 1), bits & ((1 << frac) - 1)
    bias = (1 << (ebits - 1)) - 1
    if e == (1 << ebits) - 1:
        return ("nan",) if f else ("inf", sign)
    if e == 0:
        return ("num", sign, Fraction(f, 1 << frac) * Fraction(2) ** (1 - bias))
    return ("num", sign, (1 + Fraction(f, 1 << frac)) * Fraction(2) ** (e - bias))


def encode(sign, mag, fmt):
    """Round mag (exact, >= 0) to nearest even in the format."""
    width, frac, ebits = IEEE[fmt]
    bias, emax = (1 << (ebits - 1)) - 1, (1 << ebits) - 2
    top = sign << (width - 1)
    if mag == 0:
        return top
    e = mag.numerator.bit_length() - mag.denominator.bit_length()
    if Fraction(2) ** e > mag:
        e -= 1
    e = max(e, 1 - bias)  # subnormal quantum below the min normal
    quantum = Fraction(2) ** (e - frac)
    n, rem = divmod(mag, quantum)
    n = int(n)
    if rem * 2 > quantum or (rem * 2 == quantum and n & 1):
        n += 1
    if n == 1 << (frac + 1):  # carried into the next binade
        n >>= 1
        e += 1
    if n < 1 << frac:  # subnormal (or zero)
        return top | n
    be = e + bias
    if be > emax:
        return top | (((1 << ebits) - 1) << frac)
    return top | (be << frac) | (n - (1 << frac))


def nan(fmt):
    width, frac, ebits = IEEE[fmt]
    return (((1 << ebits) - 1) << frac) | (1 << (frac - 1))


def ieee(name, args):
    """mul|add|fma _s|_d on encodings (round to nearest even)."""
    op, fmt = name.split("_")
    v = [decode(a, fmt) for a in args]
    if any(x[0] == "nan" for x in v):
        return nan(fmt)
    if op in ("mul", "fma"):
        x, y = v[0], v[1]
        psign = x[1] ^ y[1]
        if (x[0] == "inf" and y[0] == "num" and y[2] == 0) or (
            y[0] == "inf" and x[0] == "num" and x[2] == 0
        ):
            return nan(fmt)
        prod = ("inf", psign) if "inf" in (x[0], y[0]) else ("num", psign, x[2] * y[2])
        if op == "mul":
            return encode(psign, prod[2], fmt) if prod[0] == "num" else encode_inf(psign, fmt)
        terms = [prod, v[2]]
    else:
        terms = v
    infs = [t for t in terms if t[0] == "inf"]
    if infs:
        if len({t[1] for t in infs}) > 1:
            return nan(fmt)
        return encode_inf(infs[0][1], fmt)
    total = sum((-t[2] if t[1] else t[2]) for t in terms)
    if total == 0:
        # An exact zero: -0 only when every term is a negative zero (nearest).
        zsign = int(all(t[1] for t in terms) and all(t[2] == 0 for t in terms))
        return encode(zsign, Fraction(0), fmt)
    return encode(int(total < 0), abs(total), fmt)


def encode_inf(sign, fmt):
    width, frac, ebits = IEEE[fmt]
    return (sign << (width - 1)) | (((1 << ebits) - 1) << frac)


def concrete_fp_op(name):
    width = 32 if name.endswith("_s") else 64

    def f(*args):
        return z3.BitVecVal(ieee(name, [z3.simplify(a).as_long() for a in args]), width)

    return f


# ---------------------------------------------------------------- cases


def reg_value(rng, fmt):
    """(sig, meta) of a register value: mostly zeros and normals exact in the
    format, also extra bits, edge exponents, unnormals, specials, NaTVal."""
    width, frac, ebits = IEEE[fmt]
    base = 0xFFFF - ((1 << (ebits - 1)) - 1)
    sign = rng.getrandbits(1) << 17
    k = rng.random()
    if k < 0.08:
        return 0, sign  # +-0
    if k < 0.10:
        return (0, 0) if rng.random() < 0.5 else (1 << 63, 0xFFFF)  # f0, f1
    if k < 0.70:
        e = 0xFFFF + rng.randrange(-30, 31)
    elif k < 0.80:
        e = rng.choice(
            [
                base + 1,
                base + 2,
                base + (1 << ebits) - 2,
                base + (1 << ebits) - 3,
                base,
                base + (1 << ebits) - 1,
            ]
        )
    elif k < 0.85:
        return rng.getrandbits(64) >> 1, sign | rng.randrange(1, 0x1FFFE)  # unnormal
    elif k < 0.90:
        return rng.choice(
            [(1 << 63, 0x1FFFF), (3 << 62, 0x1FFFF), (0, 0x1FFFE), (rng.getrandbits(63), 0)]
        )
    else:
        return rng.getrandbits(64) | 1 << 63, sign | rng.randrange(0, 0x20000)
    sig = 1 << 63 | (rng.getrandbits(frac) << (63 - frac))
    if rng.random() < 0.1:
        sig |= 1 << rng.randrange(0, 63 - frac)  # one bit too many
    return sig, sign | e


def lane(rng):
    k = rng.random()
    if k < 0.1:
        return rng.getrandbits(1) << 31
    if k < 0.75:
        return rng.getrandbits(1) << 31 | (127 + rng.randrange(-20, 21)) << 23 | rng.getrandbits(23)
    if k < 0.85:
        return rng.getrandbits(1) << 31 | rng.choice([1, 2, 253, 254]) << 23 | rng.getrandbits(23)
    if k < 0.92:
        return rng.getrandbits(1) << 31 | rng.getrandbits(23)  # denormal
    return rng.getrandbits(32)


def packed_value(rng):
    if rng.random() < 0.04:
        return 0, 0x1FFFE  # NaTVal
    if rng.random() < 0.05:
        return (0, 0) if rng.random() < 0.5 else (1 << 63, 0xFFFF)  # f0, f1
    return lane(rng) << 32 | lane(rng), 0x1003E if rng.random() < 0.9 else rng.randrange(0, 0x40000)


def fpsr(rng, sf, rule):
    """Mostly the promise's controls in field sf (and Linux-like others)."""
    value = 0x0009804C0270033F | 1 << (18 + 13 * sf)
    if rng.random() < 0.15:
        return rng.getrandbits(58) & ~(1 << 12)
    field = rng.getrandbits(13) & ~0x3E | 1 << 12  # i set, rc nearest, wre clear
    if rule.expand["precision"] == "none":
        field |= rng.choice([2, 3]) << 2
    if rng.random() < 0.1:
        field ^= 1 << rng.choice([1, 4, 5, 12])
    value = (value & ~(0x1FFF << (6 + 13 * sf))) | field << (6 + 13 * sf)
    if rng.random() < 0.05:
        value &= ~(1 << 5)  # the inexact trap enabled
    return value


def case(rng, rule):
    packed = rule.expand["insn"] in ("FPMA", "FPMS", "FPNMA")
    fmt = (
        "s" if packed else ("d" if rule.expand["precision"] == "none" else rule.expand["precision"])
    )
    sf = rng.randrange(4)
    inp = {"qp": 0 if rng.random() < 0.08 else 1, "fpsr": fpsr(rng, sf, rule)}
    for role in "abcd":
        sig, meta = packed_value(rng) if packed else reg_value(rng, fmt)
        inp[role + ".sig"], inp[role + ".meta"] = sig, meta
    return sf, rng.choice([16, 32]), inp


def expected(rule, sf, mf, inp):
    spec = {k.replace(".", "_"): z3.BitVecVal(v, 64) for k, v in inp.items()}
    d, sig, meta, m = sem_ia64.FP_EXPAND(
        prove.expand_kind(rule), spec, sf, z3.BitVecVal(mf, 64), concrete_fp_op
    )
    return (
        z3.is_true(z3.simplify(d)),
        z3.simplify(sig).as_long(),
        z3.simplify(meta).as_long(),
        z3.simplify(m).as_long(),
    )


def check_rule(index, rule, runner, ncases, seed):
    rng = random.Random("%s:%s" % (seed, rule.name))
    cases = [case(rng, rule) for _ in range(ncases)]
    lines = "".join(
        "%x %x %x %s\n" % (index, sf, mf, " ".join("%x" % inp[n] for n in rulelib.EXPAND_INPUTS))
        for sf, mf, inp in cases
    )
    out = subprocess.run(
        [runner, "fpx"], input=lines, capture_output=True, text=True, check=True
    ).stdout.split("\n")
    fails, done, skipped = [], 0, False
    for (sf, mf, inp), line in zip(cases, out):
        f = line.split()
        if f[0] == "0":
            skipped = True
            break
        exit_, sig, meta, m = int(f[1], 16), int(f[2], 16), int(f[3], 16), int(f[4], 16)
        if exit_ == 0:
            if (sig, meta, m) != (POISON, POISON, POISON):
                fails.append("bail stored results: %s" % inp)
            continue
        if inp["qp"] == 0:
            if (sig, meta, m) != (inp["d.sig"], inp["d.meta"], 0):
                fails.append("qp 0: %x %x %x for %s" % (sig, meta, m, inp))
            continue
        defined, esig, emeta, em = expected(rule, sf, mf, inp)
        done += 1
        if not defined or (sig, meta, m) != (esig, emeta, em):
            fails.append(
                "sf %d: got %x %x %x, expected %s %x %x %x for %s"
                % (
                    sf,
                    sig,
                    meta,
                    m,
                    "defined" if defined else "UNDEFINED",
                    esig,
                    emeta,
                    em,
                    {k: hex(v) for k, v in inp.items()},
                )
            )
    return skipped, done, fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", required=True)
    ap.add_argument("--cases", type=int, default=4000)
    ap.add_argument("--seed", default="tier1-fpx-conformance")
    ap.add_argument("--only")
    args = ap.parse_args()
    runner = os.path.abspath(args.runner)
    rules = rulelib.load_expand_rules()
    ok = True
    for i, r in enumerate(rules):
        if args.only and not re.search(args.only, r.name):
            continue
        skipped, done, fails = check_rule(i, r, runner, args.cases, args.seed)
        if skipped:
            print("fpx-conformance: %-16s SKIPPED (no host FMA3)" % r.name)
            continue
        status = (
            "ok" if not fails and done else "FAILED" if fails else "FAILED (no $DONE with qp = 1)"
        )
        print("fpx-conformance: %-16s %6d cases %6d done %s" % (r.name, args.cases, done, status))
        for x in fails[:4]:
            print("    " + x)
        ok &= not fails and done > 0
    print("fpx-conformance: %s" % ("passed" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
