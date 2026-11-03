/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Rev.2.3 Vol.2 Single Step vector: MLX ISR.ei=2. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    rsm psr.ic ;;
    srlz.d ;;
    movl r2=0x100200 ;;
    mov cr.iip=r2 ;;
    movl r2=(1 << 13) | (1 << 40) | (1 << 41) ;;
    mov cr.ipsr=r2 ;;
    mov r2=0 ;;
    mov cr.ifs=r2 ;;
    rfi ;;
.org 0x200
    { .mlx
    nop.m 0
    movl r8=0x123456789abcdef ;; }
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
