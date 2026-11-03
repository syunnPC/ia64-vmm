/* SPDX-License-Identifier: GPL-2.0-or-later */
// pfm = {sof=16, sol=0, sor=1 (8 regs)} is a frame alloc can create (alloc r=ar.pfs,0,0,16,8)
// EXPECT ar64 = 0x4010
.text
.global _start
_start:
  movl r9 = 0x4010
  ;;
  mov ar.pfs = r9
  ;;
  break.i 0x1ffffe
