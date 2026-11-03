/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 5
// EXPECT r9 = 0
// EXPECT r10 = 7
.text
.global _start
_start:
{ .mlx
  mov r8 = 5
  nop.x 0x123456789
}
{ .mlx
  cmp.eq p6, p7 = r0, r8
  (p6) movl r9 = 0x1111
  ;;
}
{ .mlx
  mov r10 = 7
  break.x 0x1ffffe
}
