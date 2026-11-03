/* SPDX-License-Identifier: GPL-2.0-or-later */
// Region chaining (suite chain): loop A on code page 0
// leaves through its exit branch to loop B on page 1, so a Tier1 region at
// A's header ends at an EXIT point whose target is the header of the region
// at B. Each pass starts with an ar.itc read, outside the Tier1 subset,
// which keeps the outer loop out of the regions.
//   A  r18 = r8 iterations; with p6 (r9 == r10, the pass counting down)
//      every iteration stores r3 to [r2] (a code store into B's first
//      bundle in the coherence tests);
//   B  r11 iterations; reads r20 (the NaT tests set its NaT bit) and adds 3
//      to r15 in slot 2 of its first bundle; .Lalt is that bundle with
//      `adds r15 = 5, r15`, never executed.
// Inputs: r2, r3, r8 (>= 1), r9 passes (>= 1), r10, r11 (>= 1), r20.
// Results: r14, r15, r16. Ends with `break.i 0x1ffffe`.
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
      nop.i 0
      nop.i 0 ;;
    }
.Lpass:
    { .mii
      mov r30 = ar.itc
      mov r18 = r8
      cmp.eq p6, p0 = r9, r10 ;;
    }
.La:
    { .mmi
(p6)  st8 [r2] = r3
      nop.m 0
      adds r16 = 1, r16 ;;
    }
    { .mii
      nop.m 0
      adds r18 = -1, r18 ;;
      cmp.eq p7, p0 = r0, r18 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p7)  br.cond.dpnt.few .Lb ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few .La ;;
    }
        .org 0x1000
.Lb:
    { .mii
      add r14 = r14, r20
      adds r17 = 1, r17
      adds r15 = 3, r15 ;;
    }
    { .mii
      nop.m 0
      cmp.ne p7, p0 = r17, r11
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p7)  br.cond.dptk.few .Lb ;;
    }
    { .mii
      mov r17 = r0
      adds r9 = -1, r9 ;;
      cmp.eq p8, p0 = r0, r9 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p8)  br.cond.dpnt.few .Ldone ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few .Lpass ;;
    }
.Ldone:
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
.Lalt:
    { .mii
      add r14 = r14, r20
      adds r17 = 1, r17
      adds r15 = 5, r15 ;;
    }
        .include "ivt.inc"
