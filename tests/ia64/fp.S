/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xbcf0000000000000
// EXPECT r9 = 0x2
// EXPECT r10 = 0x3ff8000000000000
// EXPECT r11 = 0xfffffffffffffffd
// EXPECT r12 = 0xbff0000000000000
// EXPECT r13 = 0x0123456789abcdef
// EXPECT r14 = 0x1003e
// EXPECT r15 = 0x0123456789abcdef
// EXPECT r16 = 0x1003e
// EXPECT p6 = 1
// EXPECT p7 = 0
.text
.global _start
_start:
    movl r20=0x3ff0000010000000
    movl r21=0x3fefffffe0000000
    movl r22=0xbff0000000000000
    ;;
    setf.d f7=r20
    setf.d f8=r21
    setf.d f9=r22
    ;;
    fma.s f6=f7,f8,f9
    ;;
    getf.d r8=f6
    movl r20=0x3ff8000000000000
    ;;
    setf.d f7=r20
    ;;
    fcvt.fx f8=f7
    ;;
    getf.sig r9=f8
    getf.d r10=f7
    mov r20=-3
    ;;
    setf.sig f8=r20
    ;;
    fcvt.xf f9=f8
    ;;
    fcvt.fx.trunc f9=f9
    ;;
    getf.sig r11=f9
    fmerge.ns f6=f1,f1
    ;;
    getf.d r12=f6
    movl r20=0x0123456789abcdef
    movl r21=0x108000
    ;;
    setf.sig f6=r20
    ;;
    stf.spill [r21]=f6
    ;;
    ldf.fill f7=[r21]
    ;;
    getf.sig r13=f7
    getf.exp r14=f7
    ;;
    ldfp8 f8,f9=[r21]
    ;;
    getf.sig r15=f8
    getf.sig r16=f9
    fcmp.eq p6,p7=f6,f7
    ;;
    break.i 0x1ffffe
