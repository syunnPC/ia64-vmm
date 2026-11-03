/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.global _start
_start:
  movl r2 = 0x108000
  movl r5 = 0x108002
  mov r3 = 1
  ;;
  mov ar.unat = r3
  ;;
  ld8.fill r40 = [r2]
  ;;
  st8 [r2] = r1, 8
  ;;
  break.i 0x1ffffe
