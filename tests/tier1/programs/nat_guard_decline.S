/* SPDX-License-Identifier: GPL-2.0-or-later */
// forward-progress NaT guard (product dispatch):
// a loop that reads r20, whose NaT bit the harness sets, so a Tier1 region
// at the header has r20 in its NaT guard and every entry is declined; the
// NaT propagates to r14 in Tier0 (no consumption fault: add, shladd).
// Inputs (tests/tier1/upper_tests.c): r20 (NaT set), r8 ar.lc (n - 1).
// Results: r14, r15. Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      mov r14 = r0
      mov ar.lc = r8
      mov r15 = r0 ;;
    }
.Lloop:
    { .mii
      nop.m 0
      add r14 = r14, r20
      adds r15 = 3, r15 ;;
    }
    { .mib
      nop.m 0
      shladd r15 = r15, 1, r14
      br.cloop.sptk.few .Lloop ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .include "ivt.inc"
