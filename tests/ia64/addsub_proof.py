#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""All-input equivalence for the 24 packed add/subtract word formulas."""

import z3


def word(value):
    return z3.BitVecVal(value, 64)


def lane_mask(high_bits, bits):
    return (high_bits << 1) - z3.LShR(high_bits, bits - 1)


def candidate(a, b, bits, saturation, subtract):
    high = word(sum(1 << (pos + bits - 1) for pos in range(0, 64, bits)))
    if subtract:
        result = ((a | high) - (b & ~high)) ^ ((a ^ ~b) & high)
    else:
        result = ((a & ~high) + (b & ~high)) ^ ((a ^ b) & high)
    if not saturation:
        return result
    if saturation == 1:
        overflow = ((a ^ b) if subtract else ~(a ^ b)) & (a ^ result) & high
        replace = lane_mask(overflow, bits)
        limit = high - z3.LShR(high, bits - 1) + z3.LShR(a & high, bits - 1)
        return result ^ ((result ^ limit) & replace)
    if subtract:
        carry = ((~a & b) | ((~a | b) & result)) & high
    else:
        carry = ((a & b) | ((a | b) & ~result)) & high
    if saturation == 2:
        return result & ~lane_mask(carry, bits) if subtract else result | lane_mask(carry, bits)
    sign = b & high
    overflow = lane_mask(carry ^ sign, bits)
    maximum = lane_mask(~carry & sign if subtract else carry & ~sign, bits)
    return (result & ~overflow) | maximum


def original(a, b, bits, saturation, subtract):
    result = word(0)
    for pos in range(0, 64, bits):
        left = z3.Extract(pos + bits - 1, pos, a)
        right = z3.Extract(pos + bits - 1, pos, b)
        left = z3.SignExt(64 - bits, left) if saturation == 1 else z3.ZeroExt(64 - bits, left)
        right = (
            z3.SignExt(64 - bits, right) if saturation in (1, 3) else z3.ZeroExt(64 - bits, right)
        )
        value = left - right if subtract else left + right
        if saturation:
            low, high = (
                (-(1 << (bits - 1)), (1 << (bits - 1)) - 1)
                if saturation == 1
                else (0, (1 << bits) - 1)
            )
            value = z3.If(
                value < word(low), word(low), z3.If(value > word(high), word(high), value)
            )
        result |= (value & word((1 << bits) - 1)) << pos
    return result


a, b = z3.BitVecs("a b", 64)
for bits in (8, 16, 32):
    for saturation in range(4):
        for subtract in (False, True):
            solver = z3.Solver()
            solver.set(timeout=60000)
            solver.add(
                candidate(a, b, bits, saturation, subtract)
                != original(a, b, bits, saturation, subtract)
            )
            result = solver.check()
            print(
                f"{'PSUB' if subtract else 'PADD'} bits={bits} sat={saturation}: {result}",
                flush=True,
            )
            if result != z3.unsat:
                if result == z3.sat:
                    print(solver.model())
                raise SystemExit(1)
print(f"All 24 formulas proven with Z3 {z3.get_version_string()}")
