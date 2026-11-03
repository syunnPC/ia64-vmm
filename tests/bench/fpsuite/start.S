/* SPDX-License-Identifier: GPL-2.0-or-later */
// Flat-image entry of the floating-point suite (tests/bench/fpsuite/fpsuite.c),
// laid out as tests/bench/csuite/start.S: entry at the image start (GPA
// 0x100000 in the harness), the test IVT at image offset 0x8000, C code after.
// Inputs (harness: tests/bench/jobdump.c setup): r2 kernel, r3 iterations,
// r12 stack pointer, ar.bsp/ar.bspstore the backing store. gp is derived
// from the entry IP (csuite.lds: gp_offset = __gp - _start), so the image
// runs at any address with no relocation (all C code is -mauto-pic). ar.fpsr
// is set to the Linux default (traps disabled; sf0 pc 64-bit, round to
// nearest; sf1 wre, td; sf2 and sf3 td). The result is r8 (checksum), r9 = 0;
// the run ends with `break.i 0x1ffffe`.
        .text
        .explicit
        .global _start
_start:
    { .mlx
      nop.m 0
      movl r14 = gp_offset ;;
    }
    { .mlx
      nop.m 0
      movl r17 = 0x0009804c0270033f ;;
    }
1:  { .mii
      alloc r15 = ar.pfs, 0, 0, 2, 0
      mov r16 = ip
      mov out0 = r2 ;;
    }
    { .mii
      mov out1 = r3
      adds r16 = _start - 1b, r16
      nop.i 0 ;;
    }
    { .mii
      mov ar.fpsr = r17
      add r1 = r16, r14
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = fpsuite_main ;;
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
