#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check word-parallel identities against scalar lane formulas with Z3.

This proves the transcribed arithmetic identities over all 64-bit inputs;
the independent compiled-C differential harness checks the implementation.
"""

import argparse
import hashlib
import json
from pathlib import Path
import z3

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--operation", choices=("czx", "pavg", "addsub"), required=True)
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()
x, y = z3.BitVecs("x y", 64)
cases = []


def check(name, actual, expected):
    s = z3.Solver()
    s.set(timeout=30000)
    s.add(actual != expected)
    result = s.check()
    cases.append(dict(case=name, result=str(result)))
    if result != z3.unsat:
        raise AssertionError((name, result, s.model() if result == z3.sat else s.reason_unknown()))


def lanes(bits):
    mask = (1 << bits) - 1
    return [(z3.LShR(x, i) & mask, z3.LShR(y, i) & mask, i) for i in range(0, 64, bits)]


for bits in (8, 16) if a.operation != "addsub" else (8, 16, 32):
    high = sum(1 << (i + bits - 1) for i in range(0, 64, bits))
    low = high ^ ((1 << 64) - 1)
    ones = sum(1 << i for i in range(0, 64, bits))
    mask = (1 << bits) - 1
    if a.operation == "czx":
        expected = z3.BitVecVal(0, 64)
        for left, _, i in lanes(bits):
            expected |= z3.If(left == 0, z3.BitVecVal(1 << (i + bits - 1), 64), z3.BitVecVal(0, 64))
        actual = ~(((x & low) + low) | x | low)
        check(f"czx{bits}: exact zero-lane high-bit mask", actual, expected)
    elif a.operation == "pavg":
        for mode in ("odd", "ceil", "subtract-odd"):
            right = ~y if mode == "subtract-odd" else y
            different = x ^ right
            odd = different & ones
            mean = (x & right) + (z3.LShR(different, 1) & low)
            actual = (
                ((mean + odd) ^ high) | (~odd & ones)
                if mode == "subtract-odd"
                else mean + odd
                if mode == "ceil"
                else mean | odd
            )
            expected = z3.BitVecVal(0, 64)
            for left, right_lane, i in lanes(bits):
                if mode == "subtract-odd":
                    temp = (left - right_lane) & ((1 << (bits + 1)) - 1)
                    lane = (z3.LShR(temp, 1) | (temp & 1)) & mask
                elif mode == "ceil":
                    lane = z3.LShR(left + right_lane + 1, 1) & mask
                else:
                    temp = left + right_lane
                    lane = (z3.LShR(temp, 1) | (temp & 1)) & mask
                expected |= lane << i
            check(f"pavg{bits}: {mode}", actual, expected)
    else:
        for sub in (False, True):
            actual = (
                ((x | high) - (y & low)) ^ ((x ^ ~y) & high)
                if sub
                else ((x & low) + (y & low)) ^ ((x ^ y) & high)
            )
            expected = z3.BitVecVal(0, 64)
            for left, right, i in lanes(bits):
                expected |= ((left - right if sub else left + right) & mask) << i
            check(f"p{'sub' if sub else 'add'}{bits}: modular", actual, expected)

report = dict(
    operation=a.operation,
    solver="Z3 " + z3.get_version_string(),
    scope="transcribed bit-vector identities for all 64-bit inputs",
    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    cases=cases,
)
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
