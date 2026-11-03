#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove fixed byte-exchange networks against the original MUX1 table."""

import argparse
import hashlib
import json
from pathlib import Path
import z3

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--output", type=Path, required=True)
args = p.parse_args()
value = z3.BitVec("value", 64)
permutations = {
    8: (0, 4, 2, 6, 1, 5, 3, 7),
    9: (0, 4, 1, 5, 2, 6, 3, 7),
    10: (0, 2, 4, 6, 1, 3, 5, 7),
    11: (7, 6, 5, 4, 3, 2, 1, 0),
}


def swap_fields(value, mask, distance):
    swap = (z3.LShR(value, distance) ^ value) & mask
    return value ^ swap ^ (swap << distance)


cases = []
for imm in range(16):
    expected = z3.BitVecVal(0, 64)
    for out, source in enumerate(permutations.get(imm, (0,) * 8)):
        expected |= (z3.LShR(value, 8 * source) & 255) << (8 * out)
    if imm == 8:
        actual = swap_fields(value, 0x00000000FF00FF00, 24)
    elif imm == 9:
        actual = swap_fields(swap_fields(value, 0x00000000FFFF0000, 16), 0x0000FF000000FF00, 8)
    elif imm == 10:
        actual = swap_fields(swap_fields(value, 0x0000FF000000FF00, 8), 0x00000000FFFF0000, 16)
    elif imm == 11:
        actual = z3.Concat(*[z3.Extract(i + 7, i, value) for i in range(0, 64, 8)])
    else:
        actual = (value & 255) * 0x0101010101010101
    solver = z3.Solver()
    solver.set(timeout=30000)
    solver.add(actual != expected)
    result = solver.check()
    cases.append(dict(selector=imm, result=str(result)))
    assert result == z3.unsat, (imm, result)
report = dict(
    solver="Z3 " + z3.get_version_string(),
    scope="transcribed MUX1 networks over all 64-bit inputs and all 16 selectors",
    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    cases=cases,
)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
