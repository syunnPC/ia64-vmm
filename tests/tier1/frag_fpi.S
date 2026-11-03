/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.explicit
.global _start
_start:
.org 0x0
.Lfpi0:
    { .mib
      (p6) setf.sig f10 = r41
      nop.i 0
      br.cloop.sptk.few .Lfpi0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x100
.Lfpi1:
    { .mib
      (p6) setf.sig f40 = r41
      nop.i 0
      br.cloop.sptk.few .Lfpi1 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x200
.Lfpi2:
    { .mib
      (p6) setf.exp f10 = r41
      nop.i 0
      br.cloop.sptk.few .Lfpi2 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x300
.Lfpi3:
    { .mib
      (p6) setf.exp f40 = r41
      nop.i 0
      br.cloop.sptk.few .Lfpi3 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x400
.Lfpi4:
    { .mib
      (p6) getf.sig r40 = f11
      nop.i 0
      br.cloop.sptk.few .Lfpi4 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x500
.Lfpi5:
    { .mib
      (p6) getf.sig r40 = f41
      nop.i 0
      br.cloop.sptk.few .Lfpi5 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x600
.Lfpi6:
    { .mib
      (p6) getf.exp r40 = f11
      nop.i 0
      br.cloop.sptk.few .Lfpi6 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x700
.Lfpi7:
    { .mib
      (p6) getf.exp r40 = f41
      nop.i 0
      br.cloop.sptk.few .Lfpi7 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x800
.Lfpi8:
    { .mfi
      nop.m 0
      (p6) xma.l f10 = f12, f13, f11
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi8 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x900
.Lfpi9:
    { .mfi
      nop.m 0
      (p6) xma.l f40 = f42, f43, f41
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi9 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0xa00
.Lfpi10:
    { .mfi
      nop.m 0
      (p6) xma.h f10 = f12, f13, f11
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi10 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0xb00
.Lfpi11:
    { .mfi
      nop.m 0
      (p6) xma.h f40 = f42, f43, f41
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi11 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0xc00
.Lfpi12:
    { .mfi
      nop.m 0
      (p6) xma.hu f10 = f12, f13, f11
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi12 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0xd00
.Lfpi13:
    { .mfi
      nop.m 0
      (p6) xma.hu f40 = f42, f43, f41
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi13 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0xe00
.Lfpi14:
    { .mfi
      nop.m 0
      (p6) xmpy.hu f10 = f12, f13
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi14 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0xf00
.Lfpi15:
    { .mfi
      nop.m 0
      (p6) xmpy.hu f40 = f42, f43
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi15 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1000
.Lfpi16:
    { .mfi
      nop.m 0
      (p6) fand f10 = f11, f12
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi16 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1100
.Lfpi17:
    { .mfi
      nop.m 0
      (p6) fand f40 = f41, f42
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi17 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1200
.Lfpi18:
    { .mfi
      nop.m 0
      (p6) for f10 = f11, f12
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi18 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1300
.Lfpi19:
    { .mfi
      nop.m 0
      (p6) for f40 = f41, f42
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi19 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1400
.Lfpi20:
    { .mfi
      nop.m 0
      (p6) fxor f10 = f11, f12
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi20 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1500
.Lfpi21:
    { .mfi
      nop.m 0
      (p6) fxor f40 = f41, f42
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi21 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1600
.Lfpi22:
    { .mfi
      nop.m 0
      (p6) fandcm f10 = f11, f12
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi22 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1700
.Lfpi23:
    { .mfi
      nop.m 0
      (p6) fandcm f40 = f41, f42
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi23 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1800
.Lfpi24:
    { .mfi
      nop.m 0
      (p6) fmix.lr f10 = f11, f12
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi24 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1900
.Lfpi25:
    { .mfi
      nop.m 0
      (p6) fmix.lr f40 = f41, f42
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi25 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1a00
.Lfpi26:
    { .mfi
      nop.m 0
      (p6) fmix.l f10 = f11, f12
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi26 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1b00
.Lfpi27:
    { .mfi
      nop.m 0
      (p6) fmix.l f40 = f41, f42
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi27 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1c00
.Lfpi28:
    { .mfi
      nop.m 0
      (p6) fmix.r f10 = f11, f12
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi28 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1d00
.Lfpi29:
    { .mfi
      nop.m 0
      (p6) fmix.r f40 = f41, f42
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi29 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1e00
.Lfpi30:
    { .mfi
      nop.m 0
      (p6) fsxt.l f10 = f11, f12
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi30 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1f00
.Lfpi31:
    { .mfi
      nop.m 0
      (p6) fsxt.l f40 = f41, f42
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi31 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x2000
.Lfpi32:
    { .mfi
      nop.m 0
      (p6) fsxt.r f10 = f11, f12
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi32 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x2100
.Lfpi33:
    { .mfi
      nop.m 0
      (p6) fsxt.r f40 = f41, f42
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lfpi33 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x2400
    { .mib
      setf.sig f10 = r41
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      ld8 r40 = [r2]
      nop.i 0
      nop.b 0 ;;
    }
.org 0x2500
    { .mib
      setf.sig f40 = r41
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      ld8 r40 = [r2]
      nop.i 0
      nop.b 0 ;;
    }

/* Native dirtiness must survive a later helper write to the other bank. */
.org 0x2600
    { .mib
      setf.sig f10 = r41
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      ldf.fill f40 = [r2]
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x2700
    { .mib
      setf.sig f40 = r41
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      ldf.fill f10 = [r2]
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* A helper-produced NaTVal is consumed and restored by native FP-int. */
.org 0x2800
    { .mib
      ldf.fill f11 = [r2]
      nop.i 0
      nop.b 0 ;;
    }
    { .mfb
      nop.m 0
      xma.l f10 = f11, f12, f13
      nop.b 0 ;;
    }
    { .mib
      getf.sig r40 = f10
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* A helper-produced NaTVal is consumed and restored by native FP-int. */
.org 0x2900
    { .mib
      ldf.fill f41 = [r2]
      nop.i 0
      nop.b 0 ;;
    }
    { .mfb
      nop.m 0
      xma.l f40 = f41, f42, f43
      nop.b 0 ;;
    }
    { .mib
      getf.sig r40 = f40
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Preserve a native f9 across a single-FR register postincrement:
 * native accesses (ldfd, ldfs, ldf8) and helpers (ldfe, ldf.fill). */
.org 0x2a00
    { .mib
      setf.sig f9 = r8
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      ldfd f7 = [r3], r9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      getf.sig r10 = f9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Preserve a native f9 across a single-FR register postincrement. */
.org 0x2b00
    { .mib
      setf.sig f9 = r8
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      ldfs f7 = [r3], r9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      getf.sig r10 = f9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Preserve a native f9 across a single-FR register postincrement. */
.org 0x2c00
    { .mib
      setf.sig f9 = r8
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      ldf8 f7 = [r3], r9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      getf.sig r10 = f9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Preserve a native f9 across a single-FR register postincrement. */
.org 0x2d00
    { .mib
      setf.sig f9 = r8
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      ldfe f7 = [r3], r9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      getf.sig r10 = f9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Preserve a native f9 across a single-FR register postincrement. */
.org 0x2e00
    { .mib
      setf.sig f9 = r8
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      ldf.fill f7 = [r3], r9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      getf.sig r10 = f9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Speculative load defers a NaT base fault, then enters a native region. */
.org 0x2f00
    { .mib
      ldfd.s f11 = [r3]
      nop.i 0
      nop.b 0 ;;
    }
    { .mfi
      nop.m 0
      xma.l f10 = f11, f12, f13
      nop.i 0 ;;
    }
    { .mib
      getf.sig r10 = f10
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* High-bank multiply with the constant f0 addend and DFL disabled. */
.org 0x3000
    { .mfi
      nop.m 0
      xmpy.l f40 = f41, f42
      nop.i 0 ;;
    }
    { .mib
      getf.sig r10 = f40
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* SDM Vol.3 setf.sig -> xma.l -> getf.sig, with a loop-carried FR. */
.org 0x3100
.Lhash:
    { .mib
      setf.sig f2 = r8
      nop.i 0
      nop.b 0 ;;
    }
    { .mfi
      nop.m 0
      xma.l f3 = f2, f3, f4
      nop.i 0 ;;
    }
    { .mib
      getf.sig r9 = f3
      nop.i 0
      br.cloop.sptk.few .Lhash ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }

/* Keep several FR values and tags live across the NaTVal checks. */
.org 0x3200
    { .mii
      nop.m 0
      xor r12 = 12, r12
      xor r13 = 13, r13 ;;
    }
    { .mii
      nop.m 0
      xor r14 = 14, r14
      xor r15 = 15, r15 ;;
    }
    { .mii
      nop.m 0
      xor r16 = 16, r16
      xor r17 = 17, r17 ;;
    }
    { .mii
      nop.m 0
      xor r18 = 18, r18
      xor r19 = 19, r19 ;;
    }
    { .mii
      nop.m 0
      xor r20 = 20, r20
      xor r21 = 21, r21 ;;
    }
    { .mii
      nop.m 0
      xor r22 = 22, r22
      nop.i 0 ;;
    }
    { .mib
      setf.sig f8 = r8
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      setf.sig f9 = r9
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      setf.sig f10 = r10
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      setf.sig f11 = r11
      nop.i 0
      nop.b 0 ;;
    }
    { .mfi
      nop.m 0
      xma.l f3 = f8, f2, f3
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      xma.l f4 = f9, f2, f4
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      xma.l f5 = f10, f2, f5
      nop.i 0 ;;
    }
    { .mfi
      nop.m 0
      xma.l f6 = f11, f2, f6
      nop.i 0 ;;
    }
    { .mib
      getf.sig r8 = f3
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      getf.sig r9 = f4
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      getf.sig r10 = f5
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      getf.sig r11 = f6
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
