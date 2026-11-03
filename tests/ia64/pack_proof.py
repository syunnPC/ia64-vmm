# SPDX-License-Identifier: GPL-2.0-or-later
"""All-input bit-vector equivalence for the PACK word transformations."""

import z3


def bv(x):
    return z3.BitVecVal(x, 64)


def compact_bytes(x):
    x &= bv(0x00FF00FF00FF00FF)
    x = (x | z3.LShR(x, 8)) & bv(0x0000FFFF0000FFFF)
    return (x | z3.LShR(x, 16)) & bv(0xFFFFFFFF)


def signed_bytes(x):
    sign = x & bv(0x8000800080008000)
    negative = sign - z3.LShR(sign, 15)
    different = (x ^ negative) & bv(0x7F807F807F807F80)
    overflow = (different + bv(0x7FFF7FFF7FFF7FFF)) & bv(0x8000800080008000)
    replace = z3.LShR(overflow, 7) - z3.LShR(overflow, 15)
    limit = bv(0x007F007F007F007F) + z3.LShR(sign, 15)
    return compact_bytes(x ^ ((x ^ limit) & replace))


def unsigned_bytes(x):
    sign = x & bv(0x8000800080008000)
    negative = sign - z3.LShR(sign, 15)
    overflow = ((x & bv(0x7FFF7FFF7FFF7FFF)) + bv(0x7F007F007F007F00)) & bv(0x8000800080008000)
    replace = z3.LShR(overflow, 7) - z3.LShR(overflow, 15)
    return compact_bytes((x | replace) & ~negative)


def scalar_lane(x, bits, out, unsigned=False):
    lane = z3.SignExt(64 - bits, z3.Extract(bits - 1, 0, x))
    low, high = (0, (1 << out) - 1) if unsigned else (-(1 << (out - 1)), (1 << (out - 1)) - 1)
    clamped = z3.If(lane < bv(low), bv(low), z3.If(lane > bv(high), bv(high), lane))
    return clamped & bv((1 << out) - 1)


def pack4(a, b):
    return (
        scalar_lane(a, 32, 16)
        | (scalar_lane(z3.LShR(a, 32), 32, 16) << 16)
        | (scalar_lane(b, 32, 16) << 32)
        | (scalar_lane(z3.LShR(b, 32), 32, 16) << 48)
    )


def original(a, b, op):
    result = bv(0)
    bits, out = (32, 16) if op == 2 else (16, 8)
    pos = 0
    for source in (a, b):
        for lane in range(64 // bits):
            result |= scalar_lane(z3.LShR(source, lane * bits), bits, out, op == 1) << pos
            pos += out
    return result


a, b = z3.BitVecs("a b", 64)
candidates = (
    signed_bytes(a) | (signed_bytes(b) << 32),
    unsigned_bytes(a) | (unsigned_bytes(b) << 32),
    pack4(a, b),
)
for op, candidate in enumerate(candidates):
    solver = z3.Solver()
    solver.set(timeout=60000)
    solver.add(candidate != original(a, b, op))
    result = solver.check()
    print(f"PACK op {op}: {result}")
    if result != z3.unsat:
        if result == z3.sat:
            print(solver.model())
        raise SystemExit(1)
print(f"All three PACK formulas proven with Z3 {z3.get_version_string()}")
