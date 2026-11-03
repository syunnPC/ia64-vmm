/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 1
// EXPECT r9 = 0x12345
// EXPECT r10 = 0x20000000000
// EXPECT r11 = 0x100100
// EXPECT r12 = 0x20000002000
// EXPECT r13 = 0x100100
// EXPECT r14 = 0x8000000000000000
.text
.global _start
_start:
    movl r2=0x100000
    ;;
    mov cr.iva=r2
    ssm psr.ic
    ;;
    srlz.i
    ;;
    br.cond.sptk .test
.org 0x100
.explicit
.test:
    { .mii
      nop.m 0
      break.i 0x12345
      adds r8=1,r0
      ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0
      ;;
    }
.auto
.org 0x2c00
.break_handler:
    mov r9=cr.iim
    mov r10=cr.isr
    mov r11=cr.iip
    mov r12=cr.ipsr
    mov r13=cr.iipa
    cover
    ;;
    mov r14=cr.ifs
    movl r2=0x20000000000
    ;;
    add r2=r12,r2
    ;;
    mov cr.ipsr=r2
    ;;
    rfi
