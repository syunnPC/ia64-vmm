#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove predicate_result word updates against the original read/two-write order.

Each target mask is zero (discarded p0) or one-hot (any mapped predicate).
The independent masks may alias. PR, condition and NaT are unconstrained.
Use the repository's pinned z3-solver==5.1.0.0 environment.
"""

import z3


def word(value):
    return z3.BitVecVal(value, 64)


def original(pr, first, second, condition, nat, update):
    a = z3.Or(first == 0, pr & first != 0)
    b = z3.Or(second == 0, pr & second != 0)
    value = z3.And(condition, z3.Not(nat))
    if update == 1:
        a, b = z3.And(a, value), z3.And(b, value)
    elif update == 2:
        a, b = z3.Or(a, value), z3.Or(b, value)
    elif update == 3:
        a, b = z3.Or(a, value), z3.And(b, z3.Not(value))
    else:
        a, b = value, z3.And(z3.Not(condition), z3.Not(nat))
    pr = (pr & ~first) | z3.If(a, first, word(0)) | word(1)
    return (pr & ~second) | z3.If(b, second, word(0)) | word(1)


def candidate(pr, first, second, condition, nat, update):
    value = z3.And(condition, z3.Not(nat))
    active = -z3.If(value, word(1), word(0))
    if update == 1:
        pr &= ~((first | second) & ~active)
    elif update == 2:
        pr |= (first | second) & active
    elif update == 3:
        pr = (pr | (first & active)) & ~(second & active)
    else:
        complement = -z3.If(z3.And(z3.Not(condition), z3.Not(nat)), word(1), word(0))
        pr = (pr & ~(first | second)) | (first & ~second & active) | (second & complement)
    return pr | word(1)


pr, first, second = z3.BitVecs("pr first second", 64)
condition, nat = z3.Bools("condition nat")
for update, name in enumerate(("normal/default", "and", "or", "or_andcm")):
    solver = z3.Solver()
    solver.set(timeout=60000)
    solver.add(first & (first - 1) == 0, second & (second - 1) == 0)
    solver.add(
        original(pr, first, second, condition, nat, update)
        != candidate(pr, first, second, condition, nat, update)
    )
    result = solver.check()
    print(name, result, flush=True)
    if result != z3.unsat:
        if result == z3.sat:
            print(solver.model())
        raise SystemExit(1)
