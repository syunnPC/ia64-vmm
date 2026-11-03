#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the transcribed MIX/UNPACK lane permutations for all 64-bit inputs."""

import argparse
import hashlib
import json
from pathlib import Path
import z3

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--operation", choices=("mix", "unpack"), required=True)
p.add_argument("--output", type=Path, required=True)
args = p.parse_args()
a, b = z3.BitVecs("a b", 64)
cases = []
for bits in (8, 16, 32):
    for first in (False, True):
        expected = z3.BitVecVal(0, 64)
        lane_mask = (1 << bits) - 1
        if args.operation == "mix":
            even = sum(lane_mask << i for i in range(0, 64, 2 * bits))
            actual = (
                (a & ~even) | (z3.LShR(b, bits) & even)
                if first
                else ((a & even) << bits) | (b & even)
            )
            for i in range(32 // bits):
                lane = 2 * i + int(first)
                expected |= (z3.LShR(a, lane * bits) & lane_mask) << ((2 * i + 1) * bits)
                expected |= (z3.LShR(b, lane * bits) & lane_mask) << ((2 * i) * bits)
        else:
            x, y = (a & 0xFFFFFFFF, b & 0xFFFFFFFF) if first else (z3.LShR(a, 32), z3.LShR(b, 32))
            if bits < 32:
                x = (x | (x << 16)) & 0x0000FFFF0000FFFF
                y = (y | (y << 16)) & 0x0000FFFF0000FFFF
                if bits == 8:
                    x = (x | (x << 8)) & 0x00FF00FF00FF00FF
                    y = (y | (y << 8)) & 0x00FF00FF00FF00FF
            actual = (x << bits) | y
            base = 0 if first else 32 // bits
            for i in range(32 // bits):
                expected |= (z3.LShR(a, (base + i) * bits) & lane_mask) << ((2 * i + 1) * bits)
                expected |= (z3.LShR(b, (base + i) * bits) & lane_mask) << ((2 * i) * bits)
        solver = z3.Solver()
        solver.set(timeout=30000)
        solver.add(actual != expected)
        result = solver.check()
        cases.append(dict(bits=bits, first=first, result=str(result)))
        assert result == z3.unsat, (bits, first, result)
report = dict(
    operation=args.operation,
    solver="Z3 " + z3.get_version_string(),
    scope="transcribed bit permutations over all pairs of 64-bit inputs",
    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    cases=cases,
)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
