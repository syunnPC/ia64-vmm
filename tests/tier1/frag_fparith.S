/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FP arithmetic loops for tests/tier1/fp_tests.c: r2, r3 input pointers,
 * r4, r5 output pointers, ar.lc the iteration count. */
.text
.explicit
.global _start
_start:

/* Double: the fma family, its pseudo-ops, FP loads/stores, loop
 * invariant operands (f17, f18) and a rotating destination (f40). */
.org 0x0
.Ld0:
    { .mmi
      ldfd f6 = [r2], 8
      ldfd f7 = [r3], 8
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fma.d.s0 f8 = f6, f7, f8
      nop.i 0
    }
    { .mfi
      nop.m 0
      fms.d.s0 f9 = f6, f7, f9
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fnma.d.s0 f10 = f6, f7, f10
      nop.i 0
    }
    { .mfi
      nop.m 0
      fmpy.d.s0 f11 = f6, f7
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fadd.d.s0 f12 = f6, f7
      nop.i 0
    }
    { .mfi
      nop.m 0
      fsub.d.s0 f13 = f6, f7
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fnorm.d.s0 f14 = f6
      nop.i 0
    }
    { .mfi
      nop.m 0
      fnmpy.d.s0 f15 = f6, f7
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fma.d.s0 f16 = f6, f17, f18
      nop.i 0
    }
    { .mfi
      nop.m 0
      fma.d.s0 f40 = f6, f7, f40
      nop.i 0 ;;
    }
    { .mmi
      stfd [r4] = f11, 8
      stfd [r5] = f12, 8
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Ld0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Single: the same with .s, ldfs/stfs and status field 1. */
.org 0x400
.Ls0:
    { .mmi
      ldfs f6 = [r2], 4
      ldfs f7 = [r3], 4
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fma.s.s1 f8 = f6, f7, f8
      nop.i 0
    }
    { .mfi
      nop.m 0
      fms.s.s0 f9 = f6, f7, f9
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fnma.s.s1 f10 = f6, f7, f10
      nop.i 0
    }
    { .mfi
      nop.m 0
      fmpy.s.s0 f11 = f6, f7
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fadd.s.s1 f12 = f6, f7
      nop.i 0
    }
    { .mfi
      nop.m 0
      fsub.s.s0 f13 = f6, f7
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fnorm.s.s0 f14 = f6
      nop.i 0
    }
    { .mfi
      nop.m 0
      fma.s.s0 f15 = f6, f1, f15
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fms.s.s1 f16 = f6, f17, f18
      nop.i 0 ;;
    }
    { .mmi
      stfs [r4] = f8, 4
      stfs [r5] = f13, 4
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Ls0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Predicates, constants, a +0.0 addend register (SDM Vol.1 5.3.3),
 * compares, conversions, merges, min/max and a division sequence. */
.org 0x800
.Lm0:
    { .mmi
      ldfd f6 = [r2], 8
      ldfd f7 = [r3], 8
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fcmp.lt.unc.s0 p6, p7 = f6, f7
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      (p6) fma.d.s0 f8 = f6, f7, f8
      nop.i 0
    }
    { .mfi
      nop.m 0
      (p7) fms.d.s0 f9 = f6, f1, f9
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fma.d.s0 f10 = f6, f7, f0
      nop.i 0
    }
    { .mfi
      nop.m 0
      fma.d.s0 f11 = f6, f7, f12
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      frcpa.s0 f13, p8 = f6, f7
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      (p8) fnma.s1 f14 = f7, f13, f1
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      (p8) fma.s1 f13 = f13, f14, f13
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      (p8) fmpy.d.s1 f14 = f6, f13
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fcvt.fx.trunc.s0 f15 = f6
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fcvt.xf f15 = f15
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fnorm.d.s0 f15 = f15
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fmerge.ns f12 = f6, f7
      nop.i 0
    }
    { .mfi
      nop.m 0
      fmin.s0 f7 = f6, f7
      nop.i 0 ;;
    }
    { .mmi
      stfd [r4] = f15, 8
      stfd [r5] = f14, 8
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lm0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* No precision completer: pc of the status field (fp_approx). */
.org 0xc00
.Lx0:
    { .mmi
      ldfd f6 = [r2], 8
      ldfd f7 = [r3], 8
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fma.s0 f8 = f6, f7, f8
      nop.i 0
    }
    { .mfi
      nop.m 0
      fma.s1 f9 = f6, f7, f9
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fadd.s0 f10 = f6, f10
      nop.i 0
    }
    { .mfi
      nop.m 0
      fmpy.s1 f11 = f6, f7
      nop.i 0 ;;
    }
    { .mmi
      stfd [r4] = f8, 8
      stfd [r5] = f11, 8
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lx0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Accumulators in GRs (as GCC keeps them under FR pressure): setf.d/s,
 * getf.d/s around the arithmetic, a NaT GR and a NaTVal FR. */
.org 0x1000
.Lg0:
    { .mmi
      ldfd f6 = [r2], 8
      ldfd f7 = [r3], 8
      nop.i 0 ;;
    }
    { .mmi
      setf.d f8 = r20
      setf.s f9 = r21
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fma.d.s0 f8 = f6, f7, f8
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fma.s.s0 f9 = f8, f1, f9
      nop.i 0 ;;
    }
    { .mmi
      getf.d r20 = f8
      getf.s r21 = f9
      nop.i 0 ;;
    }
    { .mmi
      getf.d r22 = f10
      getf.s r23 = f11
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lg0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Parallel single: fpma, fpms, fpnma, fpmpy, f1 ((-0.0, +0.0)) as a
 * factor and as an addend, status field 2, fpack, the packed merges and a
 * rotating destination (f41). */
.org 0x1400
.Lp0:
    { .mmi
      ldf8 f6 = [r2], 8
      ldf8 f7 = [r3], 8
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fpma.s0 f8 = f6, f7, f8
      nop.i 0
    }
    { .mfi
      nop.m 0
      fpms.s1 f9 = f6, f7, f9
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fpnma.s0 f10 = f6, f7, f10
      nop.i 0
    }
    { .mfi
      nop.m 0
      fpmpy.s0 f11 = f6, f7
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fpma.s0 f12 = f6, f1, f12
      nop.i 0
    }
    { .mfi
      nop.m 0
      fpma.s1 f13 = f6, f7, f1
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      (p6) fpma.s2 f14 = f8, f9, f14
      nop.i 0
    }
    { .mfi
      nop.m 0
      fpack f15 = f16, f17
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fpmerge.ns f18 = f8, f15
      nop.i 0
    }
    { .mfi
      nop.m 0
      fpmerge.se f19 = f9, f11
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fpabs f20 = f10
      nop.i 0
    }
    { .mfi
      nop.m 0
      fpnegabs f21 = f19
      nop.i 0
    }
    { .mfi
      nop.m 0
      fpma.s0 f41 = f6, f7, f41
      nop.i 0 ;;
    }
    { .mmi
      stf8 [r4] = f8, 8
      stf8 [r5] = f13, 8
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lp0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* A loop whose first instruction stores an FR the loop does not write. */
.org 0x1800
.Lh0:
    { .mmi
      stfd [r4] = f6, 8
      nop.m 0
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      fma.d.s0 f8 = f7, f7, f8
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lh0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
