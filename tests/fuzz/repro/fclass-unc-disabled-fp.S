/* SPDX-License-Identifier: GPL-2.0-or-later */
// Reproducer (seed 85); run: python3 tests/fuzz/fuzz.py --run tests/fuzz/repro/fclass-unc-disabled-fp.S
// category: state
// where:    state before bundle #15 at 0x11d500
// culprit bundle 0x1000e0: (p37) fclass.m.unc p37,p32=f9,0x1a1
//   pr           interp=0x90c72a6161cb52f3  fork=0x90c72a4061cb52f3
// ia64 differential fuzz program, seed 85
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
    alloc r2=ar.pfs,0,3,1,0 ;;
    movl r32=0x100000000 ;;
    movl r33=0x20 ;;
    movl r34=0xfffffffffffffff9 ;;
    movl r35=0x99ee7e6a9604cdf9 ;;
    movl r2=0x90c72a6161cb52f3 ;;
    mov pr=r2,-1 ;;
    (p50) ssm 0x40000 ;;
    srlz.d ;;
    (p37) fclass.m.unc p37,p32=f9,0x1a1 ;;
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
    break.m 0x1ffffe ;;
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
    data8 0x3, 0xce2d2369733965c, 0xaf463a9c9c25dc43, 0x1a
    data8 0x80000000, 0x7fffffff7fffffff, 0xe93136f3af0deecb, 0xff
    data8 0xffff0000ffff0000, 0xf, 0xf1fc4ec34585e38a, 0x7fff
    data8 0x315dfc6df2d68cab, 0xff00ff00ff00ff00, 0xf, 0x2000000003
    data8 0x78e7582c2a3f9a2d, 0x9a67de601704e777, 0x3, 0x80000000
    data8 0x1277aa901da6f6b0, 0xffffffffffffffdc, 0x7f, 0x597bc544dcebb027
    data8 0x7, 0xbc97e6ef5d7f1c3a, 0x4ce0801038c8f1b2, 0x2198bd250a82f447
    data8 0x3b, 0x1d68d999b74ddf0e, 0xef00ff00ff00ff, 0xeba6eb3cad9bb2dc
    data8 0x1fa28450ad2bb987, 0x40100, 0x7fffffff7fffffff, 0x47fffffff
    data8 0x602cdff93198c76f, 0x7f, 0xf, 0x4931a93cb13b226f
    data8 0x32, 0x7fffffff7fffffff, 0x101010101010101, 0x7
    data8 0x1f, 0x180, 0x2940a2e4cf035b45, 0x83a18b2576aeaa79
    data8 0x101010101010100, 0xffffffffffffffed, 0x7, 0x1d
    data8 0x7fff7fff7fff7fff, 0x3, 0xac20ad950ae989c7, 0x8000000080000000
    data8 0x8000000080000000, 0x8, 0x100000000, 0xffffffffffffffd9
    data8 0xf, 0x7f, 0xb5c84a8ffb486b40, 0xad6ac1565057940f
