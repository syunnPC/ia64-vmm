/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0
// EXPECT r11 = 0xffffffffffffffff
// EXPECT r12 = 0
// EXPECT r14 = 1
// EXPECT r15 = 0x8000000000000000
// EXPECT r16 = 0x10
// EXPECT r17 = 2
// EXPECT r19 = 0x6000000080000000
// EXPECT r20 = 0x60000000bfffffff
// EXPECT r21 = 0x10000000c0000000
// EXPECT r22 = 0x1000000000000000
// EXPECT r23 = 0xffffffffffffffff
// EXPECT r24 = 0xffffffffffffffff
// EXPECT r25 = 0x8000000000000001
// EXPECT r26 = 0x00000000000ffff0
// EXPECT r27 = 0x100000000c000000
// EXPECT r28 = 0xffffffffc0000000
// EXPECT r29 = 1
// EXPECT r30 = 0x8000000000000004
// EXPECT r31 = 0x8000000000000000
// EXPECT r32 = 0x7ffffffffffffffe
// EXPECT r33 = 0xffffffffffffe000
// EXPECT r34 = 0x1fffff
// EXPECT r35 = 2
// EXPECT r36 = 3
// EXPECT r37 = 0x7fffffff40000000
// EXPECT r38 = 0x8000000000000001
// EXPECT r39 = 0xc000000000000000
// EXPECT r40 = 0xfffffffffffffff8
// EXPECT r41 = 0x0
// EXPECT r42 = 0xf0
.text
.global _start
_start:
  movl r9 = 0x8000000000000001
  mov r10 = 64
  ;;
  shl r8 = r9, r10
  shr r11 = r9, r10
  shr.u r12 = r9, r10
  mov r13 = 63
  ;;
  shr.u r14 = r9, r13
  shl r15 = r9, r13
  shladd r16 = r9, 4, r0
  shladdp4 r17 = r9, 1, r0
  movl r18 = 0xc0000000
  ;;
  addp4 r19 = r18, r18
  addp4 r20 = -1, r18
  dep r21 = r9, r18, 60, 8
  dep.z r22 = r9, 60, 8
  dep.z r23 = -1, 0, 64
  extr r24 = r9, 63, 16
  extr.u r25 = r9, 0, 64
  dep r26 = -1, r0, 4, 16
  shrp r27 = r9, r18, 4
  sxt4 r28 = r18
  zxt4 r29 = r9
  sub r30 = 5, r9
  and r31 = -2, r9
  andcm r32 = -1, r9
  adds r33 = -8192, r0
  addl r34 = 0x1fffff, r0
  popcnt r35 = r9
  add r36 = r9, r9, 1
  sub r37 = r9, r18, 1
  shrp r38 = r9, r9, 0
  shr r39 = r9, 1
  shr r40 = r9, 60
  dep.z r41 = r9, 0, 0+1
  ;;
  shl r42 = r10, 2   // wait
  break.i 0x1ffffe
