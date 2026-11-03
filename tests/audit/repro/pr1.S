/* SPDX-License-Identifier: GPL-2.0-or-later */
// After 1 ctop rotation (rrb.pr = 47): logical p17 == physical p16.
// EXPECT r20 = 0x0000000000040001
// EXPECT p17 = 1
// EXPECT p16 = 1
// EXPECT r21 = 0xffffffffffff0001
// EXPECT r22 = 0x1
.text
.global _start
_start:
  mov ar.lc = 1
  mov ar.ec = 1
  ;;
  mov pr.rot = 0
  ;;
lp:
  nop.m 0
  nop.i 0
  br.ctop.sptk out
  ;;
out:
  // rrb.pr is now 47; pr.rot is physical: physical p18 set
  mov pr.rot = 0x40000
  ;;
  mov r20 = pr
  ;;
  // logical p17 = physical (17-16+47)%48+16 = 64? -> (1+47)%48=0 -> physical 16 -> p63 written by ctop=1
  // write all predicates 1 via mov pr=r,-1 then read
  mov r9 = -1
  ;;
  mov pr = r9, -1
  ;;
  mov r21 = pr
  mov pr = r0, -1
  ;;
  mov r22 = pr
  ;;
  mov pr = r9, 0x1ffff - 0x10000 + 0  // static only 
  ;;
  break.i 0x1ffffe
