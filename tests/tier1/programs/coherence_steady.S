/* SPDX-License-Identifier: GPL-2.0-or-later */
// Steady state of Tier1's entry validation: suite "coherence". An outer loop and an inner
// loop on one code page: every pass enters the inner loop's Tier1 region
// from Tier0's lookup at its header, in one stay of the vCPU on the page,
// so after the first entry no bundle is compared again. The outer branch is
// predicated (p7 = !p6), so formation at the inner header does not stitch
// it and run the outer loop inside that region; the
// instruction before the inner header is a shrp (outside the Tier1
// subset), so no region formed in the outer code holds the inner loop.
// Inputs: r20 passes, r21 inner iterations - 1. Results:
// r14.
// Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      mov r14 = r0
      adds r9 = 1, r0
      nop.i 0 ;;
    }
.Louter:
    { .mii
      cmp.eq p6, p7 = r20, r0
      adds r20 = -1, r20
      mov ar.lc = r21 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      (p6) br.cond.dpnt.few .Lend ;;
    }
    { .mii
      nop.m 0
      nop.i 0
      shrp r30 = r0, r0, 1 ;;
    }
.Linner:
    { .mii
      nop.m 0
      add r14 = r14, r9
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Linner ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      (p7) br.cond.sptk.few .Louter ;;
    }
.Lend:
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .include "ivt.inc"
