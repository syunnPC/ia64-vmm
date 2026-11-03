/* SPDX-License-Identifier: GPL-2.0-or-later */
// shladd_xor: a shladd/xor/add loop kernel
//   shladd r16 = r16, 3, r17 ;; xor r16 = r16, r18 ;; add r17 = r17, r16 ; br.cloop
// Inputs from the harness (tests/tier1/programs.c): r8 ar.lc, r2/r3/r4 the
// initial r16/r17/r18. The run ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      mov r16 = r2
      mov ar.lc = r8
      mov r17 = r3 ;;
    }
    { .mii
      mov r18 = r4
      nop.i 0
      nop.i 0 ;;
    }
.Lloop:
    { .mmi
      shladd r16 = r16, 3, r17 ;;
      xor r16 = r16, r18
      nop.i 0 ;;
    }
    { .mib
      add r17 = r17, r16
      nop.i 0
      br.cloop.sptk.few .Lloop ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .include "ivt.inc"
