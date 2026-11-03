/* SPDX-License-Identifier: GPL-2.0-or-later */
// Flat-image entry of the compiled-C suite (tests/bench/csuite/csuite.c),
// laid out like the guest programs: entry at the image start (GPA 0x100000
// in the harness), the guest IVT at image offset 0x8000, C code after.
// Inputs (harness: tests/bench/jobdump.c setup): r2 kernel, r3 iterations,
// r12 stack pointer, ar.bsp/ar.bspstore the backing store. gp is derived
// from the entry IP (csuite.lds: gp_offset = __gp - _start), so the image
// runs at any address with no relocation (all C code is -mauto-pic). The
// result is r8 (checksum), r9 = 0; the run ends with `break.i 0x1ffffe`.
        .text
        .explicit
        .global _start
_start:
    { .mlx
      nop.m 0
      movl r14 = gp_offset ;;
    }
1:  { .mii
      alloc r15 = ar.pfs, 0, 0, 2, 0
      mov r16 = ip
      mov out0 = r2 ;;
    }
    { .mii
      mov out1 = r3
      adds r16 = _start - 1b, r16 ;;
      add r1 = r16, r14 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = csuite_main ;;
    }
    { .mii
      nop.m 0
      mov r9 = r0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .include "ivt.inc"
