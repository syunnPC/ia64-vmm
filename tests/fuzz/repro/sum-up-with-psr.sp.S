/* SPDX-License-Identifier: GPL-2.0-or-later */
// Reproducer (seed 20264); run: python3 tests/fuzz/fuzz.py --run tests/fuzz/repro/sum-up-with-psr.sp.S
// category: state
// where:    state before bundle #21 at 0x100150
// culprit bundle 0x100140: srlz.d ; sum 0x4
//   psr          interp=0x0000100000102004  fork=0x0000100000102000
// ia64 differential fuzz program, seed 20264
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
    alloc r2=ar.pfs,0,3,7,0 ;;
    movl r32=0x3c ;;
    movl r33=0xff ;;
    movl r34=0xffffffffffffffff ;;
    movl r35=0xf778942242896b05 ;;
    movl r36=0xffffffffffffffd5 ;;
    movl r37=0x2 ;;
    movl r38=0x3cc246f934bfbad9 ;;
    movl r39=0x100 ;;
    movl r40=0xffff ;;
    movl r41=0xe71df231ec8fb1f5 ;;
    mov r15=psr ;;
    movl r2=0x100000 ;;
    xor r15=r15,r2 ;;
    mov psr.l=r15 ;;
    srlz.d ;;
    sum 0x4 ;;
    srlz.d ;;
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
    break.m 0x1ffffe ;;
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
    data8 0x2, 0xffffffffffffffe9, 0x56c8395c4b550ef8, 0x20
    data8 0x2, 0x3, 0x10, 0x7f
    data8 0xff00bf00ff00ff00, 0x8000, 0x3bc0703c5a7f6297, 0xaaaeaaaaaaaaaaaa
    data8 0x1f, 0x1f, 0x8, 0x467c08a8e897f96f
    data8 0x400000000020, 0x2d78300561d5c7e7, 0x8000800080008000, 0x5555555555555555
    data8 0x8c31e55b2e5fb59f, 0x1f, 0x1b, 0x20
    data8 0x452272cf7a2c190d, 0xf, 0x439835ca6583383e, 0x80a30908a9802c8a
    data8 0x3, 0x123456789abcdef, 0x591ba7c3facfc88a, 0x3a
    data8 0x5555555555555555, 0xffffffffffffffef, 0x101010105010101, 0x35075aca4eaf21e7
    data8 0xff00ff00ff00ff00, 0x5555555555555555, 0x80, 0x85537183ca75deda
    data8 0x6ed2e31b5354a9dd, 0x593b44c7ef03a7d3, 0x5555555555555555, 0xfffffffffffffff4
    data8 0xc06301145fd8ee94, 0x9d75f10c6c8fddc8, 0x40, 0x8
    data8 0x5ff2524e76d67c80, 0x8000800080008000, 0x8000000080004000, 0x3
    data8 0xffffffffffffffdf, 0x4de56b066072c6a6, 0x9b3839b49257ce18, 0x8080808080808080
    data8 0xff00ff00ff00ff00, 0x7fff, 0xffff0000ffff0008, 0x7fffffffffffffff
    data8 0x7fff7fff7fff7fff, 0x7f, 0xe924135b597d3f55, 0xffff8000
