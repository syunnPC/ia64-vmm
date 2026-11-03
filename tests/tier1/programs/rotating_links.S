/* SPDX-License-Identifier: GPL-2.0-or-later */
// Rotating links (product dispatch):
// every outer iteration enters the loop header twice, first with the
// rotating bases cleared (clrrrb), then after one br.ctop rotation
// (rrb.gr = 7, rrb.pr = 47). Tier0's key masks the rotation, so one Tier0
// region at the header stands for two Tier1 regions (Tier1's contract keeps
// the complete CFM): each entry checks the CFM and re-probes when its link
// names the other rotation. The body reads and writes rotating registers,
// so a region run under the wrong bases computes other values.
// Inputs (tests/tier1/upper_tests.c): r2 a seed, r10 the outer count.
// Results: r14, r32-r39. Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      alloc r9 = ar.pfs, 0, 8, 0, 8     // sof = sol = sor = 8: r32-r39 rotate
      mov r14 = r0
      nop.i 0 ;;
    }
    { .mii
      mov r32 = r2
      adds r33 = 1, r2
      adds r34 = 2, r2 ;;
    }
    { .mii
      adds r35 = 3, r2
      adds r36 = 4, r2
      adds r37 = 5, r2 ;;
    }
    { .mii
      adds r38 = 6, r2
      adds r39 = 7, r2
      mov ar.ec = r0 ;;
    }
.Louter:
    { .mib
      nop.m 0
      mov ar.lc = 1
      clrrrb ;;
    }
    // An indirect branch, neither the fall-through nor an IP-relative
    // branch a trace region could follow into the header: the header is a
    // Tier0 lookup point under both rotations.
    { .mlx
      nop.m 0
      movl r3 = .Lhead ;;
    }
    { .mii
      nop.m 0
      mov b6 = r3
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few b6 ;;
    }
.Lhead:                                 // 11 instructions per entry (above P_min = 8)
    { .mii
      add r14 = r14, r32
      shladd r35 = r33, 1, r34
      sub r38 = r36, r33 ;;
    }
    { .mii
      xor r14 = r14, r35
      adds r37 = 9, r36
      add r36 = r38, r34 ;;
    }
    { .mii
      or r33 = r37, r32
      shladd r14 = r14, 2, r39
      xor r39 = r39, r38 ;;
    }
    { .mib
      add r14 = r14, r33
      add r32 = r32, r35
      br.ctop.sptk.few .Lhead ;;
    }
    { .mii
      adds r10 = -1, r10
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      cmp.eq p8, p9 = r0, r10
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p9)  br.cond.dptk.few .Louter ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .include "ivt.inc"
