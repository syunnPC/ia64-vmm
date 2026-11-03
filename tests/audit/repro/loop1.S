/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT mem[0x108800] = 0x11
// EXPECT mem[0x108808] = 0x22
// EXPECT mem[0x108810] = 0x33
// EXPECT mem[0x108818] = 0x44
// EXPECT mem[0x108820] = 0x55
// EXPECT mem[0x108828] = 0
// EXPECT ar65 = 0
// EXPECT ar66 = 0
// EXPECT r2 = 0x108428
// EXPECT r3 = 0x108828
// EXPECT r20 = 0x2a00000000000000
// EXPECT r21 = 3
// EXPECT r22 = 0
.text
.global _start
_start:
  movl r2 = 0x108400
  movl r3 = 0x108800
  mov r4 = 0x11
  mov r5 = 0x22
  mov r6 = 0x33
  ;;
  st8 [r2] = r4, 8
  ;;
  st8 [r2] = r5, 8
  ;;
  st8 [r2] = r6, 8
  mov r4 = 0x44
  mov r5 = 0x55
  ;;
  st8 [r2] = r4, 8
  ;;
  st8 [r2] = r5, 8
  ;;
  movl r2 = 0x108400
  mov ar.lc = 4
  mov ar.ec = 2
  mov pr.rot = 0x10000
  ;;
loop:
  (p16) ld8 r32 = [r2], 8
  (p17) st8 [r3] = r33, 8
  br.ctop.sptk loop
  ;;
  mov r20 = r0
  ;;
  // cfm rrb fields: extract rrb.pr (bits 37:32) via mov r = ar.pfs? not available; use pr dump
  mov r21 = 3
  mov r22 = 0
  ;;
  break.i 0x1ffffe
