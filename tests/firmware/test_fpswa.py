# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""FPSWA ABI and integer model differential (seed 0x47534650)."""

import copy
import math
import os
from pathlib import Path
import random
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/fuzz/fp"))
import fpmodel as m
import fpexec as x


def build():
    target = ROOT / "build/tests/fpswa-harness"
    target.parent.mkdir(parents=True, exist_ok=True)
    sources = [
        ROOT / "tests/firmware/fpswa-harness.c",
        ROOT / "firmware/new/fpswa/assist.c",
        *sorted((ROOT / "firmware/new/fpswa").glob("model-*.c")),
    ]
    subprocess.run(
        [
            os.environ.get("HOSTCC", "gcc"),
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I.",
            "-Iinclude",
            *map(str, sources),
            "-o",
            str(target),
        ],
        cwd=ROOT,
        check=True,
    )
    return target


def line(i, st, extra=""):
    return x.to_line(i, st) + " " + extra


def run_cases(cases):
    p = subprocess.run(
        [str(build())],
        input="\n".join(c[0] for c in cases) + "\n",
        text=True,
        stdout=subprocess.PIPE,
        check=True,
    )
    rows = p.stdout.splitlines()
    assert len(rows) == len(cases)
    for (request, expected), row in zip(cases, rows):
        values = dict(t.split("=") for t in row.split())
        for key, value in expected.items():
            got = values[key] if key.startswith("f") and key != "fpsr" else int(values[key], 16)
            if got != value:
                raise AssertionError(f"{request}\n{key}: {got!r} != {value!r}")


def expected_model(i, st):
    res = x.execute(i, st)
    o = res.output(st)
    parallel = i["op"].startswith("fp")
    fault = res.kind == "fault"
    status = (5 if parallel else 1) if fault else 0
    if res.fault == "fp_trap":
        status |= 2
    result = {
        "status": status,
        "err": 0,
        "fpsr": o["fpsr"],
        "psr": o["psr"],
        "pr": o["pr"],
        "isr": res.info if fault else (0x88 if parallel else 8),
        "ifs": 0,
    }
    for r in range(2, 128):
        result["f" + str(r)] = (
            (res.st if not fault or res.fault == "fp_trap" else st)["fr"].get(r, m.ZERO).fmt()
        )
    return result


def exact_result(i, st):
    """New raw divide/sqrt operations use the existing Python IEEE oracle."""
    sf = i.get("sf", 0)
    controls = x.controls(st["fpsr"], sf)
    masks = x.trap_mask(st["fpsr"], sf)
    env = m.model_env(None, controls, masks)
    b = st["fr"][i["f3"]]
    a = b if i["op"] == "frsqrta" else st["fr"][i["f2"]]
    flags = m.D if m.is_unorm(a) or m.is_unorm(b) else 0
    result = copy.deepcopy(st)
    if flags & ~masks:
        status, isr = 1, m.D
    else:
        # 256 guard bits suffice for every bounded/directed rounding decision;
        # append a sticky bit rather than discarding a nonzero exact remainder.
        if i["op"] == "frsqrta":
            am, ae = m.mag(a)
            odd = ae & 1
            rad = am << (512 + odd)
            n = math.isqrt(rad)
            mant = (n << 1) | int(n * n != rad)
            exp = (ae - odd) // 2 - 257
            sign = 0
        else:
            am, ae = m.mag(a)
            bm, be = m.mag(b)
            q, r = divmod(am << 256, bm)
            mant = (q << 1) | int(r != 0)
            exp = ae - be - 257
            sign = a.sign ^ b.sign
        rounded = m.ieee_round(sign, mant, exp, env, m.Knobs())
        flags |= rounded.flags
        result["fr"][i["f1"]] = m.to_register(rounded, env)
        result["psr"] |= m.PSR_MFL if i["f1"] < 32 else m.PSR_MFH
        result["fpsr"] |= flags << (13 + 13 * sf)
        result["pr"] &= ~(1 << i["p2"])
        status = 3 if rounded.trap else 0
        isr = 1 | x.trap_code(rounded.trap, rounded.fpa) if rounded.trap else 8
    values = {
        "status": status,
        "err": 0,
        "fpsr": result["fpsr"],
        "psr": result["psr"],
        "isr": isr,
        "pr": result["pr"],
        "ifs": 0,
    }
    values.update(("f" + str(r), result["fr"].get(r, m.ZERO).fmt()) for r in range(2, 128))
    return values


def expected_packed(i, st):
    sf = i.get("sf", 0)
    masks = x.trap_mask(st["fpsr"], sf)
    env = m.model_env(None, x.controls(st["fpsr"], sf), masks | 7, parallel=True)
    result = x.Result(st)
    outs = []
    vals = []
    preds = []
    sqrt = i["op"] == "fprsqrta"
    a = list(reversed(x.lanes(x.rd(st, i.get("f2", 0)))))
    b = list(reversed(x.lanes(x.rd(st, i["f3"]))))
    for k in range(2):
        n = b[k] if sqrt else a[k]
        d = b[k]
        ordinary = (
            m.is_finite(n)
            and m.is_finite(d)
            and n.sig
            and d.sig
            and not m.is_natval(n)
            and not m.is_natval(d)
            and (not sqrt or not n.sign)
        )
        safe = False
        if ordinary:
            ne = m.normalize(n)[2]
            de = m.normalize(d)[2]
            safe = (
                not m.is_unorm(n)
                and not m.is_unorm(d)
                and ne > m.BIAS - 127 + 24
                and (sqrt or (de < m.BIAS + 126 and 2 - 127 < ne - de < 127))
            )
        if ordinary and not safe:
            single = x.new_state()
            single["fpsr"] = (masks | 7) | ((x.controls(st["fpsr"], sf) & ~2) << (6 + 13 * sf))
            single["fr"] = {7: n, 8: d}
            # Use the shared rational raw result, rounded to single precision.
            am, ae = m.mag(n)
            bm, be = m.mag(d)
            if sqrt:
                odd = ae & 1
                rad = am << (512 + odd)
                q = math.isqrt(rad)
                mant = (q << 1) | int(q * q != rad)
                exp = (ae - odd) // 2 - 257
                sign = 0
            else:
                q, r = divmod(am << 256, bm)
                mant = (q << 1) | bool(r)
                exp = ae - be - 257
                sign = n.sign ^ d.sign
            rounded = m.ieee_round(sign, mant, exp, env, m.Knobs())
            out = m.Out(
                value=rounded,
                flags=rounded.flags | (m.D if m.is_unorm(n) or m.is_unorm(d) else 0),
                trap=rounded.trap,
                fpa=rounded.fpa,
            )
            value = m.to_single_bits(rounded)
            pred = False
        else:
            result.cur_f1 = i["f1"]
            out, value, _ = (
                x.lane_frsqrta(d, env, result) if sqrt else x.lane_frcpa(n, d, env, result, k)
            )
            pred = bool(ordinary and safe)
        outs.append(out)
        vals.append(value)
        preds.append(pred)
    faults = (outs[1].flags & ~masks & 7) | ((outs[0].flags & ~masks & 7) << 4)
    if faults:
        status, isr = 5, faults
        final = st
    else:
        final = copy.deepcopy(st)
        flags = outs[0].flags | outs[1].flags
        final["fpsr"] |= flags << (13 + 13 * sf)
        final["psr"] |= m.PSR_MFL if i["f1"] < 32 else m.PSR_MFH
        final["fr"][i["f1"]] = m.integer(vals[0] | (vals[1] << 32))
        if all(preds):
            final["pr"] |= 1 << i["p2"]
        else:
            final["pr"] &= ~(1 << i["p2"])
        code = 0
        for k in range(2):
            enabled = outs[k].flags & ~masks & 0x38
            if enabled & 0x18:
                enabled |= outs[k].flags & m.I
            if enabled:
                code |= x.trap_code(enabled, outs[k].fpa, lane_hi=bool(k))
        status, isr = (7, code | 1) if code else (0, 0x88)
    expected = {
        "status": status,
        "err": 0,
        "isr": isr,
        "fpsr": final["fpsr"],
        "psr": final["psr"],
        "pr": final["pr"],
        "ifs": 0,
    }
    expected.update(("f" + str(r), final["fr"].get(r, m.ZERO).fmt()) for r in range(2, 128))
    return expected


class FpswaTests(unittest.TestCase):
    def test_arithmetic_faults(self):
        rng = random.Random(0x47534650)
        edge = [
            m.ZERO,
            m.ONE,
            m.NATVAL,
            m.QNAN_INDEF,
            m.FR(1, m.SPECIAL, m.IBIT),
            m.FR(0, m.SPECIAL, m.IBIT | 1),
            m.FR(0, m.BIAS, 1),
            m.FR(0, 0, 1),
            m.FR(0, 1, m.IBIT),
            m.FR(1, 0x1FFFE, m.M64),
        ]
        cases = []
        for sf in range(4):
            for rc in range(4):
                for pc in (0, 2, 3):
                    for k in range(80):
                        ctl = (
                            (pc << 2)
                            | (rc << 4)
                            | ((k & 1) << 1)
                            | ((k & 2) >> 1)
                            | ((k & 4) << 4 if sf else 0)
                        )
                        st = x.new_state()
                        st["fpsr"] = (63 if k < 20 else rng.randrange(64)) | (ctl << (6 + 13 * sf))
                        st["fpsr"] |= rng.getrandbits(6) << (13 + 13 * ((sf + 1) % 4))
                        for r in (6, 7, 8, 9):
                            st["fr"][r] = (
                                rng.choice(edge)
                                if k < 40
                                else m.FR(
                                    rng.randrange(2), rng.randrange(0x20000), rng.getrandbits(64)
                                )
                            )
                        op = rng.choice(
                            ["fma", "fms", "fnma", "fmin", "fmax", "famin", "famax", "fcmp", "fcvt"]
                        )
                        i = dict(
                            op=op,
                            sf=sf,
                            f1=6,
                            f2=7,
                            f3=8,
                            f4=9,
                            p1=6,
                            p2=7,
                            rel=rng.choice(["eq", "lt", "le", "unord"]),
                            form=rng.choice(["fx", "fxu", "fx.trunc", "fxu.trunc"]),
                        )
                        expected = expected_model(i, st)
                        cases.append((line(i, st), expected))
        run_cases(cases)

    def test_limit_assists(self):
        rng = random.Random(0x46505357)
        cases = []
        for sf in range(4):
            for rc in range(4):
                for pc in (0, 2, 3):
                    for k in range(80):
                        st = x.new_state()
                        st["pr"] |= 1 << 7
                        controls = (
                            (pc << 2)
                            | (rc << 4)
                            | ((k & 1) << 1)
                            | ((k & 2) >> 1)
                            | ((k & 4) << 4 if sf else 0)
                        )
                        st["fpsr"] = (63 if k < 40 else rng.randrange(64)) | (
                            controls << (6 + 13 * sf)
                        )
                        # The numerator itself requests scalar SWA (normalized exponent <=64).
                        st["fr"][7] = m.FR(
                            rng.randrange(2),
                            rng.choice([1, 2, 63, 64]),
                            rng.getrandbits(64) | m.IBIT,
                        )
                        st["fr"][8] = m.FR(
                            rng.randrange(2),
                            rng.choice([1, 64, m.BIAS, 0x1FFFC, 0x1FFFE]),
                            rng.getrandbits(64) | m.IBIT,
                        )
                        if k % 3 == 0:
                            st["fr"][7] = m.FR(0, 1, 1)
                        i = dict(op="frcpa", sf=sf, f1=6, f2=7, f3=8, p2=7)
                        cases.append((line(i, st), exact_result(i, st)))
                        st["fr"][8] = m.FR(
                            0, rng.choice([1, 2, 63, 64]), rng.getrandbits(64) | m.IBIT
                        )
                        i = dict(op="frsqrta", sf=sf, f1=6, f3=8, p2=7)
                        cases.append((line(i, st), exact_result(i, st)))
        run_cases(cases)

    def test_abi(self):
        i = dict(op="frcpa", sf=0, f1=6, f2=7, f3=8, p2=7)
        st = x.new_state()
        st["fpsr"] = 63 | (14 << 6)
        st["fr"][7] = m.FR(0, 1, m.IBIT)
        st["fr"][8] = m.ONE
        expected = exact_result(i, st)
        run_cases(
            [
                (line(i, st, "masklow=fc0 maskhigh=0"), expected),
                (line(i, st, "masklow=0 maskhigh=0"), expected),
                (line(i, st, "slot=2"), expected),
                (line(i, st, "bad=1"), {"status": m.M64, "err": 5 << 56}),
                (line(i, st, "bad=2"), {"status": m.M64, "err": 5 << 56}),
                (line(i, st, "trap=2"), {"status": m.M64, "err": 5 << 56}),
                (line(i, st, "isr=2"), {"status": 1, "isr": 2, "fpsr": st["fpsr"]}),
                (line(i, st, "slot=3"), {"status": m.M64, "err": 2 << 56}),
            ]
        )

    def test_traps(self):
        rng = random.Random(0x53574154)
        cases = []
        for sf in range(4):
            for rc in range(4):
                for pc in ("s", "d", None):
                    for sign in (0, 1):
                        for k in range(30):
                            st = x.new_state()
                            st["fpsr"] = 63 | ((12 | (rc << 4)) << (6 + 13 * sf))
                            if k & 1:
                                st["fpsr"] &= ~m.I
                            env = m.model_env(
                                pc, x.controls(st["fpsr"], sf), x.trap_mask(st["fpsr"], sf)
                            )
                            st["fr"][7] = m.FR(
                                sign,
                                env.emin + m.BIAS - rng.randrange(1, 70),
                                rng.getrandbits(64) | m.IBIT,
                            )
                            st["fr"][8] = m.ONE
                            i = dict(op="fma", pc=pc, sf=sf, f1=6, f2=0, f3=7, f4=8)
                            first_st = copy.deepcopy(st)
                            first_st["fpsr"] &= ~m.U
                            first = x.execute(i, first_st)
                            if first.fault != "fp_trap":
                                continue
                            request_st = copy.deepcopy(st)
                            request_st["fr"][6] = first.st["fr"][6]
                            # Inputs deliberately destroyed: traps must use the destination.
                            request_st["fr"][7] = request_st["fr"][8] = m.NATVAL
                            final = x.execute(i, st)
                            expected = expected_model(i, st)
                            expected["f7"] = expected["f8"] = m.NATVAL.fmt()
                            expected["status"] = 1 if final.fault == "fp_trap" else 0
                            expected["isr"] = final.info if final.fault == "fp_trap" else first.info
                            cases.append(
                                (line(i, request_st, f"trap=0 isr={first.info:x}"), expected)
                            )
        run_cases(cases)

    def test_packed_arithmetic(self):
        rng = random.Random(0x5041434B)
        cases = []
        for sf in range(4):
            for rc in range(4):
                for k in range(120):
                    st = x.new_state()
                    st["fpsr"] = rng.randrange(64) | ((rc << 4) << (6 + 13 * sf))
                    for r in (6, 7, 8, 9):
                        st["fr"][r] = m.integer(rng.getrandbits(64))
                    op = rng.choice(
                        [
                            "fpma",
                            "fpms",
                            "fpnma",
                            "fpmin",
                            "fpmax",
                            "fpamin",
                            "fpamax",
                            "fpcmp",
                            "fpcvt",
                        ]
                    )
                    i = dict(
                        op=op,
                        sf=sf,
                        f1=6,
                        f2=7,
                        f3=8,
                        f4=9,
                        rel=rng.choice(["eq", "lt", "le", "unord"]),
                        form=rng.choice(["fx", "fxu", "fx.trunc", "fxu.trunc"]),
                    )
                    cases.append((line(i, st, "isr=88"), expected_model(i, st)))
        run_cases(cases)

    def test_overflow_and_inexact_traps(self):
        cases = []
        for sf in range(4):
            for rc in range(4):
                for pc in ("s", "d", None):
                    for sign in (0, 1):
                        for overflow in (False, True):
                            st = x.new_state()
                            st["fpsr"] = 63 | ((12 | (rc << 4)) << (6 + 13 * sf))
                            env = m.model_env(pc, x.controls(st["fpsr"], sf), 63)
                            st["fr"][7] = m.FR(
                                sign,
                                m.BIAS + env.emax + 4 if overflow else m.BIAS,
                                m.IBIT | 0x123456789,
                            )
                            st["fr"][8] = m.FR(0, m.BIAS, m.IBIT | 0x987654321)
                            i = dict(op="fma", pc=pc, sf=sf, f1=6, f2=0, f3=7, f4=8)
                            first_st = copy.deepcopy(st)
                            first_st["fpsr"] &= ~(m.O if overflow else m.I)
                            first = x.execute(i, first_st)
                            if first.fault != "fp_trap":
                                continue
                            request = copy.deepcopy(st)
                            request["fr"][6] = first.st["fr"][6]
                            request["fr"][7] = request["fr"][8] = m.NATVAL
                            expected = expected_model(i, st)
                            expected["f7"] = expected["f8"] = m.NATVAL.fmt()
                            expected["status"] = 0
                            expected["isr"] = first.info
                            cases.append((line(i, request, f"trap=0 isr={first.info:x}"), expected))
        self.assertGreater(len(cases), 100)
        run_cases(cases)

    def test_packed_traps(self):
        cases = []
        for sf in range(4):
            for rc in range(4):
                for sign in (0, 1):
                    for k in range(1, 20):
                        st = x.new_state()
                        st["fpsr"] = 63 | ((rc << 4) << (6 + 13 * sf))
                        st["fr"][7] = m.integer(
                            ((sign << 31) | 0x00800000 | k) | ((0x3F800000 + k) << 32)
                        )
                        st["fr"][8] = m.integer(0x3EFFFFFF | ((0x40000000 + k) << 32))
                        st["fr"][9] = m.integer(0)
                        i = dict(op="fpma", sf=sf, f1=6, f2=9, f3=7, f4=8)
                        first_st = copy.deepcopy(st)
                        first_st["fpsr"] &= ~m.U
                        first = x.execute(i, first_st)
                        if first.fault != "fp_trap":
                            continue
                        request = copy.deepcopy(st)
                        request["fr"][6] = first.st["fr"][6]
                        request["fr"][7] = request["fr"][8] = m.NATVAL
                        expected = expected_model(i, st)
                        expected["f7"] = expected["f8"] = m.NATVAL.fmt()
                        expected["status"] = 0
                        expected["isr"] = first.info
                        cases.append((line(i, request, f"trap=0 isr={first.info:x}"), expected))
        self.assertGreater(len(cases), 100)
        run_cases(cases)

    def test_rotation(self):
        i = dict(op="frcpa", sf=0, f1=32, f2=33, f3=34, p2=16)
        st = x.new_state()
        st["fpsr"] = 63 | (14 << 6)
        st["pr"] |= 1 << 16
        st["fr"][33] = m.FR(0, 1, m.IBIT)
        st["fr"][34] = m.ONE
        logical = exact_result(i, st)
        cases = []
        for fr_base in (0, 1, 63, 95):
            for pr_base in (0, 1, 47):
                physical = copy.deepcopy(st)
                physical["fr"] = {32 + (r - 32 + fr_base) % 96: v for r, v in st["fr"].items()}
                physical["pr"] = 1 | (1 << (16 + pr_base))
                expected = dict(logical)
                for r in range(2, 128):
                    expected["f" + str(r)] = m.ZERO.fmt()
                for r in range(32, 128):
                    expected["f" + str(32 + (r - 32 + fr_base) % 96)] = logical["f" + str(r)]
                expected["ifs"] = (fr_base << 25) | (pr_base << 32)
                cases.append((line(i, physical, f"ifs={expected['ifs']:x} maskhigh=0"), expected))
        run_cases(cases)

    def test_packed_assists(self):
        rng = random.Random(0x53574150)
        cases = []
        for sf in range(4):
            for rc in range(4):
                for k in range(80):
                    st = x.new_state()
                    st["fpsr"] = (63 if k < 40 else rng.randrange(64)) | (
                        (rc << 4) << (6 + 13 * sf)
                    )
                    lanes = [
                        rng.randrange(1, 0x800000),
                        rng.choice(
                            [0x3F800000, 0x7E800000, 0x7F000000, rng.randrange(1, 0x800000)]
                        ),
                    ]
                    st["fr"][7] = m.integer(lanes[0] | (lanes[1] << 32))
                    st["fr"][8] = m.integer(
                        (rng.randrange(1, 0x800000) if k & 1 else 0x3F800000) | (0x3F800000 << 32)
                    )
                    i = dict(op="fprcpa", sf=sf, f1=6, f2=7, f3=8, p2=7)
                    cases.append((line(i, st, "isr=88"), expected_packed(i, st)))
                    i = dict(op="fprsqrta", sf=sf, f1=6, f3=7, p2=7)
                    cases.append((line(i, st, "isr=88"), expected_packed(i, st)))
        run_cases(cases)


if __name__ == "__main__":
    unittest.main()
