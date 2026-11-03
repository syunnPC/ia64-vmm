/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.3 itc/itr: executed with PSR.ic=1 they raise Illegal Operation
 * (checked before the privilege level) and insert nothing. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    movl r3=0x600000 ;;
    mov cr.ifa=r3 ;;
    mov r4=48 ;;
    mov cr.itir=r4 ;;
    movl r5=0x300661 ;;
    ssm 0x2000 ;;
    srlz.d ;;
    itc.d r5 ;;
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
.section .ivt,"ax"
.org 0x5400
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    ssm psr.ic ;;
    srlz.d ;;
    break.m 0x1ffffe ;;
