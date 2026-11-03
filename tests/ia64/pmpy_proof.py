# SPDX-License-Identifier: GPL-2.0-or-later
"""All-input equivalence of packed-multiply lane normalization and unrolling."""

import z3


def product(a, b):
    return z3.SignExt(16, z3.Extract(15, 0, a)) * z3.SignExt(16, z3.Extract(15, 0, b))


def original(a, b, right):
    result = z3.BitVecVal(0, 64)
    first_lane = 0 if right else 1
    for output in range(2):
        shift = (first_lane + output * 2) * 16
        lane = product(z3.LShR(a, shift), z3.LShR(b, shift))
        result |= z3.ZeroExt(32, lane) << (output * 32)
    return result


def candidate(a, b, right):
    first_shift = 0 if right else 16
    a, b = z3.LShR(a, first_shift), z3.LShR(b, first_shift)
    low = product(a, b)
    high = product(z3.LShR(a, 32), z3.LShR(b, 32))
    return z3.ZeroExt(32, low) | (z3.ZeroExt(32, high) << 32)


def shifted_original(a, b, shift, signed):
    result = z3.BitVecVal(0, 64)
    for lane in range(4):
        left = z3.Extract(lane * 16 + 15, lane * 16, a)
        right = z3.Extract(lane * 16 + 15, lane * 16, b)
        left = z3.SignExt(48, left) if signed else z3.ZeroExt(48, left)
        right = z3.SignExt(48, right) if signed else z3.ZeroExt(48, right)
        multiplied = left * right
        shifted = multiplied >> shift if signed else z3.LShR(multiplied, shift)
        result |= (shifted & 0xFFFF) << (lane * 16)
    return result


def shifted_lane(a, b, shift, signed):
    if signed:
        left = z3.SignExt(48, z3.Extract(15, 0, a))
        right = z3.SignExt(48, z3.Extract(15, 0, b))
        shifted = (left * right) >> shift
    else:
        shifted = z3.LShR((a & 0xFFFF) * (b & 0xFFFF), shift)
    return shifted & 0xFFFF


def shifted_candidate(a, b, shift, signed):
    return (
        shifted_lane(a, b, shift, signed)
        | (shifted_lane(z3.LShR(a, 16), z3.LShR(b, 16), shift, signed) << 16)
        | (shifted_lane(z3.LShR(a, 32), z3.LShR(b, 32), shift, signed) << 32)
        | (shifted_lane(z3.LShR(a, 48), z3.LShR(b, 48), shift, signed) << 48)
    )


a, b = z3.BitVecs("a b", 64)
for right in (False, True):
    solver = z3.Solver()
    solver.set(timeout=60000)
    solver.add(candidate(a, b, right) != original(a, b, right))
    result = solver.check()
    print(f"PMPY2 {'R' if right else 'L'}: {result}")
    if result != z3.unsat:
        if result == z3.sat:
            print(solver.model())
        raise SystemExit(1)
count = z3.BitVec("count", 32)
shift = z3.ZeroExt(32, count)
for signed in (False, True):
    solver = z3.Solver()
    solver.set(timeout=60000)
    solver.add(z3.ULT(count, 64))
    solver.add(shifted_candidate(a, b, shift, signed) != shifted_original(a, b, shift, signed))
    result = solver.check()
    print(f"PMPYSH2 {'signed' if signed else 'unsigned'}, all counts 0..63: {result}")
    if result != z3.unsat:
        if result == z3.sat:
            print(solver.model())
        raise SystemExit(1)
print(f"All four packed-multiply formulas proven with Z3 {z3.get_version_string()}")
