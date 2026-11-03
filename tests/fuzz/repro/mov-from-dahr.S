/* SPDX-License-Identifier: GPL-2.0-or-later */
/* mov r1=dahr[r3] (opcode 1, x3=0, x6=0x20) is an Itanium 9500 instruction.
 * SDM Rev.2.3 Vol.3 Table 4-45 marks the cell Reserved if PR[qp] is 1, so
 * Madison and Montecito raise Illegal Operation; the fork reads a DAHR. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    ssm psr.ic ;;
    srlz.d ;;
    mov r4=dahr[r5] ;;
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
