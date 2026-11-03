#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the transcribed absolute-byte differences and their reduction."""

import argparse
import hashlib
import json
from pathlib import Path
import z3

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--output", type=Path, required=True)
args = p.parse_args()
a, b = z3.BitVecs("a b", 64)
high = 0x8080808080808080
different = a ^ b
difference = (b | high) - (a & ~high)
greater = ((a & ~b) | (~different & ~difference)) & high
mask = greater | (greater - z3.LShR(greater, 7))
maximum, minimum = b ^ (different & mask), a ^ (different & mask)
delta = maximum - minimum
expected = z3.BitVecVal(0, 64)
for i in range(0, 64, 8):
    left, right = z3.Extract(i + 7, i, a), z3.Extract(i + 7, i, b)
    value = z3.If(z3.UGT(left, right), left - right, right - left)
    expected |= z3.ZeroExt(56, value) << i
cases = []


def prove(name, actual, expected):
    s = z3.Solver()
    s.set(timeout=30000)
    s.add(actual != expected)
    result = s.check()
    cases.append(dict(case=name, result=str(result)))
    assert result == z3.unsat, (name, result)


prove("all eight absolute byte differences", delta, expected)
# The reduction is valid for every possible byte-difference word, so it
# composes with the first identity without duplicating its solver circuit.
values = z3.BitVec("values", 64)
sums = (values & 0x00FF00FF00FF00FF) + (z3.LShR(values, 8) & 0x00FF00FF00FF00FF)
actual_sum = z3.LShR(sums * 0x0001000100010001, 48)
expected_sum = sum(z3.LShR(values, i) & 255 for i in range(0, 64, 8))
prove("horizontal sum of arbitrary unsigned bytes", actual_sum, expected_sum)
report = dict(
    solver="Z3 " + z3.get_version_string(),
    scope="transcribed difference and reduction identities over all 64-bit inputs",
    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    cases=cases,
)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
