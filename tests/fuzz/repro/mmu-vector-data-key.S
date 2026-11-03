/* SPDX-License-Identifier: GPL-2.0-or-later */
// MMU interruption-vector fixture; SDM Rev.2.3 Vol.2 Table 5-7.
// ia64 differential fuzz program, seed 0
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    rsm psr.ic ;;
    srlz.d ;;
    mov r2=0 ;;
    mov r3=48 ;;
    mov rr[r2]=r3 ;;
    srlz.d ;;
    mov cr.ifa=r2 ;;
    mov r3=112 ;;
    mov cr.itir=r3 ;;
    mov r3=2017 ;;
    itr.i itr[r2]=r3 ;;
    itr.d dtr[r2]=r3 ;;
    srlz.i ;;
    srlz.d ;;
    mov r3=1 ;;
    mov pkr[r2]=r3 ;;
    srlz.d ;;
    movl r2=0x2000000000400000 ;;
    mov r3=304 ;;
    mov rr[r2]=r3 ;;
    srlz.d ;;
    mov cr.ifa=r2 ;;
    mov r3=16944 ;;
    mov cr.itir=r3 ;;
    movl r3=0x1107e1 ;;
    itc.d r3 ;;
    srlz.d ;;
    movl r3=0x100002a000 ;;
    mov cr.ipsr=r3 ;;
    movl r3=.Lvirtual ;;
    mov cr.iip=r3 ;;
    mov cr.ifs=r0 ;;
    rfi ;;
    .align 16
.Lvirtual:
    ld8 r4=[r2] ;;
fuzz_checkpoint:
    break.m 0x1ffffe ;;
.include "tests/fuzz/repro/mmu-vector-ivt.inc"
