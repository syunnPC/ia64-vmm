/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x1234
// EXPECT mem[0x108000] = 0x1234
// EXPECT r9 = 0
// EXPECT nat9 = 0
// EXPECT r2 = 0x8000000000108008
// EXPECT r10 = 0x8000000000100100
// EXPECT r11 = 0x55
// EXPECT r12 = 3
// EXPECT r13 = 0x8000000000100100
.text
.global _start
_start:
  movl r2 = 0x8000000000108000
  mov r3 = 0x1234
  ;;
  st8 [r2] = r3
  ;;
  ld8 r8 = [r2]
  ;;
  ld8.a r9 = [r2], 8
  movl r4 = 0x8000000000100100
  ;;
  mov b1 = r4
  ;;
  br.sptk b1
  ;;
.org 0x100
aliased:
  mov r10 = ip
  mov ar.lc = 2
  mov r12 = 0
  ;;
cl:
  adds r12 = 1, r12
  br.cloop.sptk cl
  ;;
  br.sptk next
  ;;
next:
  mov r13 = r10
  chk.a.nc r9, rec
back:
  ;;
  break.i 0x1ffffe
  ;;
rec:
  mov r11 = 0x55
  br.sptk back
