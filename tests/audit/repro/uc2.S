/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.global _start
_start:
  movl r2 = 0x8000000000108000
  ;;
  xchg8 r8 = [r2], r0
  ;;
  break.i 0x1ffffe
