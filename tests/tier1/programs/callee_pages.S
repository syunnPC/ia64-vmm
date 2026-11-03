/* SPDX-License-Identifier: GPL-2.0-or-later */
// callee-pages (IP-relative calls whose callee lies on another code page; SDM
// Vol.3 br.call, fc, sync.i, itr.i, ptr.i; Vol.2 §4.1): a counted loop on
// code page 1 calls .Lfar1 on page 0 (below the loop's page), .Lnear on
// page 2 and .Lfar2 on page 5 (four pages above the loop's), which calls
// .Lfar1 itself. After the calls of iteration r9 the loop rewrites the
// first bundle of .Lfar1 with the bundle at .Lpatch (st8, then fc, sync.i
// and srlz.i). With PSR.it = 1 (the code mapped by one 64 KiB ITR at VRN 0
// and its alias at VRN 4), after the calls of iteration r10 .Lremap,
// running at the VRN 4 alias (b6 = r11), replaces the 64 KiB ITR
// by ITRs of 4, 8 and 16 KiB, and the 4 KiB one maps page 0 to the copy
// of that page at image offset 0x6000, whose .Lfar1 differs.
// far1(a) = a + c with c = 5, 33 after the rewrite, 77 at the copy;
// near(a) = 2a + 1; far2(a) = far1(3a) + 11.
// Inputs (tests/tier1/programs.c): r8 ar.lc (n - 1), r9 the rewrite
// iteration, r10 the remap iteration (all ones: none), r11 the VRN 4
// address of .Lremap. Results: r14 (the sum of all calls), r33 (n).
// Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      alloc r40 = ar.pfs, 0, 10, 2, 0    // sof 12, sol 10: locals r32-r41, outputs r42-r43
      mov r14 = r0
      mov r33 = r0 ;;
    }
    { .mii
      nop.m 0
      mov b6 = r11
      mov.i ar.lc = r8 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few .Lmain ;;
    }
        .org 0x400
.Lfar1:                                  // r8 = in0 + 5
    { .mii
      adds r8 = 5, r32
      nop.i 0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
        .org 0x1000
.Lmain:
    { .mib
      mov r42 = r33
      nop.i 0
      br.call.sptk.many b0 = .Lfar1 ;;
    }
    { .mib
      add r14 = r14, r8
      mov r42 = r33
      br.call.sptk.many b0 = .Lnear ;;
    }
    { .mib
      add r14 = r14, r8
      mov r42 = r33
      br.call.sptk.many b0 = .Lfar2 ;;
    }
    { .mii
      add r14 = r14, r8
      cmp.eq p6, p0 = r33, r9
      cmp.eq p7, p0 = r33, r10 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p6)  br.call.sptk.many b0 = .Lpatch_code ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p7)  br.call.sptk.many b0 = b6 ;;
    }
    { .mib
      adds r33 = 1, r33
      nop.i 0
      br.cloop.sptk.few .Lmain ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .org 0x2000
.Lnear:                                  // r8 = 2 * in0 + 1
    { .mii
      nop.m 0
      add r8 = r32, r32 ;;
      adds r8 = 1, r8 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
.Lpatch_code:                            // .Lfar1's first bundle = .Lpatch
    { .mlx
      nop.m 0
      movl r24 = .Lpatch ;;
    }
    { .mlx
      nop.m 0
      movl r26 = .Lfar1 ;;
    }
    { .mmi
      ld8 r25 = [r24], 8 ;;
      ld8 r27 = [r24]
      nop.i 0 ;;
    }
    { .mmi
      st8 [r26] = r25, 8 ;;
      st8 [r26] = r27
      adds r26 = -8, r26 ;;
    }
    { .mmi
      fc r26 ;;
      nop.m 0
      nop.i 0 ;;
    }
    { .mmi
      sync.i ;;
      srlz.i
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
        .org 0x2400                      // programs.c CALLEE_PAGES_REMAP_VA
.Lremap:                                 // runs at the VRN 4 alias of this page
    { .mmi
      rsm psr.ic | psr.i ;;
      srlz.i
      nop.i 0 ;;
    }
    { .mlx
      nop.m 0
      movl r24 = 0x100000 ;;
    }
    { .mii
      mov r25 = 16 << 2
      nop.i 0
      nop.i 0 ;;
    }
    { .mmi
      ptr.i r24, r25 ;;
      srlz.i
      nop.i 0 ;;
    }
    // ITR 0: VA 0x100000, 4 KiB -> the copy at PA 0x106000.
    { .mlx
      mov r25 = 12 << 2
      movl r26 = 0x106000 | 0x661 ;;     // p, a, d, ar = 3 (RWX), ma = WB
    }
    { .mmi
      mov cr.ifa = r24
      mov cr.itir = r25
      mov r27 = 0 ;;
    }
    { .mmi
      srlz.d ;;
      itr.i itr[r27] = r26
      nop.i 0 ;;
    }
    // ITR 2: VA 0x101000, 4 KiB.
    { .mlx
      nop.m 0
      movl r24 = 0x101000 ;;
    }
    { .mlx
      mov r27 = 2
      movl r26 = 0x101000 | 0x661 ;;
    }
    { .mmi
      mov cr.ifa = r24
      mov cr.itir = r25
      nop.i 0 ;;
    }
    { .mmi
      srlz.d ;;
      itr.i itr[r27] = r26
      nop.i 0 ;;
    }
    // ITR 3: VA 0x102000, 8 KiB.
    { .mlx
      mov r25 = 13 << 2
      movl r24 = 0x102000 ;;
    }
    { .mlx
      mov r27 = 3
      movl r26 = 0x102000 | 0x661 ;;
    }
    { .mmi
      mov cr.ifa = r24
      mov cr.itir = r25
      nop.i 0 ;;
    }
    { .mmi
      srlz.d ;;
      itr.i itr[r27] = r26
      nop.i 0 ;;
    }
    // ITR 4: VA 0x104000, 16 KiB (pages 4 to 7).
    { .mlx
      mov r25 = 14 << 2
      movl r24 = 0x104000 ;;
    }
    { .mlx
      mov r27 = 4
      movl r26 = 0x104000 | 0x661 ;;
    }
    { .mmi
      mov cr.ifa = r24
      mov cr.itir = r25
      nop.i 0 ;;
    }
    { .mmi
      srlz.d ;;
      itr.i itr[r27] = r26
      nop.i 0 ;;
    }
    // ITRs 5 and 6: VA 0x108000 and 0x10c000, 16 KiB each (the IVT).
    { .mlx
      mov r27 = 5
      movl r24 = 0x108000 ;;
    }
    { .mlx
      nop.m 0
      movl r26 = 0x108000 | 0x661 ;;
    }
    { .mmi
      mov cr.ifa = r24
      mov cr.itir = r25
      nop.i 0 ;;
    }
    { .mmi
      srlz.d ;;
      itr.i itr[r27] = r26
      nop.i 0 ;;
    }
    { .mlx
      mov r27 = 6
      movl r24 = 0x10c000 ;;
    }
    { .mlx
      nop.m 0
      movl r26 = 0x10c000 | 0x661 ;;
    }
    { .mmi
      mov cr.ifa = r24
      mov cr.itir = r25
      nop.i 0 ;;
    }
    { .mmi
      srlz.d ;;
      itr.i itr[r27] = r26
      nop.i 0 ;;
    }
    { .mmi
      ssm psr.ic | psr.i ;;
      srlz.i
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
        .org 0x5000
.Lfar2:                                  // r8 = far1(3 * in0) + 11
    { .mii
      alloc r34 = ar.pfs, 1, 3, 1, 0     // in r32, locals r33-r35, out r36: sof 5, sol 4
      mov r35 = b0
      shladd r36 = r32, 1, r32 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lfar1 ;;
    }
    { .mii
      adds r8 = 11, r8
      mov b0 = r35
      mov.i ar.pfs = r34 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
        .org 0x6400                      // .Lfar1 on the copy of page 0
    { .mii
      adds r8 = 77, r32
      nop.i 0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
        .org 0x7000
.Lpatch:                                 // the rewritten first bundle of .Lfar1
    { .mii
      adds r8 = 33, r32
      nop.i 0
      nop.i 0 ;;
    }
        .include "ivt.inc"
