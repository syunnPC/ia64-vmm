/* SPDX-License-Identifier: GPL-2.0-or-later */
// Reproducer (seed 70177); run: python3 tests/fuzz/fuzz.py --run tests/fuzz/repro/fsub-extended-range-operand.S
// category: state
// where:    state before bundle #33 at 0x1001c0
// culprit bundle 0x1001b0: (p01) fsub.s3 f121=f21,f16
//   ar.fpsr      interp=0x0009804c0270033f  fork=0x0299804c0270033f
//   f121         interp=0x3ffff8000000000000000  fork=0x3ffffc000000000000000
// ia64 differential fuzz program, seed 70177
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
    alloc r2=ar.pfs,0,11,0,0 ;;
    movl r32=0x8000 ;;
    movl r33=0x7076d0db047c2192 ;;
    movl r34=0xffffffffffffffd1 ;;
    movl r35=0x80 ;;
    movl r36=0x39 ;;
    movl r37=0xbbeacfe6e15d73f0 ;;
    movl r38=0xe5263dc5c11c902d ;;
    movl r39=0x7 ;;
    movl r40=0xfffeffff ;;
    movl r41=0x9035119312f3371a ;;
    movl r42=0x40 ;;
    movl r2=0x37e82 ;;
    setf.exp f16=r2 ;;
    movl r2=0xfff0000000000000 ;;
    setf.d f21=r2 ;;
    movl r2=1 ;;
    mov ar.lc=r2 ;;
    mov ar.ec=2 ;;
.Lf119:
    cmp.lt.or p48,p30=r0,r33
    (p54) shladdp4 r36=r32,2,r37
    (p16) tbit.z.or p1,p60=r39,34 ;;
    br.cexit.spnt.few .Lf119x ;;
    br.cond.sptk.few .Lf119 ;;
.Lf119x:
    (p1) fsub.s3 f121=f21,f16 ;;
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
    srlz.d ;;
    rfi ;;
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
    data8 0xb, 0x1e, 0xfbf1daadf6e76e80, 0x950d6c0a643cdfe3
    data8 0x3a, 0x2d, 0x1f, 0x7fffffff7fffffff
    data8 0x10007fffffff, 0x45f8e635baea646, 0xffffffff, 0x1c106d4cba44a4e
    data8 0x8000, 0xaaaaaaaaaaaaaaaa, 0x101010101010101, 0xed4d9ac3627597ed
    data8 0x7, 0x8080808080808080, 0x26a5dcfa6848549a, 0x0
    data8 0xd5933da986a4d994, 0xffffffffffffffff, 0xfffffffffffffffe, 0x929851a006414d6f
    data8 0xc726fc2d0354b0c1, 0x8000000000000000, 0xffffffffffffffe0, 0x3520533ce846fd98
    data8 0xffffffffffffffea, 0x7b0afb61077f9fc2, 0x7fffffff, 0x40
    data8 0x5319ced99b5220e3, 0x2fd533c277c8918c, 0xe731fe0a86fe3b89, 0x820492a5d4023f3e
    data8 0x6e0710b33a27867c, 0xffffffffffffffff, 0x0, 0x8080808000808080
    data8 0xeec6ee74de52152b, 0x20000000000000, 0x20000000100, 0x1729a1e6b9307f29
    data8 0x8000000000000000, 0x1e, 0x8000000000000001, 0x7f7f7f7f7f7f7f7f
    data8 0xfffffffffffffffd, 0xff00ff00ff40ff, 0x7, 0xffffffffffffffff
    data8 0xaaaaaaaaaaaaaaaa, 0x1b6d96900c582cc8, 0x3f, 0x7
    data8 0x7f7f7f7f7f7f7f7f, 0x20007f, 0x8000000000000000, 0x1d
    data8 0xffff8000, 0x8080808080828080, 0x8000000080000000, 0x8000800080008000
