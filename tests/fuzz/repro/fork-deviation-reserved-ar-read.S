/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AR15 is reserved: the fork raises Illegal Operation (SDM Vol.3 mov ar);
 * the interpreter models reserved ARs as per-vCPU storage. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    ssm psr.ic ;;
    srlz.d ;;
    mov r4=ar15 ;;
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
