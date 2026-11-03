/* SPDX-License-Identifier: GPL-2.0-or-later */
// Store pairing (checked execution): a counted
// loop in physical mode whose stores have no base update, the form Tier0
// executes inline through tier0_memory_pointer when no memory hook is
// installed (engine/tier0/translate.c eligible()). Each iteration reads a
// word of the array, stores the running sum back into it (st8) and its low
// half into the next word (st4), then advances the pointer by an add.
// Inputs (tests/tier1/ver_tests.c): r2 the array (8-byte aligned, RAM),
// r8 ar.lc (n - 1). Results: r14 the sum, the array rewritten.
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
      adds r17 = 8, r2 ;;
    }
    { .mii
      add r14 = r14, r16
      adds r15 = 3, r15
      nop.i 0 ;;
    }
    { .mmi
      st8 [r2] = r14
      st4 [r17] = r15
      adds r2 = 8, r2 ;;
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
