/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 2
// EXPECT r9 = 1
// EXPECT r10 = 0x77
.text
.global _start
_start:
  br.sptk fwd
  ;;
back:
  mov r9 = 1
  ;;
{ .mlx
  nop.m 0
  brl.sptk far2
}
  ;;
.org 0x100
fwd:
  mov r8 = 2
  ;;
{ .mlx
  nop.m 0
  brl.sptk back
}
.org 0x6000
rec_back:
  mov r10 = 0x77
  br.sptk cont
  ;;
.org 0x8000
far2:
  movl r2 = 0x108000
  ;;
  ld8.a r3 = [r2]
  ;;
  chk.a.nc r3, rec_back
cont:
  ;;
  break.i 0x1ffffe
  ;;
