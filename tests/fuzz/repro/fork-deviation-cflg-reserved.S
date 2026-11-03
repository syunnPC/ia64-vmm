/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fork deviation: SDM Vol.2 §10.3.3.1 CFLG bits63:43 are reserved. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    ssm psr.ic ;;
    srlz.d ;;
    movl r2=1 << 43 ;;
    mov ar.cflg=r2 ;;
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
