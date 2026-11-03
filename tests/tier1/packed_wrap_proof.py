#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the exact packed_wrap IR expression against independent lane sums.

This covers all 128 input bits. Register mapping, predication, NaT and
restoration are covered by compiled-code tests in packed_wrap.inc.
"""

import json
import z3


def lowered(a, b, bits, sub):
    if bits == 32:
        total = a - b if sub else a + b
        carry = (a ^ b ^ total) & (1 << 32)
        return total + carry if sub else total - carry
    high = 0x8080808080808080 if bits == 8 else 0x8000800080008000
    low = (~high) & ((1 << 64) - 1)
    total = (a | high) - (b & low) if sub else (a & low) + (b & low)
    result = total ^ ((a ^ b) & high)
    return result ^ high if sub else result


def lanes(a, b, bits, sub):
    result = []
    for lo in reversed(range(0, 64, bits)):
        av = z3.Extract(lo + bits - 1, lo, a)
        bv = z3.Extract(lo + bits - 1, lo, b)
        result.append(av - bv if sub else av + bv)
    return z3.Concat(*result)


if __name__ == "__main__":
    a, b = z3.BitVecs("a b", 64)
    rows = []
    for bits in [8, 16, 32]:
        for sub in [False, True]:
            solver = z3.Solver()
            solver.add(lowered(a, b, bits, sub) != lanes(a, b, bits, sub))
            result = solver.check()
            rows.append({"bits": bits, "operation": "sub" if sub else "add", "result": str(result)})
            if result != z3.unsat:
                raise SystemExit(str(rows[-1]) + "\n" + str(solver.model()))
    print(json.dumps({"z3": z3.get_version_string(), "input_bits": 128, "cases": rows}, indent=2))
