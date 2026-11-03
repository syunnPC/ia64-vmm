/* SPDX-License-Identifier: GPL-2.0-or-later */
// alloc with qp field = p5 (PR[5]=0): SDM requires Illegal Operation (qp != 0 check is outside PR[qp])
// EXPECT r8 = 0
.text
.global _start
_start:
{ .mii
  alloc r8 = ar.pfs, 0, 0, 0, 0
  nop.i 0
  nop.i 0 ;;
}
  break.i 0x1ffffe
