/* SPDX-License-Identifier: GPL-2.0-or-later */
// forward-progress (product dispatch): a loop
// whose header is a load from an MMIO register, so a Tier1 region at the
// header leaves at its very first instruction, before effect, on every
// entry (the window refuses MMIO). The progress rule makes Tier0 execute
// that load exactly once before Tier1 is consulted again at the header.
// Inputs (tests/tier1/upper_tests.c): r2 the address of a counting MMIO
// register (each read returns the number of reads so far), r8 ar.lc (n - 1).
// Results: r14 = the sum of the n values read, r15 an order-sensitive mix.
// Ends with `break.i 0x1ffffe`.
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
    { .mmi
      ld8 r16 = [r2]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      add r14 = r14, r16
      shladd r15 = r15, 1, r16
      nop.i 0 ;;
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
