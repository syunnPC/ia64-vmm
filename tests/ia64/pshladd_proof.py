# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove PSHLADD2's two-stage saturation for every 128-bit operand pair.

The production fast path accepts counts 0..3. Decoded instructions use 1..3;
count 0 is also proven because the exported helper accepts diagnostic calls.
Counts outside the fast path execute the unchanged scalar implementation.
"""

import z3


def bv(x):
    return z3.BitVecVal(x, 64)


def original(a, b, count):
    result = bv(0)
    for pos in range(0, 64, 16):
        left = z3.SignExt(48, z3.Extract(pos + 15, pos, a))
        right = z3.SignExt(48, z3.Extract(pos + 15, pos, b))
        shifted = left * bv(1 << count)
        added = shifted + right
        clamped_add = z3.If(
            added < bv(-32768), bv(-32768), z3.If(added > bv(32767), bv(32767), added)
        )
        lane = z3.If(
            shifted > bv(32767), bv(32767), z3.If(shifted < bv(-32768), bv(-32768), clamped_add)
        )
        result |= (lane & bv(0xFFFF)) << pos
    return result


def candidate(a, b, count):
    high = bv(0x8000800080008000)
    ones = bv(0x0001000100010001)
    low = bv(0x7FFF7FFF7FFF7FFF)
    sign = a & high
    sign_bit = z3.LShR(sign, 15)
    negative = sign - sign_bit
    upper = high - z3.LShR(high, count)
    different = (a ^ negative) & upper
    shift_overflow = (different + low) & high
    low_bits = (ones << count) - ones
    shifted = (a << count) & ~low_bits
    different_signs = shifted ^ b
    summed = ((shifted & ~high) + (b & ~high)) ^ (different_signs & high)
    add_overflow = ~different_signs & (shifted ^ summed) & high
    overflow = shift_overflow | add_overflow
    replace = (overflow << 1) - z3.LShR(overflow, 15)
    limit = low + sign_bit
    return summed ^ ((summed ^ limit) & replace)


a, b = z3.BitVecs("a b", 64)
for count in range(4):
    solver = z3.Solver()
    solver.set(timeout=60000)
    solver.add(candidate(a, b, count) != original(a, b, count))
    result = solver.check()
    print(f"PSHLADD2 count {count}: {result}")
    if result != z3.unsat:
        if result == z3.sat:
            print(solver.model())
        raise SystemExit(1)
print(f"All four PSHLADD2 formulas proven with Z3 {z3.get_version_string()}")
