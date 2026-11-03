/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.global _start
_start:
  movl r2 = 0x108000
  mov r9 = -1
  mov r11 = 0
  ;;
loop:
  ld8 r8 = [r2]
  adds r11 = 1, r11
  ;;
  cmp.eq p6, p0 = r8, r0
  cmp.eq p7, p0 = r8, r9
  ;;
  (p6) br.cond.sptk loop
  (p7) br.cond.sptk loop
  ;;
  break.i 0x1ffffe
