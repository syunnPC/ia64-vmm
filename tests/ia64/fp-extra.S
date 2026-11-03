/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x4000000000000000
// EXPECT r9 = 0x3f80000040400000
// EXPECT r10 = 0x4160000041000000
// EXPECT r11 = 0x40000000c0000000
// EXPECT r12 = 0xc000000040000000
// EXPECT r13 = 0xc0000000bf800000
// EXPECT r14 = 0xffffffffffffffff
// EXPECT r15 = 0x0000000200000001
// EXPECT r16 = 0x3eff80003f7f8000
// EXPECT r17 = 0x3f34a0003f7f8000
// EXPECT r18 = 0x0c
// EXPECT p6 = 1
// EXPECT p7 = 0
// EXPECT p9 = 1
// EXPECT p10 = 1
.text
.global _start
_start:
    movl r20=0x4008000000000000
    ;;
    setf.d f7=r20
    ;;
    fsub.d f6=f7,f1
    ;;
    getf.d r8=f6
    fpack f6=f1,f7
    ;;
    getf.sig r9=f6
    movl r20=0x400000003f800000
    movl r21=0x4080000040400000
    movl r22=0x40c0000040a00000
    ;;
    setf.sig f7=r20
    setf.sig f8=r21
    setf.sig f9=r22
    ;;
    fpma f6=f7,f8,f9
    ;;
    getf.sig r10=f6
    fpms f6=f7,f8,f9
    ;;
    getf.sig r11=f6
    fpnma f6=f7,f8,f9
    ;;
    getf.sig r12=f6
    fpneg f6=f7
    ;;
    getf.sig r13=f6
    fpcmp.lt f6=f7,f8
    ;;
    getf.sig r14=f6
    fpcvt.fx.trunc f6=f7
    ;;
    getf.sig r15=f6
    fprcpa f6,p9=f8,f7
    ;;
    getf.sig r16=f6
    fprsqrta f6,p10=f7
    ;;
    getf.sig r17=f6
    fsetc.s1 0x7f,0
    ;;
    mov r18=ar.fpsr
    ;;
    shr.u r18=r18,19
    ;;
    and r18=0x7f,r18
    fclass.m p6,p7=f1,@pos|@norm
    ;;
    break.i 0x1ffffe
