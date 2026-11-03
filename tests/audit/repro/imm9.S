/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r2 = 0x107f00
// EXPECT r3 = 0x1080ff
// EXPECT r4 = 0x107f00
// EXPECT r5 = 0x108100
// EXPECT r6 = 0xffffffffffff0000
// EXPECT r20 = 0x7f
// EXPECT r21 = 0xffffffffffffff80
.text
.global _start
_start:
  movl r2 = 0x108000
  movl r3 = 0x108000
  movl r4 = 0x108000
  movl r5 = 0x108000
  ;;
  ld8 r8 = [r2], -256
  ld1 r9 = [r3], 255
  st8 [r4] = r0, -256
  st1 [r5] = r0, 255
  ;;
  adds r5 = 1, r5
  mov pr.rot = -0x10000
  ;;
  mov r6 = pr
  mov ar.ccv = 127
  ;;
  mov r20 = ar.ccv
  mov ar.ccv = -128
  ;;
  mov r21 = ar.ccv
  ;;
  break.i 0x1ffffe
