/* SPDX-License-Identifier: GPL-2.0-or-later */
// window-tags: one loop whose twelve loads use twelve
// distinct window tags (gva >> w for the hosted w = 40 and the fallback 36:
// VRN 1..7, and offsets 0 and 2^40 in the payload), so one region execution
// cycles more tags than hostwin has slots (8). Virtual mode; the harness
// (tests/tier1/programs.c) maps each address with a D-TC entry and passes
// them in r2..r13, ar.lc in r14. Result: r15 = sum of all loaded words.
// Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      nop.m 0
      mov ar.lc = r14
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
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r3]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r4]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r5]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r6]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r7]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r8]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r9]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r10]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r11]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r12]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
      nop.i 0 ;;
    }
    { .mmi
      ld8 r16 = [r13]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r15 = r15, r16
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
