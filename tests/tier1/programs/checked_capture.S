/* SPDX-License-Identifier: GPL-2.0-or-later */
// Checked lockstep, the C side's page capture
// (engine/tier1/checked.c): run with gather-virt's guest
// MMU (tests/tier1/programs.c), a loop that reads, increments and stores a
// word on VHPT page 0 (mapped by DTR1) and then loads from the next page of
// the table in region 2, whose translation the VHPT walker fills from VHPT
// page 0. Inside a Tier1 execution the walk runs in hostwin_resolve, on the
// C side, after the native store to the same page: the capture must undo
// that store, or the replay's load of the word sees the stored value.
// Inputs (tests/tier1/ver_tests.c): r2 the word's VA, r3 the table's VA,
// r8 ar.lc (n - 1, n <= 8: the table pages hashing into VHPT page 0).
// Results: r14 the sum of the words stored, r15 of the table words loaded.
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
      nop.m 0
      adds r16 = 1, r16
      nop.i 0 ;;
    }
    { .mmi
      st8 [r2] = r16
      ld8 r17 = [r3]
      adds r3 = 4096, r3 ;;
    }
    { .mii
      add r14 = r14, r16
      add r15 = r15, r17
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
