/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Rev.2.3 Vol.3 st; Vol.2 §§3.3.5.7, 5.1, 8.1-8.2: a store to an
 * untranslated address reports a write reference and preserves its bundle
 * and slot. cover captures the interrupted frame, including stacked r36.
 * Keep the address and value in separate static/stacked registers. */
// EXPECT r8 = 0x20200000000
// EXPECT r9 = 0
// EXPECT r10 = 0xa0000001001d4bf0
// EXPECT r11 = 0x121008026010
// EXPECT r12 = 0x800000000000193a
// EXPECT r14 = 3
// EXPECT r15 = 0
.text
.global _start
_start:
    rsm psr.ic
    bsw.1
    ;;
    movl r20=0x100000
    ;;
    mov cr.iva=r20
    alloc r31=ar.pfs,0,50,8,0
    mov r21=56
    ;;
    mov rr[r20]=r21
    mov cr.ifa=r20
    mov cr.itir=r21
    movl r22=0x100e61
    ;;
    itr.i itr[r0]=r22
    movl r20=0xa0000001001d4bf0
    ;;
    mov rr[r20]=r21
    mov cr.ifa=r20
    mov cr.itir=r21
    movl r22=0x104e61
    mov r23=1
    ;;
    itr.i itr[r23]=r22
    mov r29=3
    mov r36=0
    mov cr.iip=r20
    movl r21=0x121008026010
    ;;
    mov cr.ipsr=r21
    movl r21=0x800000000000193a
    ;;
    mov cr.ifs=r21
    ;;
    rfi
.org 0x1000
    mov r8=cr.isr
    mov r9=cr.ifa
    mov r10=cr.iip
    mov r11=cr.ipsr
    mov r15=r36
    cover
    ;;
    mov r12=cr.ifs
    bsw.1
    ;;
    mov r14=r29
    break.m 0x1ffffe
.org 0x4bf0
.explicit
    { .mmb
      nop.m 0
      st4 [r36]=r29
      br.call.sptk.many b0=_start
      ;;
    }
