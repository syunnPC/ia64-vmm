#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""All-input equivalence of six packed shifts, including all 64-bit counts."""

import z3


def word(value):
    return z3.BitVecVal(value, 64)


def candidate(src, count, bits, kind):
    wide = z3.UGE(count, word(bits))
    if kind == 2:
        count = z3.If(wide, word(bits - 1), count)
    ones = word(sum(1 << pos for pos in range(0, 64, bits)))
    mask = z3.LShR(word((1 << bits) - 1), count) * ones
    if kind == 0:
        return z3.If(wide, word(0), (src << count) & (mask << count))
    result = z3.LShR(src, count) & mask
    if kind == 1:
        return z3.If(wide, word(0), result)
    sign = src & (ones << (bits - 1))
    negative = (sign - z3.LShR(sign, bits - 1)) | sign
    return result | (negative & ~mask)


def original(src, count, bits, kind):
    result = word(0)
    wide = z3.UGE(count, word(bits))
    for pos in range(0, 64, bits):
        lane = z3.Extract(pos + bits - 1, pos, src)
        if kind == 2:
            lane = z3.SignExt(64 - bits, lane) >> z3.If(wide, word(bits - 1), count)
        elif kind == 1:
            lane = z3.If(wide, word(0), z3.LShR(z3.ZeroExt(64 - bits, lane), count))
        else:
            lane = z3.If(wide, word(0), z3.ZeroExt(64 - bits, lane) << count)
        result |= (lane & word((1 << bits) - 1)) << pos
    return result


src, count = z3.BitVecs("src count", 64)
for bits in (16, 32):
    for kind in (0, 1, 2):
        solver = z3.Solver()
        solver.set(timeout=60000)
        solver.add(candidate(src, count, bits, kind) != original(src, count, bits, kind))
        result = solver.check()
        print(f"{('PSHL', 'PSHR_U', 'PSHR')[kind]} bits={bits}: {result}", flush=True)
        if result != z3.unsat:
            if result == z3.sat:
                print(solver.model())
            raise SystemExit(1)
print(f"All six formulas proven with Z3 {z3.get_version_string()}")
