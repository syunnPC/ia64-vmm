/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x00000000002dc6c0
// EXPECT r9 = 0x00000000005b8d80
.text
.explicit
.global _start
_start:
    { .mii
      mov r8=0
      mov r9=0
      nop.i 0 ;;
    }
    { .mlx
      nop.m 0
      movl r20=2999999 ;;
    }
    { .mii
      nop.m 0
      mov ar.lc=r20
      nop.i 0 ;;
    }
.loop:
    { .mii
      adds r8=1,r8
      adds r9=2,r9
      nop.i 0
      ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk .loop
      ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
