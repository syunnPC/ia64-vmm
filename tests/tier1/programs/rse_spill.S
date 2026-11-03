/* SPDX-License-Identifier: GPL-2.0-or-later */
// rse-spill (mandatory RSE stores inside Tier1 regions; SDM Vol.2 §6.1,
// §6.5, §6.6, Vol.3 alloc): .Lmain runs above a frame of 70 locals the
// first call preserved. Its counted loop calls .Lwide1 (sof 30), then,
// through b6, descends a recursion of frames of 12 registers (sol 10) to
// the depth d = iteration mod 9; the deepest level calls .Lbottom (sof 3,
// sol 2), which calls two wide functions (sof 30, then sof 50, then the
// first again). Where the dirty registers and a new frame exceed the 96
// stacked registers, alloc spills the oldest dirty registers (with the
// RNaT collections between them) to the backing store, and the returns
// load them back. An alloc at a function head, an alloc after a call
// inside a region, an alloc that spills exactly one register (.Lbottom at
// depth 8 once the 70 locals are spilled; r23 reads ar.bspstore right
// after it) and a later alloc that no longer spills all occur.
// Every level holds a NaT in r33 (ld8.fill with its ar.unat bit set) and
// counts it after its callee returned (tnat), so the NaT bits travel
// through the RNaT collections. After each descent to depth 8 the loop
// purges the translations of the backing-store pages r12 and r13 (0:
// none; ptc.l, SDM Vol.3), so with
// PSR.rt = 1 the first mandatory store on each page takes the Alternate
// Data TLB and Dirty-Bit vectors, in the middle of an alloc's spill.
// Inputs (tests/tier1/programs.c): r2 the initial r32, r8 ar.lc (n - 1),
// r10 the address of a data word (the loop adds it to r14), r11 the
// ar.unat bit of that word, r12 and r13 the pages to purge. Results: r14,
// r15, r32 (programs.c models them). Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      alloc r3 = ar.pfs, 0, 70, 0, 0     // 70 locals the call preserves: dirty below .Lmain's frame
      nop.i 0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lmain ;;
    }
.Lmain:
    { .mii
      alloc r40 = ar.pfs, 0, 10, 2, 0    // sof 12, sol 10: locals r32-r41, outputs r42-r43
      mov r32 = r2
      mov r14 = r0 ;;
    }
    { .mlx
      nop.m 0
      movl r20 = .Ldown ;;
    }
    { .mii
      mov r15 = r0
      mov r34 = r0                       // depth
      mov b6 = r20 ;;
    }
    { .mii
      mov r18 = 12 << 2                  // 4 KiB purges
      cmp.ne p12, p0 = r0, r12
      mov.i ar.lc = r8 ;;
    }
.Lhead:
    { .mib
      mov r42 = 1
      nop.i 0
      br.call.sptk.many b0 = .Lwide1 ;;  // its alloc spills while the preserved locals are dirty
    }
    { .mii
      add r14 = r14, r8
      mov r42 = r34
      adds r34 = 1, r34 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = b6 ;;       // .Ldown(depth)
    }
    { .mii
      add r14 = r14, r8
      cmp.eq p6, p0 = 9, r34
(p12) cmp.eq.unc p13, p0 = 9, r34 ;;
    }
    { .mmi
      ld8 r19 = [r10]
      add r14 = r14, r32
(p6)  mov r34 = r0 ;;
    }
    { .mii
      add r14 = r14, r19
      nop.i 0
      nop.i 0 ;;
    }
    { .mmi
(p13) ptc.l r12, r18 ;;
(p13) ptc.l r13, r18
      nop.i 0 ;;
    }
    { .mmi
(p13) srlz.d
      nop.m 0
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
.Ldown:                                  // r8 = down(in0)
    { .mii
      alloc r34 = ar.pfs, 1, 9, 2, 0     // in r32, locals r33-r41, out r42-r43: sof 12, sol 10
      mov r35 = b0
      cmp.eq p8, p9 = r0, r32 ;;
    }
    { .mmi
      mov.m ar.unat = r11
      nop.m 0
      shladd r36 = r32, 1, r32 ;;        // 3 * in0
    }
    { .mmi
      ld8.fill r33 = [r10]               // NaT: the word's ar.unat bit is set
      adds r41 = 100, r32
      adds r36 = 1, r36 ;;               // 3 * in0 + 1
    }
    { .mii
      adds r42 = -1, r32
      mov r8 = r0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p9)  br.call.sptk.many b0 = .Ldown ;;
    }
    { .mib
      cmp.eq p8, p0 = r0, r32            // the callee changed p8
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p8)  br.call.sptk.many b0 = .Lbottom ;;
    }
    { .mii
      add r8 = r8, r36
      tnat.nz p10, p0 = r33
      nop.i 0 ;;
    }
    { .mii
      add r8 = r8, r41
(p10) adds r15 = 1, r15
      mov b0 = r35 ;;
    }
    { .mib
      nop.m 0
      mov.i ar.pfs = r34
      br.ret.sptk.many b0 ;;
    }
.Lbottom:                                // r8 = wide1(5) + wide2(7) + wide1(9)
    { .mii                               // at depth 8 with nothing dirty below .Lmain: 94 dirty, one spill
      alloc r33 = ar.pfs, 1, 1, 1, 0     // in r32, local r33, out r34: sof 3, sol 2
      mov r21 = b0                       // r21, r22: .Lbottom is no recursion
      mov r34 = 5 ;;
    }
    { .mmi
      mov r23 = ar.bspstore              // after the spill (a region boundary)
      mov r22 = r0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lwide1 ;;
    }
    { .mii
      add r22 = r22, r8
      mov r34 = 7
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lwide2 ;;
    }
    { .mii
      add r22 = r22, r8
      mov r34 = 9
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lwide1 ;;
    }
    { .mii
      add r8 = r22, r8
      mov b0 = r21
      mov.i ar.pfs = r33 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
.Lwide1:                                 // r8 = 2 * in0 + 3
    { .mii
      alloc r40 = ar.pfs, 1, 27, 2, 0    // in r32, locals r33-r59, out r60-r61: sof 30, sol 28
      adds r33 = 1, r32
      adds r59 = 2, r32 ;;
    }
    { .mib
      add r8 = r33, r59
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
.Lwide2:                                 // r8 = 2 * in0 + 7
    { .mii
      alloc r60 = ar.pfs, 1, 47, 2, 0    // in r32, locals r33-r79, out r80-r81: sof 50, sol 48
      adds r79 = 7, r32
      nop.i 0 ;;
    }
    { .mib
      add r8 = r79, r32
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
        .include "ivt.inc"
