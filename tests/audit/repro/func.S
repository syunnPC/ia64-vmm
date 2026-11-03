/* SPDX-License-Identifier: GPL-2.0-or-later */
// SDM Vol3 fcmp/fclass: unc type clears both targets even when PR[qp]==0
// EXPECT p6 = 0
// EXPECT p7 = 0
// EXPECT p8 = 0
// EXPECT p9 = 0
.text
.global _start
_start:
  movl r4 = 0x3c0     // p6..p9 = 1, p5 = 0
  ;;
  mov pr = r4, -1
  ;;
  (p5) fcmp.eq.unc p6, p7 = f0, f0
  (p5) fclass.m.unc p8, p9 = f0, @zero
  ;;
  break.i 0x1ffffe
