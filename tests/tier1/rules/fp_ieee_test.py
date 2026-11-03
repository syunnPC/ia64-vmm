# SPDX-License-Identifier: GPL-2.0-or-later
"""IEEE 754 RNE examples and SDM FPSR/MXCSR field mappings.

The tie examples use exact powers of two.  (1+2^-p)*(1-2^-p)-1
distinguishes a fused operation from a rounded product followed by add.
Run with the pinned z3 venv and -I, like prove.py.
"""

import importlib.util
import os
import unittest

import z3

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("sem_ia64", os.path.join(HERE, "sem_ia64.py"))
sem = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sem)


class IEEE(unittest.TestCase):
    def value(self, name, args):
        width = 32 if name.endswith("_s") else 64
        term = sem.fp_op(name)(*[z3.BitVecVal(v, width) for v in args])
        return z3.simplify(term).as_long()

    def test_native_operations(self):
        for fmt, width, kind in [("s", 32, z3.Float32()), ("d", 64, z3.Float64())]:
            for op, expected in [
                ("add", z3.Z3_OP_FPA_ADD),
                ("mul", z3.Z3_OP_FPA_MUL),
                ("fma", z3.Z3_OP_FPA_FMA),
            ]:
                args = [
                    z3.BitVec("%s_%d" % (fmt, k), width) for k in range(3 if op == "fma" else 2)
                ]
                term = sem.fp_op(op + "_" + fmt)(*args)
                self.assertEqual(term.decl().kind(), z3.Z3_OP_FPA_TO_IEEE_BV)
                self.assertEqual(term.arg(0).decl().kind(), expected)
                self.assertEqual(term.arg(0).sort(), kind)
                self.assertEqual(term.arg(0).arg(0), z3.RNE())

    def test_rounding_and_signed_zero(self):
        for fmt, one, ulp, half_ulp, sign, tiny, half, two, infinity, largest in [
            (
                "s",
                0x3F800000,
                1,
                0x33800000,
                1 << 31,
                0x00800000,
                0x3F000000,
                0x40000000,
                0x7F800000,
                0x7F7FFFFF,
            ),
            (
                "d",
                0x3FF0000000000000,
                1,
                0x3CA0000000000000,
                1 << 63,
                0x0010000000000000,
                0x3FE0000000000000,
                0x4000000000000000,
                0x7FF0000000000000,
                0x7FEFFFFFFFFFFFFF,
            ),
        ]:
            with self.subTest(fmt=fmt):
                self.assertEqual(self.value("add_" + fmt, [one, half_ulp]), one)
                self.assertEqual(self.value("add_" + fmt, [one + ulp, half_ulp]), one + 2 * ulp)
                self.assertEqual(self.value("add_" + fmt, [sign, sign]), sign)
                self.assertEqual(self.value("mul_" + fmt, [sign, one]), sign)
                self.assertEqual(self.value("fma_" + fmt, [sign, one, 0]), 0)
                self.assertEqual(self.value("fma_" + fmt, [sign, one, sign]), sign)
                self.assertEqual(self.value("mul_" + fmt, [tiny, half]), tiny >> 1)
                self.assertEqual(self.value("mul_" + fmt, [1, half]), 0)
                self.assertEqual(self.value("mul_" + fmt, [largest, two]), infinity)

    def test_fused_rounding(self):
        for fmt, one, left, right, expected in [
            ("s", 0x3F800000, 0x3F800001, 0x3F7FFFFE, 0xA8800000),
            ("d", 0x3FF0000000000000, 0x3FF0000000000001, 0x3FEFFFFFFFFFFFFE, 0xB970000000000000),
        ]:
            with self.subTest(fmt=fmt):
                sign = 1 << (31 if fmt == "s" else 63)
                self.assertEqual(self.value("fma_" + fmt, [left, right, one | sign]), expected)
                prod = self.value("mul_" + fmt, [left, right])
                self.assertEqual(self.value("add_" + fmt, [prod, one | sign]), 0)

    def test_soft_normal_rounding(self):
        # SDM Figure 5-12: nearest-even with guard and sticky information.
        for precision in (24, 53):
            shift = 64 - precision
            one = 1 << (precision - 1)
            cases = [
                (1 << 63, False, one, False, False),
                ((1 << 63) + (1 << (shift - 1)), False, one, True, False),
                ((1 << 63) + (1 << (shift - 1)), True, one + 1, True, True),
                ((1 << 63) + (3 << (shift - 1)), False, one + 2, True, True),
                ((1 << 64) - 1, False, 1 << precision, True, True),
            ]
            for mant, tail, expected, inexact, up in cases:
                result = sem.fp_round_normal(z3.BitVecVal(mant, 64), z3.BoolVal(tail), precision)
                self.assertEqual(z3.simplify(result[0]).as_long(), expected)
                self.assertEqual(z3.is_true(z3.simplify(result[1])), inexact)
                self.assertEqual(z3.is_true(z3.simplify(result[2])), up)


class Status(unittest.TestCase):
    def test_flag_fields(self):
        for sf in range(4):
            shift = 13 + 13 * sf
            for flag in range(6):
                fpsr = z3.BitVecVal(0, 64)
                csr = z3.BitVecVal(1 << flag, 64)
                self.assertEqual(
                    z3.simplify(sem.fp_apply_flags(fpsr, sf, csr)).as_long(), 1 << (shift + flag)
                )

    def test_mxcsr_controls(self):
        for sf in range(4):
            for rc in range(4):
                fpsr = z3.BitVecVal(rc << (10 + 13 * sf), 64)
                self.assertEqual(z3.simplify(sem.fp_mxcsr(fpsr, sf)).as_long(), 0x1F80 | (rc << 13))

    def test_traps(self):
        # traps.id masks I globally; sf1..3.td masks all local traps.
        for sf in range(4):
            pe = z3.BitVecVal(32, 64)
            self.assertFalse(z3.is_true(z3.simplify(sem.fp_traps(z3.BitVecVal(32, 64), sf, pe))))
            self.assertTrue(z3.is_true(z3.simplify(sem.fp_traps(z3.BitVecVal(0, 64), sf, pe))))
            if sf:
                fpsr = z3.BitVecVal(1 << (12 + 13 * sf), 64)
                self.assertFalse(z3.is_true(z3.simplify(sem.fp_traps(fpsr, sf, pe))))

    def test_guard_status_theorem(self):
        fpsr, sf, csr = z3.BitVecs("fpsr sf csr", 64)
        for exact in (False, True):
            solver = z3.Solver()
            solver.set(timeout=10000)
            solver.add(z3.ULE(sf, 3), sem.fp_controls(fpsr, sf, exact), csr & 31 == 0)
            self.assertEqual(solver.check(), z3.sat)
            bad = z3.Or(
                sem.fp_apply_flags(fpsr, sf, csr) != fpsr,
                sem.fp_traps(fpsr, sf, csr),
                sem.fp_mxcsr(fpsr, sf) != 0x1F80,
            )
            solver.add(bad)
            self.assertEqual(solver.check(), z3.unsat)

    def test_missing_inexact_guards(self):
        # Without either i already set or traps.id, an inexact operation
        # respectively changes FPSR or traps.
        fpsr = z3.BitVecVal(32, 64)
        pe = z3.BitVecVal(32, 64)
        self.assertNotEqual(z3.simplify(sem.fp_apply_flags(fpsr, 0, pe)).as_long(), 32)
        fpsr = z3.BitVecVal(1 << 18, 64)  # sf0.i already set, traps.id clear
        self.assertTrue(z3.is_true(z3.simplify(sem.fp_traps(fpsr, 0, pe))))


if __name__ == "__main__":
    unittest.main()
