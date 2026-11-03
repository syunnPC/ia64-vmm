/* SPDX-License-Identifier: GPL-2.0-or-later */
// A deferred Key Miss does not preclude a lower-priority Access Bit
// fault (SDM Rev.2.3 Vol.2 Tables 5-3 and 5-4). The code page has ITLB.ed = 1
// and DCR.dk = 1, DCR.da = 0, so ld8.s raises Data Access Bit (0x2800).
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
    movl r3=0x100000000007e1 ;;
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
    movl r3=0x1107c1 ;;
    itc.d r3 ;;
    srlz.d ;;
    movl r3=0x700 ;;
    mov cr.dcr=r3 ;;
    srlz.d ;;
    movl r3=0x100002a000 ;;
    mov cr.ipsr=r3 ;;
    movl r3=.Lvirtual ;;
    mov cr.iip=r3 ;;
    mov cr.ifs=r0 ;;
    rfi ;;
    .align 16
.Lvirtual:
    ld8.s r4=[r2] ;;
fuzz_checkpoint:
    break.m 0x1ffffe ;;
.include "tests/fuzz/repro/mmu-vector-ivt.inc"
