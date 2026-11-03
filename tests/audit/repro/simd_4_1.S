/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xffffff0000ff   pack2.uss R = r2, r3
// EXPECT r9 = 0x80007fff7fff7f01   pack4.sss R = r2, r3
// EXPECT r10 = 0x1   czx1.l R = r5
// EXPECT r11 = 0x0   czx1.r R = r5
// EXPECT r12 = 0x2   czx2.l R = r5
// EXPECT r13 = 0x1   czx2.r R = r5
.text
.global _start
_start:
  movl r2 = 0x7fffffff00007f01
  movl r3 = 0x80d89fff01802cfe
  movl r4 = 0x0
  movl r5 = 0x7f00ff0000007f00
  ;;
  pack2.uss r8 = r2, r3
  pack4.sss r9 = r2, r3
  czx1.l r10 = r5
  czx1.r r11 = r5
  czx2.l r12 = r5
  czx2.r r13 = r5
  ;;
  break.i 0x1ffffe
