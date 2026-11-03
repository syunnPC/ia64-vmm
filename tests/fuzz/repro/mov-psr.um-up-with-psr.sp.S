/* SPDX-License-Identifier: GPL-2.0-or-later */
// Reproducer (seed 70000); run: python3 tests/fuzz/fuzz.py --run tests/fuzz/repro/mov-psr.um-up-with-psr.sp.S
// category: state
// where:    state before bundle #29 at 0x1001d0
// culprit bundle 0x1001c0: mov psr.um=r5 ; srlz.d
//   psr          interp=0x0000100000d0a030  fork=0x0000100000d0a034
// ia64 differential fuzz program, seed 70000
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
    alloc r2=ar.pfs,0,8,5,8 ;;
    movl r32=0x6b6b0f7bb50713af ;;
    movl r33=0x1000000000000f ;;
    movl r34=0x8 ;;
    movl r35=0xc1780179a206c521 ;;
    movl r36=0x8000 ;;
    movl r37=0xfffffffffffffff4 ;;
    movl r38=0x80000000 ;;
    movl r39=0x7fff ;;
    movl r40=0x1f ;;
    movl r41=0x8b0dec3781e83286 ;;
    movl r42=0xffff0000ffff0000 ;;
    movl r43=0x3c ;;
    movl r44=0x600dff0ecbc1b14c ;;
    movl r2=0xb2a70d07bf552c1a ;;
    mov pr=r2,-1 ;;
    mov r14=psr ;;
    movl r2=0x908000 ;;
    xor r14=r14,r2 ;;
    mov psr.l=r14 ;;
    srlz.d ;;
    (p10) ssm 0xc00004 ;;
    srlz.d ;;
    (p18) chk.s.i r17,.Lf120 ;;
    movl r5=0x30 ;;
    mov psr.um=r5 ;;
    srlz.d ;;
    mov r43=psr.um ;;
    mov b3=r0 ;;
.Lf120:
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
    break.m 0x1ffffe ;;
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
    data8 0x3, 0x100, 0xbc779640128656e, 0x7f
    data8 0x20, 0x7fff7fff7fff7fff, 0x36, 0xf
    data8 0x7fff7fff7fff7fff, 0x40, 0x8000000080000000, 0xbf5ad0d52297eb4e
    data8 0x32, 0x2e036b49dbf0ab28, 0x8080808080808000, 0x7f
    data8 0xb, 0x35, 0x100, 0x7c244dc18b2407fd
    data8 0x100, 0x200008, 0x37adbdd2b03211d1, 0x7fffffff
    data8 0x40ac4af02aa86a9d, 0xffffffffffffffff, 0x7fff7fff7fff7fff, 0xffffffffffffffc6
    data8 0x7f7f7f7f7f7f7f7f, 0xffffdffffffffffe, 0x43e9773540f2f90e, 0xfffffffffffffffd
    data8 0xb6a5cd7850fd90ba, 0x8080808080808080, 0xec1cbe70ba185a15, 0x8080808080808080
    data8 0x2704cd6c57da837c, 0x7fff7fff7fff7fff, 0xffff0000ffff0000, 0xff00ff00ff00ff00
    data8 0x39b73ba8712fa4bc, 0xffffffff80000000, 0xffffffffffdfffff, 0xe793078c3edd1297
    data8 0x8dc53a027db5e2d6, 0xc36790a2fba3a282, 0x80000001, 0xa668f5cec1dd0ad0
    data8 0x7fffffffffffffff, 0x80000000, 0xfffffffff7ffffff, 0x8000000000000002
    data8 0x1e, 0xdbdc130fd6d2d1b5, 0x732474129291bf81, 0xe3051733e0269231
    data8 0xffffffffffffffdc, 0x100, 0x7865291a785de438, 0x7d10973882d3a1ec
    data8 0x7fffffff, 0x8000, 0xffff8000, 0x881d08c35eefc6cc
