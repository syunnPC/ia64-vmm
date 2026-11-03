/* SPDX-License-Identifier: GPL-2.0-or-later */
// Reproducer (seed 580); run: python3 tests/fuzz/fuzz.py --run tests/fuzz/repro/mov-cr.ifs-frame-marker.S
// category: control
// where:    bundle #35: interp ip 0x11d400, fork ip 0x1001d0
// culprit bundle 0x1001c0: (p28) mov cr.ifs=r18 ; srlz.d
// ia64 differential fuzz program, seed 580
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    ssm 0x2000 ;;
    srlz.d ;;
    bsw.1 ;;
    mov ar.rsc=0 ;;
    movl r2=0x120000 ;;
    mov ar.bspstore=r2 ;;
    alloc r2=ar.pfs,0,8,8,0 ;;
    movl r32=0x4b408c63a5443051 ;;
    movl r33=0x8000001000000001 ;;
    movl r34=0x7f ;;
    movl r35=0x436fc87d30640747 ;;
    movl r36=0xbd1b6b24f94691d0 ;;
    movl r37=0xffffffff ;;
    movl r38=0xff00ff00ff00ff ;;
    movl r39=0x7fff7fff7fff7fff ;;
    movl r40=0x100000000 ;;
    movl r41=0x5a51ebe06015bda4 ;;
    movl r42=0x94b2a32b1d121ee7 ;;
    movl r43=0xebb376388dc74ab3 ;;
    movl r44=0x40 ;;
    movl r45=0xffffffffffffffff ;;
    movl r46=0x7a587857b4640a6d ;;
    movl r47=0xffff8000 ;;
    movl r2=0x60c3cc44e9c2f6fb ;;
    mov pr=r2,-1 ;;
    mov ar.lc=1 ;;
    mov ar.ec=1 ;;
.Lf119:
    rsm 0x2000 ;;
    srlz.d ;;
    movl r18=0x800000260e1947c1 ;;
    (p28) mov cr.ifs=r18 ;;
    srlz.d ;;
    mov r28=cr.ifs ;;
    ssm 0x2000 ;;
    srlz.d ;;
    br.ctop.sptk.few .Lf119 ;;
fuzz_checkpoint:
    flushrs ;;
    mov r1=ar.bsp ;;
    mov r2=ar.bspstore ;;
    extr.u r4=r2,3,6 ;;
    mov r5=1 ;;
    shl r5=r5,r4 ;;
    adds r5=-1,r5 ;;
    .align 16
    { .mmi
    mov r9=ar.rnat ;;
    and r9=r9,r5
    nop.i 0 ;; }
    movl r31=0x110000 ;;
    ld8 r10=[r31],8 ;;
    ld8 r11=[r31],8 ;;
    ld8 r12=[r31],8 ;;
    ld8 r13=[r31],8 ;;
    ld8 r14=[r31],8 ;;
    ld8 r15=[r31],8 ;;
    ld8 r16=[r31],8 ;;
    ld8 r17=[r31],8 ;;
    ld8 r18=[r31],8 ;;
    ld8 r19=[r31],8 ;;
    ld8 r20=[r31],8 ;;
    ld8 r21=[r31],8 ;;
    ld8 r22=[r31],8 ;;
    ld8 r23=[r31],8 ;;
    ld8 r10=[r31],8 ;;
    ld8 r11=[r31],8 ;;
    ld8 r12=[r31],8 ;;
    ld8 r13=[r31],8 ;;
    ld8 r14=[r31],8 ;;
    ld8 r15=[r31],8 ;;
    ld8 r16=[r31],8 ;;
    ld8 r17=[r31],8 ;;
    ld8 r18=[r31],8 ;;
    ld8 r19=[r31],8 ;;
    ld8 r20=[r31],8 ;;
    ld8 r21=[r31],8 ;;
    ld8 r22=[r31],8 ;;
    ld8 r23=[r31],8 ;;
    ld8 r10=[r31],8 ;;
    ld8 r11=[r31],8 ;;
    ld8 r12=[r31],8 ;;
    ld8 r13=[r31],8 ;;
    ld8 r14=[r31],8 ;;
    ld8 r15=[r31],8 ;;
    ld8 r16=[r31],8 ;;
    ld8 r17=[r31],8 ;;
    ld8 r18=[r31],8 ;;
    ld8 r19=[r31],8 ;;
    ld8 r20=[r31],8 ;;
    ld8 r21=[r31],8 ;;
    ld8 r22=[r31],8 ;;
    ld8 r23=[r31],8 ;;
    ld8 r10=[r31],8 ;;
    ld8 r11=[r31],8 ;;
    ld8 r12=[r31],8 ;;
    ld8 r13=[r31],8 ;;
    ld8 r14=[r31],8 ;;
    ld8 r15=[r31],8 ;;
    ld8 r16=[r31],8 ;;
    ld8 r17=[r31],8 ;;
    ld8 r18=[r31],8 ;;
    ld8 r19=[r31],8 ;;
    ld8 r20=[r31],8 ;;
    ld8 r21=[r31],8 ;;
    ld8 r22=[r31],8 ;;
    ld8 r23=[r31],8 ;;
    movl r29=0x110200 ;;
    ld8 r29=[r29] ;;
    cmp.eq p6,p0=0,r29 ;;
    (p6) movl r29=0x120000 ;;
    movl r28=0x500 ;;
    sub r28=r1,r28 ;;
    cmp.ltu p6,p0=r29,r28 ;;
    (p6) mov r29=r28 ;;
    sub r28=r1,r29 ;;
    shr.u r28=r28,3 ;;
    cmp.eq p6,p0=0,r28 ;;
    (p6) br.cond.sptk.few .Lepi_done ;;
    adds r28=-1,r28 ;;
    mov ar.lc=r28 ;;
.Lepi_loop:
    dep r26=-1,r29,3,6
    extr.u r25=r29,3,6 ;;
    cmp.ltu p7,p8=r26,r2 ;;
    (p7) ld8 r24=[r26]
    (p8) mov r24=r9 ;;
    shr.u r24=r24,r25 ;;
    tbit.nz p9,p10=r24,0 ;;
    (p9) mov r27=0 ;;
    (p10) ld8 r27=[r29] ;;
    adds r29=8,r29
    br.cloop.sptk.few .Lepi_loop ;;
.Lepi_done:
fuzz_end:
    break.m 0x1ffffe ;;
.section .ivt,"ax"
.org 0x0
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x400
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x800
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0xc00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x1000
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x1400
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x1800
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x1c00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x2000
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x2400
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x2800
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x2c00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x3000
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x3400
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x3800
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x3c00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x4000
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x4400
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x4800
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x4c00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5000
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5100
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5200
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5300
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5400
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    mov r16=cr.ipsr ;;
    mov r17=cr.iip ;;
    extr.u r18=r16,41,2 ;;
    adds r19=1,r18 ;;
    shr.u r20=r19,1 ;;
    and r20=r20,r19 ;;
    shl r21=r20,4 ;;
    add r17=r17,r21 ;;
    shladd r22=r20,1,r20 ;;
    sub r19=r19,r22 ;;
    dep r16=r19,r16,41,2 ;;
    mov cr.ipsr=r16 ;;
    mov cr.iip=r17 ;;
    srlz.d ;;
    rfi ;;
.org 0x5500
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    mov r16=cr.ipsr ;;
    mov r17=cr.iip ;;
    extr.u r18=r16,41,2 ;;
    adds r19=1,r18 ;;
    shr.u r20=r19,1 ;;
    and r20=r20,r19 ;;
    shl r21=r20,4 ;;
    add r17=r17,r21 ;;
    shladd r22=r20,1,r20 ;;
    sub r19=r19,r22 ;;
    dep r16=r19,r16,41,2 ;;
    mov cr.ipsr=r16 ;;
    mov cr.iip=r17 ;;
    srlz.d ;;
    rfi ;;
.org 0x5600
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    mov r16=cr.ipsr ;;
    mov r17=cr.iip ;;
    extr.u r18=r16,41,2 ;;
    adds r19=1,r18 ;;
    shr.u r20=r19,1 ;;
    and r20=r20,r19 ;;
    shl r21=r20,4 ;;
    add r17=r17,r21 ;;
    shladd r22=r20,1,r20 ;;
    sub r19=r19,r22 ;;
    dep r16=r19,r16,41,2 ;;
    mov cr.ipsr=r16 ;;
    mov cr.iip=r17 ;;
    srlz.d ;;
    rfi ;;
.org 0x5700
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5800
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5900
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5a00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5b00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5c00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5d00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5e00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x5f00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6000
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6100
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6200
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6300
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6400
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6500
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6600
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6700
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6800
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6900
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6a00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6b00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6c00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6d00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6e00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x6f00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7000
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7100
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7200
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7300
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7400
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7500
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7600
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7700
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7800
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7900
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7a00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7b00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7c00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7d00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7e00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.org 0x7f00
    { .mii
    nop.m 0
    nop.i 0
    nop.i 0 ;; }
    break.m 0x1ffffe ;;
.data
fuzz_scratch:  // linked at SCRATCH
    data8 0x8080808280808080, 0x2000ffffffff, 0x6604e101008f784d, 0x8000020
    data8 0xb4c28876b7374da1, 0x40, 0x282ac77e06bf1e95, 0xffffffff80000000
    data8 0x20, 0x7fff, 0x8000000000000001, 0x8000000000000001
    data8 0x80, 0xffffffffffffffd7, 0xfffffffffffffffa, 0x8
    data8 0x1, 0x8, 0x7ffffffffbffffff, 0x100000000
    data8 0x29dff9b60d10a98d, 0x1a, 0xaae3a447d32bc53a, 0x57e8cfa1f80fab15
    data8 0xaadacba6a9b8483b, 0x100000000, 0xffffffff, 0x69851d720881bee8
    data8 0x2000007fffffff, 0x3d, 0x1, 0xffffffffffffffd8
    data8 0xffffffffffffffcd, 0x1, 0x30, 0xffffffffffffffff
    data8 0xd13fe6211be5fa24, 0xffffffffffffffff, 0xffff0000ffff0004, 0x7fffffff
    data8 0xb24a3da53a942a1d, 0xfffffffffffffff3, 0x0, 0x101010101010301
    data8 0xaaaaaaaaaaaaaaaa, 0x10, 0x3f, 0x8000
    data8 0xc31a5408834ba553, 0x3b73007e7ff751c0, 0x514879dd2afdc4ff, 0xe93d656f33aff4b
    data8 0x3, 0xff00ff00ff00ff, 0xe6bfe01a1057810e, 0xffff8000
    data8 0xffffffffffffffff, 0x17f, 0x14, 0x77ca8b561ada8d39
    data8 0xffffffffffffffe5, 0xe32bb19b987486e5, 0x40, 0x152440de754fa870
