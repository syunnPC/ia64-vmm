/* SPDX-License-Identifier: GPL-2.0-or-later */
// call-return (calls, returns and alloc in Tier1 regions; SDM Vol.2 §6, Vol.3
// alloc, br): a counted loop whose body calls functions on the same code
// pages. Per iteration, in order:
//   1  .Lsum(r32, r33) on iterations 0 mod 4, a predicated call from slot
//      2 (Tier1 leaves at .Lsum's head): alloc (sof 6, sol 5), a nested
//      call of .Lleaf, b0 and ar.pfs restored, br.ret;
//   2  .Lleaf(r14) from slot 0 of a BBB bundle: the return skips slots 1
//      and 2 (b0 = the next bundle);
//   3  .Ltwo(r33) on iterations 1 mod 4, a predicated call in slot 1 of an
//      MBB bundle: its frame has two outputs and it calls .Lsum under a
//      predicate, so .Lsum is entered from frames of 4 and 2 outputs;
//   4  .Lleaf(r15) through brl.call;
//   5  .Lec writes ar.ec = 9; br.ret restores ar.ec from PFS.pec (40:
//      all six bits of ar.ec);
//   6  .Lb0 returns to b0 + 16, past the bundle after its call;
//   7  .Lpfs returns with PFM.sof one smaller: a call of .Lleaf then saves
//      that frame in ar.pfs (r19 reads it), and an alloc restores the frame
//      (sof 14, sol 10);
//   8  chk.s on a register with its NaT bit clear (falls through) and on
//      an ld8.s result (in physical mode always deferred: NaT, recovery);
//   9  every fourth iteration .Lrec(r9): r9 levels, each sof 6, sol 5, so
//      deep levels make alloc spill and returns load the frames back;
//  10  .Lleaf(r33) through an indirect call (b6).
// Inputs (tests/tier1/programs.c): r2 the initial r32, r8 ar.lc (n - 1),
// r9 the recursion depth, r10 the address of a data word. Physical mode.
// Results: r14, r15, r16, r17, r18 (programs.c models them).
// Ends with `break.i 0x1ffffe`; .Lbad ends with `break.i 0x1ffffd`.
.text
.explicit
.global _start
_start:
    { .mii
      alloc r40 = ar.pfs, 0, 10, 4, 0    // sof 14, sol 10: locals r32-r41, outputs r42-r45
      mov r32 = r2
      mov r33 = r0 ;;
    }
    { .mii
      mov r14 = r0
      mov r15 = r0
      mov r16 = r0 ;;
    }
    { .mii
      mov r17 = r0
      mov r18 = r0
      mov r34 = 40 ;;
    }
    { .mlx
      nop.m 0
      movl r28 = .Lleaf ;;
    }
    { .mii
      nop.m 0
      mov.i ar.ec = r34
      mov b6 = r28 ;;
    }
    { .mii
      nop.m 0
      mov.i ar.lc = r8
      nop.i 0 ;;
    }
.Lhead:
    { .mii                              // 1
      mov r42 = r32
      mov r43 = r33
      and r27 = 3, r33 ;;
    }
    { .mii
      cmp.eq p7, p0 = 0, r27
      cmp.eq p6, p0 = 1, r27
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p7)  br.call.sptk.many b0 = .Lsum ;;
    }
    { .mii
(p7)  add r14 = r14, r8
      adds r15 = 1, r15
      nop.i 0 ;;
    }
    { .mii                              // 2
      mov r42 = r14
      nop.i 0
      nop.i 0 ;;
    }
    { .bbb
      br.call.sptk.many b0 = .Lleaf
      br.cond.sptk.few .Lbad
      nop.b 0 ;;
    }
    { .mii
      add r14 = r14, r8
      mov r42 = r33                     // 3
      nop.i 0 ;;
    }
    { .mbb
      nop.m 0
(p6)  br.call.sptk.many b0 = .Ltwo
      nop.b 0 ;;
    }
    { .mii
(p6)  add r14 = r14, r8
      mov r42 = r15                     // 4
      nop.i 0 ;;
    }
    { .mlx
      nop.m 0
      brl.call.sptk.many b0 = .Lleaf ;;
    }
    { .mii
      add r14 = r14, r8
      nop.i 0
      nop.i 0 ;;
    }
    { .mib                              // 5
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lec ;;
    }
    { .mii
      nop.m 0
      mov.i r25 = ar.ec ;;
      add r17 = r17, r25 ;;
    }
    { .mib                              // 6
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lb0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lbad ;;          // skipped by .Lb0's return
    }
    { .mib                              // 7
      adds r18 = 1, r18
      nop.i 0
      br.call.sptk.many b0 = .Lpfs ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lleaf ;;
    }
    { .mii
      nop.m 0
      mov.i r19 = ar.pfs
      nop.i 0 ;;
    }
    { .mii
      alloc r41 = ar.pfs, 0, 10, 4, 0
      nop.i 0
      nop.i 0 ;;
    }
    { .mmi                              // 8
      ld8 r20 = [r10]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      chk.s.i r20, .Lbad
      nop.i 0 ;;
    }
    { .mmi
      ld8.s r21 = [r10]
      nop.m 0
      nop.i 0 ;;
    }
    { .mmi
      chk.s.m r21, .Lrecover
      nop.m 0
      nop.i 0 ;;
    }
.Lrecovered:
    { .mii
      add r14 = r14, r21
      and r27 = 3, r33                  // 9
      mov r42 = r9 ;;
    }
    { .mii
      nop.m 0
      cmp.eq p8, p0 = r0, r27
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p8)  br.call.sptk.many b0 = .Lrec ;;
    }
    { .mii
(p8)  add r14 = r14, r8
      mov r42 = r33                     // 10
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = b6 ;;
    }
    { .mii
      add r14 = r14, r8
      adds r33 = 1, r33
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lhead ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
.Lbad:
    { .mii
      nop.m 0
      break.i 0x1ffffd
      nop.i 0 ;;
    }
        .org 0x400                      // tests/tier1/upper_tests.c CALL_PROG_SUM
.Lsum:                                  // r8 = .Lleaf(in0) + in0 + 3 * in1
    { .mii
      alloc r34 = ar.pfs, 2, 3, 1, 0    // in r32-r33, locals r34-r36, out r37: sof 6, sol 5
      mov r35 = b0
      shladd r36 = r33, 1, r33 ;;
    }
    { .mii
      mov r37 = r32
      add r36 = r36, r32
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lleaf ;;
    }
    { .mii
      add r8 = r8, r36
      mov b0 = r35
      mov.i ar.pfs = r34 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
.Lleaf:                                 // r8 = 2 * in0 + 1; no frame of its own
    { .mii                              // in0 through two byte reversals (helpers)
      nop.m 0
      mux1 r8 = r32, @rev ;;
      mux1 r8 = r8, @rev ;;
    }
    { .mii
      nop.m 0
      add r8 = r8, r8 ;;
      adds r8 = 1, r8 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
.Ltwo:                                  // r8 = .Lsum(in0, in0 + 3) + 7
    { .mii
      alloc r33 = ar.pfs, 1, 2, 2, 0    // in r32, locals r33-r34, out r35-r36: sof 5, sol 3
      mov r34 = b0
      cmp.eq p11, p0 = r0, r0 ;;
    }
    { .mii
      mov r35 = r32
      adds r36 = 3, r32
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p11) br.call.sptk.many b0 = .Lsum ;;
    }
    { .mii
      adds r8 = 7, r8
      mov b0 = r34
      mov.i ar.pfs = r33 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
.Lec:
    { .mii
      nop.m 0
      mov r25 = 9
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      mov.i ar.ec = r25
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
.Lb0:
    { .mii
      nop.m 0
      mov r26 = b0 ;;
      adds r26 = 16, r26 ;;
    }
    { .mii
      nop.m 0
      mov b0 = r26
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
.Lpfs:
    { .mii
      nop.m 0
      mov.i r19 = ar.pfs ;;
      adds r19 = -1, r19 ;;
    }
    { .mii
      nop.m 0
      mov.i ar.pfs = r19
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
.Lrecover:
    { .mmi
      ld8 r21 = [r10]
      nop.m 0
      adds r16 = 1, r16 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few .Lrecovered ;;
    }
.Lrec:                                  // r8 = in0 * (in0 + 1) / 2
    { .mii
      alloc r34 = ar.pfs, 1, 4, 1, 0    // in r32, locals r33-r36, out r37: sof 6, sol 5
      mov r35 = b0
      cmp.eq p12, p13 = r0, r32 ;;
    }
    { .mii
      adds r37 = -1, r32
      mov r8 = r0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p13) br.call.sptk.many b0 = .Lrec ;;
    }
    { .mii
      add r8 = r8, r32
      mov b0 = r35
      mov.i ar.pfs = r34 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
        .include "ivt.inc"
