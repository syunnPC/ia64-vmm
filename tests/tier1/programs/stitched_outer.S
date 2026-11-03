/* SPDX-License-Identifier: GPL-2.0-or-later */
// Stitched outer loop: an inner loop whose
// enclosing loop returns by an unconditional branch. A Tier1 region formed
// at the inner header follows that branch through the outer code into the
// head bundle again and closes at a second back-edge to its head, so the
// region holds the inner back-edge, taken every inner iteration, and the
// duplicate's back-edge, taken once per pass. Suites programs, unit_programs and dispatch
// (budgets, splits, interrupts, timer and kicks at both back-edges).
// Inputs from the harness (tests/tier1/programs.c): r8 passes, r9 inner
// iterations - 1, r11 the data pointer (one 8-byte word per inner
// iteration, reread every pass). Results: r14. Ends with `break.i 0x1ffffe`.
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
      cmp.eq p6, p7 = r8, r0
      adds r8 = -1, r8
      mov ar.lc = r9 ;;
    }
    { .mib
      mov r10 = r11
      nop.i 0
      (p6) br.cond.dpnt.few .Lend ;;
    }
.Linner:
    { .mmi
      ld8 r15 = [r10], 8
      nop.m 0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      shladd r14 = r14, 1, r15
      br.cloop.sptk.few .Linner ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Louter ;;
    }
.Lend:
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .include "ivt.inc"
