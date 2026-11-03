# SPDX-License-Identifier: GPL-2.0-or-later
"""All-input equivalence of the PSHRADD2 word path, including count zero."""

import z3


def word(value):
    return z3.BitVecVal(value, 64)


def candidate(a, b, count):
    high = word(0x8000800080008000)
    ones = word(0x0001000100010001)
    low = word(0x7FFF7FFF7FFF7FFF)
    bias = z3.LShR(high, count)
    mask = (bias << 1) - ones
    shifted = ((z3.LShR(a ^ high, count) & mask) + (high - bias)) ^ high
    different_signs = shifted ^ b
    summed = ((shifted & low) + (b & low)) ^ (different_signs & high)
    overflow = ~different_signs & (shifted ^ summed) & high
    replace = (overflow << 1) - z3.LShR(overflow, 15)
    limit = low + z3.LShR(a & high, 15)
    return summed ^ ((summed ^ limit) & replace)


def original(a, b, count):
    result = word(0)
    for pos in range(0, 64, 16):
        left = z3.SignExt(48, z3.Extract(pos + 15, pos, a))
        right = z3.SignExt(48, z3.Extract(pos + 15, pos, b))
        added = (left >> count) + right
        clamped = z3.If(
            added < word(-32768), word(-32768), z3.If(added > word(32767), word(32767), added)
        )
        result |= (clamped & word(0xFFFF)) << pos
    return result


a, b = z3.BitVecs("a b", 64)
for count in range(4):
    solver = z3.Solver()
    solver.set(timeout=60000)
    solver.add(candidate(a, b, count) != original(a, b, count))
    result = solver.check()
    print(f"PSHRADD2 count {count}: {result}", flush=True)
    if result != z3.unsat:
        if result == z3.sat:
            print(solver.model())
        raise SystemExit(1)
print(f"All four PSHRADD2 formulas proven with Z3 {z3.get_version_string()}")
