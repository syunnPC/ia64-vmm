/* SPDX-License-Identifier: GPL-2.0-or-later */
// Loop nest: an outer loop counted by r8 whose
// body loads one word, runs an inner br.cloop loop over r9 + 1 words and
// stores the running value. A Tier1 region at the outer header holds the
// inner loop as an inner loop with its own checkpoint. Suites programs, unit_programs and
// dispatch (budgets, splits, interrupts, timer and kicks inside the nest).
// Inputs from the harness (tests/tier1/programs.c): r8 passes, r9 inner
// iterations - 1, r11 the inner data (reread every pass), r18 the outer
// data (one word per pass), r19 the output (one word per pass). Results:
// r14, r15 and the output words. Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      mov r14 = r0
      mov r15 = r0
      nop.i 0 ;;
    }
.Louter:
    { .mii
      ld8 r16 = [r18], 8
      adds r8 = -1, r8
      mov ar.lc = r9 ;;
    }
    { .mii
      mov r10 = r11
      nop.i 0
      nop.i 0 ;;
    }
.Linner:
    { .mmi
      ld8 r17 = [r10], 8
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      shladd r14 = r14, 1, r17
      add r15 = r15, r16 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Linner ;;
    }
    { .mii
      cmp.eq p7, p6 = r8, r0
      nop.i 0
      nop.i 0 ;;
    }
    { .mib
      st8 [r19] = r14, 8
      nop.i 0
      (p6) br.cond.dptk.few .Louter ;;
    }
.Lend:
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .include "ivt.inc"
