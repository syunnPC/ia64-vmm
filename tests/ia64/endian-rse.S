/* SPDX-License-Identifier: GPL-2.0-or-later */
// RSC.be = 1: RSE stores and loads, NaT collections included, are
// big-endian independent of UM.be (SDM Vol.2 §6.4). The spilled words are
// read with UM.be = 0, then replaced and refilled by br.ret after
// mov ar.bspstore invalidates the clean registers (§6.5.3). DCR.be is the
// handler's PSR.be on interruption (Vol.2 §5.5); IPSR keeps the old value.
// EXPECT r8 = 0x1
// EXPECT r10 = 0x10c208
// EXPECT r11 = 0x0
// EXPECT r12 = 0x0807060504030201
// EXPECT r13 = 0x0000000000000040
// EXPECT r14 = 0x1817161514131211
// EXPECT r17 = 0x2122232425262728
// EXPECT r18 = 0x3132333435363738
// EXPECT r20 = 0x2
// EXPECT r21 = 0x01020304
// EXPECT p6 = 1
.text
.global _start
_start:
    mov ar.rsc=0
    movl r2=0x10c1e8
    ;;
    mov ar.bspstore=r2
    ;;
    mov ar.rnat=r0
    mov r3=0x10
    ;;
    mov ar.rsc=r3
    alloc r9=ar.pfs,0,3,0,0
    movl r2=0x8000000000000
    ;;
    movl r32=0x0102030405060708
    ld8.s r33=[r2]
    movl r34=0x1112131415161718
    br.call.sptk.many b0=.callee
    ;;
    mov r17=r32
    mov r18=r34
    tnat.nz p6,p7=r33
    movl r2=0x10a000
    movl r3=0x04030201
    ;;
    st4 [r2]=r3
    movl r2=0x100000
    ;;
    mov cr.iva=r2
    mov r3=2
    ;;
    mov cr.dcr=r3
    ssm psr.ic
    ;;
    srlz.i
    ;;
    br.cond.sptk .test
.callee:
    flushrs
    ;;
    mov r10=ar.bspstore
    movl r2=0x10c1e8
    movl r3=0x10c1f8
    movl r4=0x10c200
    ;;
    ld8 r12=[r2]
    ld8 r13=[r3]
    ld8 r14=[r4]
    movl r5=0x2827262524232221
    movl r6=0x3837363534333231
    ;;
    st8 [r2]=r5
    st8 [r4]=r6
    mov r15=ar.rnat
    mov r16=ar.bspstore
    ;;
    mov ar.bspstore=r16
    ;;
    mov ar.rnat=r15
    br.ret.sptk.many b0
.org 0x400
.explicit
.test:
    { .mii
      nop.m 0
      break.i 0x12345
      adds r8=1,r0
      ;;
    }
    { .mii
      mov cr.dcr=r0
      nop.i 0
      nop.i 0
      ;;
    }
    { .mii
      srlz.i
      nop.i 0
      nop.i 0
      ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0
      ;;
    }
.auto
.org 0x2c00
.break_handler:
    mov r20=psr
    movl r2=0x10a000
    mov r11=cr.ipsr
    ;;
    and r20=2,r20
    and r11=2,r11
    ld4 r21=[r2]
    mov r3=cr.ipsr
    ;;
    movl r2=0x20000000000
    ;;
    add r2=r3,r2
    ;;
    mov cr.ipsr=r2
    ;;
    rfi
