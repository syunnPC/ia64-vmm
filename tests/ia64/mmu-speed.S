/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x2dc6c0
.text
.explicit
.global _start
_start:
    rsm psr.ic ;;
    srlz.d ;;
    mov r2=0 ;;
    mov r3=64 ;;
    mov rr[r2]=r3 ;;
    srlz.d ;;
    movl r3=0x100000 ;;
    mov cr.ifa=r3 ;;
    mov r3=64 ;;
    mov cr.itir=r3 ;;
    movl r3=0x1007e1 ;;
    itr.i itr[r2]=r3 ;;
    srlz.i ;;
    movl r10=0x2000000000400000 ;;
    mov r3=304 ;;
    mov rr[r10]=r3 ;;
    srlz.d ;;
    mov cr.ifa=r10 ;;
    mov r3=48 ;;
    mov cr.itir=r3 ;;
    movl r3=0x1087e1 ;;
    itr.d dtr[r2]=r3 ;;
    srlz.d ;;
    movl r2=0x108000 ;;
    st8 [r2]=r0 ;;
    movl r2=0x1000022000 ;;
    mov cr.ipsr=r2 ;;
    movl r2=.virtual ;;
    mov cr.iip=r2 ;;
    mov cr.ifs=r0 ;;
    rfi ;;
.align 16
.virtual:
    movl r20=2999999 ;;
    mov ar.lc=r20 ;;
.loop:
    { .mii
      ld8 r8=[r10]
      nop.i 0 ;;
      adds r8=1,r8
      ;;
    }
    { .mib
      st8 [r10]=r8
      nop.i 0
      br.cloop.sptk .loop ;;
    }
    break.m 0x1ffffe ;;
