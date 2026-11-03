/* SPDX-License-Identifier: GPL-2.0-or-later */
// Comparison self-test program for tests/tier1/compare_test.c (not a guest loop). A counted loop with loads, a
// predicated post-increment store, a predicated pure op and an inner
// compare, then one write and one read of the test MMIO device.
// Layout (compare_test.c): image at 0x100000, data array at 0x108000
// (64 x 8 bytes), test device at 0x400000 (tests/tier1/harness.h); break.i
// 0x1ffffe ends the run.
.text
.explicit
.global _start
_start:
    { .mlx
      mov r8 = 0
      movl r14 = 0x108000 ;;
    }
    { .mlx
      mov r15 = 0
      movl r16 = 0x400000 ;;
    }
    { .mii
      nop.m 0
      mov r17 = 63
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      mov ar.lc = r17
      nop.i 0 ;;
    }
.Lloop:
    { .mmi
      ld8 r18 = [r14]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      add r8 = r8, r18
      cmp.ltu p6, p7 = r18, r15 ;;
    }
    { .mmi
      (p6) st8 [r14] = r8, 8
      nop.m 0
      (p7) adds r14 = 8, r14 ;;
    }
    { .mib
      nop.m 0
      adds r15 = 3, r15
      br.cloop.sptk.few .Lloop ;;
    }
    { .mmi
      st8 [r16] = r8
      ld8 r19 = [r16]
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
