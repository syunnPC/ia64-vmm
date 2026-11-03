/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xff008000ff00ff   pack2.uss R = r2, r3
// EXPECT r9 = 0x8000800080008000   pack4.sss R = r2, r3
// EXPECT r10 = 0x1   czx1.l R = r5
// EXPECT r11 = 0x0   czx1.r R = r5
// EXPECT r12 = 0x4   czx2.l R = r5
// EXPECT r13 = 0x4   czx2.r R = r5
.text
.global _start
_start:
  movl r2 = 0xff0101018bfe7fff
  movl r3 = 0x9fff01fffeb80080
  movl r4 = 0x3f
  movl r5 = 0xff00010000fe7f00
  ;;
  pack2.uss r8 = r2, r3
  pack4.sss r9 = r2, r3
  czx1.l r10 = r5
  czx1.r r11 = r5
  czx2.l r12 = r5
  czx2.r r13 = r5
  ;;
  break.i 0x1ffffe
