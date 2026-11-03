/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x12c
// EXPECT r10 = 0x19
// EXPECT r11 = 0x108000
.text
.global _start
_start:
    mov ar.rsc=0
    movl r2=0x108000
    ;;
    mov ar.bspstore=r2
    ;;
    mov ar.rnat=r0
    mov ar.rsc=3
    alloc r2=ar.pfs,0,0,1,0
    ;;
    mov r32=24
    br.call.sptk.many b0=.recursive
    ;;
    mov r11=ar.bsp
    break.i 0x1ffffe
.recursive:
    alloc r33=ar.pfs,1,7,1,0
    ;;
    mov r34=b0
    mov r36=r32
    movl r2=0x8000000000000
    ;;
    ld8.s r35=[r2]
    cmp.eq p6,p7=0,r32
    (p6) br.cond.sptk .leaf
    ;;
    adds r40=-1,r32
    br.call.sptk.many b0=.recursive
    .align 16
    br.cond.sptk .finish
.leaf:
    mov r8=0
    mov r10=0
.finish:
    add r8=r8,r36
    tnat.nz p8,p9=r35
    (p8) adds r10=1,r10
    mov ar.pfs=r33
    mov b0=r34
    br.ret.sptk.many b0
