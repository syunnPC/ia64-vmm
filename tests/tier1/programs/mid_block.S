/* SPDX-License-Identifier: GPL-2.0-or-later */
// mid-block-exit: a load after two completed pure
// instructions of the same basic block, so a forced exit at the load has a
// non-zero retired offset inside its block, and a run continues through
// the lower tier within the same t1_run(budget).
// Inputs from the harness (tests/tier1/programs.c): r2/r3 the initial
// r16/r17, r11 the data pointer (8-byte words, post-incremented), r8 ar.lc.
// A tbit (an in-region helper) decides whether a word is
// added twice, so the restoration tests also force a helper exit here.
// Results: r16, r17, r20 (sum of the loaded words, those with bit 3 clear
// twice). Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      alloc r37 = ar.pfs, 0, 8, 0, 0
      mov r16 = r2
      mov r17 = r3 ;;
    }
    { .mii
      mov r32 = r11
      mov ar.lc = r8
      mov r20 = r0 ;;
    }
.Lloop:
    { .mii
      nop.m 0
      add r16 = r16, r17              // pure 1
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      shladd r17 = r16, 2, r17        // pure 2
      nop.i 0 ;;
    }
    { .mmi
      ld8 r19 = [r32], 8              // the load, third in its block
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      tbit.z p6, p7 = r19, 3 ;;       // in-region helper
      nop.i 0
    }
    { .mii
      nop.m 0
(p6)  add r20 = r20, r19
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r20 = r20, r19
      xor r16 = r16, r19 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lloop ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .include "ivt.inc"
