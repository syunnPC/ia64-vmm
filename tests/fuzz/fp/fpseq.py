#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""End-to-end check of the SDM Vol.1 12.3.3 software divide and square-root
sequences (double precision, max-throughput forms) on the harness.

Vol.1 12.3.3 states that frcpa/frsqrta set p6=1 only "if the ratio f6/f7 can
be obtained using the prescribed Newton-Raphson iterations", otherwise they
deliver the IEEE special result with p6=0 (or raise SWA when the operands are
well outside the double-extended range), and that the sequences produce the
correctly rounded IEEE result with S0 flags "as per the standard". The final
values are compared with exact rounding computed here; the model is also run
step by step as a cross-check of fpmodel itself."""

import argparse
import os
import random
import subprocess
import sys
from math import isqrt

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fpmodel import (
    FR,
    BIAS,
    SPECIAL,
    IBIT,
    QBIT,
    Knobs,
    model_env,
    ieee_round,  # noqa: E402
    to_register,
    double_to_fr,
    mag,
    QNAN_INDEF,
    quiet,
    is_nan,
    V,
    Z,
    O,
    U,
    I,
)
from fpexec import execute, new_state, to_line, snapshot  # noqa: E402

S0 = lambda rc: (rc << 4) | (3 << 2)  # wre=0, pc=11 (ignored for .d), ftz=0
S1 = 0x40 | (3 << 2) | 2  # td=1, pc=11, wre=1, rc=RN


def F(op, f1, f3, f4, f2, pc=None, sf=1, qp=6):
    return {"op": op, "pc": pc, "sf": sf, "f1": f1, "f2": f2, "f3": f3, "f4": f4, "qp": qp}


DIV = [
    {"op": "frcpa", "sf": 0, "f1": 8, "p2": 6, "f2": 6, "f3": 7},
    F("fnma", 9, 7, 8, 1),
    F("fma", 8, 9, 8, 8),
    F("fma", 9, 9, 9, 0),
    F("fma", 8, 9, 8, 8),
    F("fma", 9, 9, 9, 0),
    F("fma", 8, 9, 8, 8),
    F("fma", 9, 6, 8, 0, "d"),
    F("fnma", 6, 7, 9, 6, "d"),
    F("fma", 8, 6, 8, 9, "d", sf=0),
]
SQRT = [
    {"op": "frsqrta", "sf": 0, "f1": 7, "p2": 6, "f3": 6},
    F("fma", 8, 10, 7, 0),
    F("fma", 7, 6, 7, 0),
    F("fnma", 9, 7, 8, 10),
    F("fma", 8, 9, 8, 8),
    F("fma", 7, 9, 7, 7),
    F("fnma", 9, 7, 8, 10),
    F("fma", 8, 9, 8, 8),
    F("fma", 7, 9, 7, 7),
    F("fnma", 9, 7, 8, 10),
    F("fma", 8, 9, 8, 8),
    F("fma", 7, 9, 7, 7, "d"),
    F("fnma", 9, 7, 7, 6),
    F("fma", 7, 9, 8, 7, "d", sf=0),
]


def rand_double(rng):
    k = rng.random()
    s = rng.getrandbits(1)
    if k < 0.05:
        return (s << 63) | rng.choice(
            (
                0,
                0x7FF0000000000000,
                0x7FF8000000000001,
                0x7FF0000000000001,
                1,
                0x000FFFFFFFFFFFFF,
                0x0010000000000000,
                0x7FEFFFFFFFFFFFFF,
            )
        )
    if k < 0.15:
        return (s << 63) | rng.getrandbits(52)  # denormal
    e = rng.choice(
        (rng.randint(1, 2046), rng.randint(1, 60), rng.randint(1980, 2046), rng.randint(990, 1060))
    )
    return (s << 63) | (e << 52) | rng.getrandbits(52)


def exact_div(a, b, rc):
    """IEEE double a/b (register image) and the IEEE flags."""
    env = model_env("d", S0(rc), 0x3F)
    if is_nan(a) or is_nan(b):
        return quiet(a if is_nan(a) else b), (
            V if (is_nan(a) and not a.sig & QBIT) or (is_nan(b) and not b.sig & QBIT) else 0
        )
    ainf, binf = a.exp == SPECIAL, b.exp == SPECIAL
    az, bz = a.sig == 0 and not ainf, b.sig == 0 and not binf
    s = a.sign ^ b.sign
    if (ainf and binf) or (az and bz):
        return QNAN_INDEF, V
    if ainf or bz:
        return FR(s, SPECIAL, IBIT), (Z if bz and not ainf else 0)
    if binf or az:
        return FR(s, 0, 0), 0
    ma, ea = mag(a)
    mb, eb = mag(b)
    K = 200
    q, r = divmod(ma << K, mb)
    rr = ieee_round(s, (q << 1) | (r != 0), ea - eb - K - 1, env, Knobs())
    return to_register(rr, env), rr.flags


def exact_sqrt(a, rc):
    env = model_env("d", S0(rc), 0x3F)
    if is_nan(a):
        return quiet(a), (V if not a.sig & QBIT else 0)
    if a.sig == 0 and a.exp != SPECIAL:
        return FR(a.sign, 0, 0), 0
    if a.sign:
        return QNAN_INDEF, V
    if a.exp == SPECIAL:
        return a, 0
    m, e = mag(a)
    if e & 1:
        m, e = m << 1, e - 1
    K = 120
    M = m << (2 * K)
    q = isqrt(M)
    rr = ieee_round(0, (q << 1) | (q * q != M), e // 2 - K - 1, env, Knobs())
    return to_register(rr, env), rr.flags


def as_fr(x):
    return x if isinstance(x, FR) else double_to_fr(x)


def build(kind, x, y, rc):
    st = new_state()
    st["fpsr"] = 0x3F | (S0(rc) << 6) | (S1 << 19)
    if kind == "div":
        st["fr"] = {6: as_fr(x), 7: as_fr(y)}
    else:
        st["fr"] = {6: as_fr(x), 10: FR(0, BIAS - 1, IBIT)}
    return st


def rand_wide(rng):
    """53-bit significand anywhere in the 17-bit register exponent range."""
    e = rng.choice(
        (
            rng.randint(1, 0x1FFFE),
            rng.randint(1, 200),
            rng.randint(0x1FF00, 0x1FFFE),
            rng.randint(0xBF00, 0xC100),
            rng.randint(0x13F00, 0x14100),
        )
    )
    return FR(rng.getrandbits(1), e, (1 << 63) | (rng.getrandbits(52) << 11))


def line(seq, st):
    parts = [to_line(i, st).split() for i in seq]
    raws = ",".join(p[1][4:] for p in parts)
    head = parts[0]
    return " ".join([head[0], "raw=" + raws] + head[2:])


def model_run(seq, st):
    for i in seq:
        r = execute(i, st)
        if r.kind != "complete":
            return r
        st = r.st
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", default="build/fpaudit-harness")
    ap.add_argument("--count", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument(
        "--wide",
        action="store_true",
        help="53-bit operands across the whole register exponent range",
    )
    a = ap.parse_args()
    rng = random.Random(a.seed)
    cases = []
    for n in range(a.count):
        kind = "div" if n % 2 == 0 else "sqrt"
        if a.wide:
            x, y = rand_wide(rng), rand_wide(rng)
            if kind == "sqrt":
                x = FR(0, x.exp, x.sig)
        else:
            x, y = rand_double(rng), rand_double(rng)
            if kind == "sqrt" and rng.random() < 0.8:
                x &= (1 << 63) - 1
        cases.append((kind, x, y, rng.randint(0, 3)))
    lines = [line(DIV if k == "div" else SQRT, build(k, x, y, rc)) for k, x, y, rc in cases]
    out = subprocess.run(
        [a.harness], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True
    ).stdout.splitlines()
    bad = model_bad = swa = 0
    shown = 0
    for (kind, x, y, rc), l, o in zip(cases, lines, out):
        d = dict(t.split("=", 1) for t in o.split())
        st = build(kind, x, y, rc)
        if kind == "div":
            want, wflags = exact_div(st["fr"][6], st["fr"][7], rc)
            got = d["f8"]
        else:
            want, wflags = exact_sqrt(st["fr"][6], rc)
            got = d["f7"]
        mr = model_run(DIV if kind == "div" else SQRT, st)
        mgot = snapshot(mr.st)["f8" if kind == "div" else "f7"]
        flags = (int(d["fpsr"], 16) >> 13) & 0x3F
        if d["kind"] == "fault" and d["fault"] == "fp" and int(d["info"], 16) == 8:
            swa += 1  # SDM permits assistance outside the extended range
            continue
        ok = (
            d["kind"] == "complete"
            and got == want.fmt()
            and (flags & (V | Z | O | U | I)) == wflags
        )
        if mgot != want.fmt():
            model_bad += 1
        if not ok:
            bad += 1
            if shown < 10:
                shown += 1
                print(
                    "MISMATCH %s x=%s y=%s rc=%d: want %s flags=%x, harness %s %s flags=%x pr=%s"
                    % (
                        kind,
                        as_fr(x).fmt(),
                        as_fr(y).fmt(),
                        rc,
                        want.fmt(),
                        wflags,
                        d["kind"],
                        got,
                        flags,
                        d["pr"],
                    )
                )
                print("   ", l)
    print(
        "SDM 12.3.3 sequences%s: %d cases, %d SWA faults, %d harness mismatches, "
        "%d model mismatches" % (" (wide)" if a.wide else "", len(cases), swa, bad, model_bad)
    )
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
