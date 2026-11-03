/* SPDX-License-Identifier: GPL-2.0-or-later */
// tag-keep (product dispatch):
// an outer loop running loop A and, with r9 bit 0, loop B. A loads through
// r2 (its own tag) and r4 (a tag shared with B); B through r3 (its own tag)
// and r4. Each tag is a distinct window tag (VRN and payload bits above
// w = 40 and the fallback 36). Virtual mode; the harness
// (tests/tier1/upper_tests.c) maps every address with a D-TC entry.
// Inputs: r2, r3, r4 the addresses, r8 ar.lc of each inner loop (n - 1),
// r9 the phase bits, r10 the outer count. Result: r14 = the sum of every
// word loaded. Ends with `break.i 0x1ffffe`. The outer code reads ar.itc,
// which no region holds, before each loop: no region
// holds the outer loop with its inner loops, so every
// outer iteration enters the loops' regions again.
.text
.explicit
.global _start
_start:
    { .mii
      mov r14 = r0
      nop.i 0
      nop.i 0 ;;
    }
.Louter:
    { .mii
      mov r15 = ar.itc
      mov ar.lc = r8
      tbit.z p6, p7 = r9, 0 ;;
    }
.LA:
    { .mmi
      ld8 r16 = [r2]
      ld8 r17 = [r4]
      nop.i 0 ;;
    }
    { .mii
      add r14 = r14, r16
      nop.i 0 ;;
      add r14 = r14, r17
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .LA ;;
    }
    { .mib
      mov r15 = ar.itc
      mov ar.lc = r8
(p6)  br.cond.dptk.few .Lnext ;;
    }
.LB:
    { .mmi
      ld8 r16 = [r3]
      ld8 r17 = [r4]
      nop.i 0 ;;
    }
    { .mii
      add r14 = r14, r16
      nop.i 0 ;;
      add r14 = r14, r17
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .LB ;;
    }
.Lnext:
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
