/* SPDX-License-Identifier: GPL-2.0-or-later */
// Stores to a page that holds code (bundle coherence): suite "coherence" and the
// per-store measurement `tier1-test --suite=coh-bench`. A loop whose first
// bundle stores r3 to [r2] and counts:
//   r2 on a data page                  the baseline (no trap);
//   r2 on the loop's own code page, outside its bundles (.Ldata)
//                                      a Tier1 window store faults (the page
//                                      holds code), the site exits, Tier0
//                                      stores with the bundle filter: no
//                                      dependent overlaps, nothing retires;
//   r2 = .Lloop + 8, r3 = that bundle's high word with slot 2 changed
//                                      self-modifying code: the store rewrites
//                                      the add of its own bundle; every engine
//                                      executes the new add from the first
//                                      iteration on.
// Inputs: r2, r3, r8 iterations - 1. Results: r14. Ends with
// `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      mov r14 = r0
      adds r9 = 1, r0
      adds r10 = 2, r0 ;;
    }
    { .mii
      nop.m 0
      mov ar.lc = r8
      nop.i 0 ;;
    }
.Lloop:
    { .mmi
      st8 [r2] = r3
      nop.m 0
      add r14 = r14, r9 ;;
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
        .org 0x800                      // a data word on the code page
.Ldata:
        data8 0
        .include "ivt.inc"
