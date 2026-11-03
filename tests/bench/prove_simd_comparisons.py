#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the transcribed packed compare/min/max formulas with Z3."""

import argparse
import hashlib
import json
from pathlib import Path
import z3

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--operation", choices=("pcmp", "minmax"), required=True)
p.add_argument("--output", type=Path, required=True)
args = p.parse_args()
a, b = z3.BitVecs("a b", 64)
cases = []
for bits in (8, 16, 32) if args.operation == "pcmp" else (8, 16):
    high = sum(1 << (i + bits - 1) for i in range(0, 64, bits))
    different = a ^ b
    for mode in ("eq", "gt") if args.operation == "pcmp" else ("min", "max"):
        signed = args.operation == "pcmp" or bits == 16
        if mode == "eq":
            low = high ^ ((1 << 64) - 1)
            selected = ~(((different & low) + low) | different | low)
        else:
            difference = (b | high) - (a & ~high)
            sign_greater = b & ~a if signed else a & ~b
            selected = (sign_greater | (~different & ~difference)) & high
        mask = selected | (selected - z3.LShR(selected, bits - 1))
        actual = (
            mask if args.operation == "pcmp" else (b if mode == "max" else a) ^ (different & mask)
        )
        expected = z3.BitVecVal(0, 64)
        for i in range(0, 64, bits):
            left, right = z3.Extract(i + bits - 1, i, a), z3.Extract(i + bits - 1, i, b)
            gt = left > right if signed else z3.UGT(left, right)
            if args.operation == "pcmp":
                lane = z3.If(
                    left == right if mode == "eq" else gt,
                    z3.BitVecVal((1 << bits) - 1, bits),
                    z3.BitVecVal(0, bits),
                )
            else:
                lane = z3.If(gt, left if mode == "max" else right, right if mode == "max" else left)
            expected |= z3.ZeroExt(64 - bits, lane) << i
        solver = z3.Solver()
        solver.set(timeout=30000)
        solver.add(actual != expected)
        result = solver.check()
        cases.append(dict(bits=bits, mode=mode, result=str(result)))
        assert result == z3.unsat, (bits, mode, result)
report = dict(
    operation=args.operation,
    solver="Z3 " + z3.get_version_string(),
    scope="transcribed formulas over all pairs of 64-bit inputs",
    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    cases=cases,
)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
