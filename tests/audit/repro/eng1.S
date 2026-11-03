/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x123456789abcdef0
// EXPECT r9 = 3
// EXPECT r10 = 1
// EXPECT r11 = 0x100020
// EXPECT r12 = 7
// EXPECT r13 = 0
// EXPECT r14 = 0x55
// EXPECT r15 = 2
// EXPECT r16 = 9
// EXPECT r17 = 0x42
.text
.global _start
_start:
{ .mlx
  mov r9 = 3
  movl r8 = 0x123456789abcdef0
}
{ .mlx
  nop.m 0
  brl.sptk target
}
  mov r9 = 99
  ;;
target:
{ .mii
  mov r10 = 1
  mov r11 = ip
  nop.i 0
}
  // br.cloop counting
  mov ar.lc = 6
  mov r12 = 0
  ;;
cl:
  add r12 = 1, r12
  br.cloop.sptk cl
  ;;
  // br.wtop: run while p16 true; uses rotation, need sor
  mov ar.ec = 1
  mov r13 = 0
  ;;
  // chk.s with no NaT: fallthrough
  chk.s r12, recov
  mov r14 = 0x55
  ;;
  // ld.a + chk.a must branch to recovery (zero ALAT)
  movl r2 = 0x108000
  mov r3 = 2
  ;;
  st8 [r2] = r3
  ;;
  ld8.a r15 = [r2]
  ;;
  chk.a.clr r15, recov2
back2:
  mov r16 = 9
  ;;
  break.i 0x1ffffe
recov:
  mov r14 = 0x66
  br.sptk back2
recov2:
  ld8 r15 = [r2]
  mov r17 = 0x42
  br.sptk back2
