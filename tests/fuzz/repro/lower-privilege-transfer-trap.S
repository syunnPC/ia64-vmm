/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Rev.2.3 Vol.2 §§5.5, 7.1 and 8.1: concurrent LP/TB. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    ssm psr.ic ;;
    srlz.d ;;
    movl r2=3 << 62 ;;
    mov ar.pfs=r2 ;;
    movl r2=0x100200 ;;
    mov b0=r2 ;;
    mov r2=psr ;;
    movl r3=(1 << 25) | (1 << 26) ;;
    or r2=r2,r3 ;;
    mov psr.l=r2 ;;
    srlz.d ;;
    br.ret.sptk.many b0 ;;
.org 0x200
    nop.m 0 ;;
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
.section .ivt,"ax"
.org 0x5e00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    ssm psr.ic ;;
    srlz.d ;;
    break.m 0x1ffffe ;;
