/* SPDX-License-Identifier: GPL-2.0-or-later */
// Bundle-granular code coherence: suite "coherence"
// (tests/tier1/coherence_tests.c). Two loops share code page L (0x101000):
//   A  header A0 at L + 0xfd0, bundles A0..A4, the last two on page L + 1:
//      a Tier1 region with bundles on its entry page and on a non-entry
//      page. A0 ends in a forward (p7) branch to A2 (p7 is always false),
//      so Tier0's region at the header holds A0 alone while the Tier1
//      region also holds A1 and A2 (tier1-only-bundle).
//   B  header B0 at L + 0xe00, two bundles: a second region on page L.
// T (page 0, its last two bundles) falls through into E0, the first bundle
// of page L, whose slot 0 (mov from ar.rnat) is outside the Tier1 subset:
// a region formed at T reads E0 and ends before it, so page L is not one of
// its sources. U0 (L + 0x800), E0 and A5 (after A's back-edge, page L + 1)
// are code that only Tier0 or the lower tier runs: no Tier1 region holds
// them. The driver on page 0 reaches A, B, U0 and T by forward branches
// (the v1 engine's hotness compiles only backward-branch targets) and runs
// each once per pass: r20 passes, r21 + 1 iterations per loop. Each bundle
// adds a constant to its own accumulator, so the results show which code
// ran:
//   A0: r14 += r9    A1: r14 += r10   A2: r15 += r9    A3: r15 += r10
//   B0: r16 += r9    U0: r17 += r9    A5: r18 += r9    T: r19 += r9, r10
//   E0: r24 += r9
// Inputs: r20 passes, r21 iterations - 1. Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      mov r14 = r0
      mov r15 = r0
      mov r16 = r0 ;;
    }
    { .mii
      mov r17 = r0
      mov r18 = r0
      mov r19 = r0 ;;
    }
    { .mii
      mov r24 = r0
      adds r9 = 1, r0
      adds r10 = 2, r0 ;;
    }
    { .mii
      cmp.ne p7, p0 = r0, r0
      adds r11 = 4, r0
      nop.i 0 ;;
    }
.Ldriver:
    { .mii
      cmp.eq p6, p0 = r20, r0
      adds r20 = -1, r20
      mov ar.lc = r21 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      (p6) br.cond.dpnt.few .Lend ;;
    }
.Ldriver_a:
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few .La0 ;;
    }
.Ld_after_a:
    { .mii
      nop.m 0
      mov ar.lc = r21
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few .Lb0 ;;
    }
.Ld_after_b:
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few .Lu0 ;;
    }
.Ld_after_u0:
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few .Lt ;;
    }
.Lend:
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }

        .org 0xfe0                      // T: the last two bundles of page 0
.Lt:
    { .mii
      nop.m 0
      add r19 = r19, r9
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r19 = r19, r10
      nop.i 0 ;;
    }
.Le0:                                   // 0x101000: page L, first bundle
    { .mib
      mov r22 = ar.rnat
      add r24 = r24, r9
      br.sptk.few .Ldriver ;;
    }

        .org 0x1800                     // U0: page L
.Lu0:
    { .mib
      nop.m 0
      add r17 = r17, r9
      br.sptk.few .Ld_after_u0 ;;
    }

        .org 0x1e00                     // B: page L
.Lb0:
    { .mii
      nop.m 0
      add r16 = r16, r9
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lb0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few .Ld_after_b ;;
    }

        .org 0x1fd0                     // A: entry page L, non-entry page L + 1
.La0:
    { .mib
      nop.m 0
      add r14 = r14, r9
      (p7) br.cond.dpnt.few .La2 ;;
    }
.La1:
    { .mii
      nop.m 0
      add r14 = r14, r10
      nop.i 0 ;;
    }
.La2:
    { .mii
      nop.m 0
      add r15 = r15, r9
      nop.i 0 ;;
    }
.La3:                                   // 0x102000: page L + 1
    { .mii
      nop.m 0
      add r15 = r15, r10
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .La0 ;;
    }
.La5:
    { .mib
      nop.m 0
      add r18 = r18, r9
      br.sptk.few .Ld_after_a ;;
    }
        .include "ivt.inc"
