/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Rev.2.3 Vol.2 §§5.5, 7.1, 8.1: precise debug/trap delivery. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    ssm psr.ic ;;
    srlz.d ;;
    rsm psr.ic ;;
    srlz.d ;;
    movl r3=(1 << 40) | (1 << 13) ;;
    mov cr.ipsr=r3 ;;
    movl r3=.Lstep ;;
    mov cr.iip=r3 ;;
    mov cr.ifs=r0 ;;
    rfi ;;
.Lstep:
    add r4=1,r4 ;;
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
.section .ivt,"ax"
.org 0x6000
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    ssm psr.ic ;;
    srlz.d ;;
    break.m 0x1ffffe ;;
