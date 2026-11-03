/* SPDX-License-Identifier: GPL-2.0-or-later */
// Big-endian floating-point loads and stores with UM.be = 1. Single,
// double and integer data use the integer byte order (SDM Vol.1 §5.3.1);
// paired loads order each element (Vol.3 Table 3-1 mem_read_pair, first
// register from the lower address); double-extended and spill/fill data
// are fully reversed (Vol.1 Figure 5-10). Memory is set up and read back
// with UM.be = 0.
// EXPECT r8 = 0x3f800000
// EXPECT r9 = 0x3ff0000000000000
// EXPECT r10 = 0x0102030405060708
// EXPECT r11 = 0x3f800000
// EXPECT r12 = 0x40000000
// EXPECT r13 = 0x3ff0000000000000
// EXPECT r14 = 0x4000000000000000
// EXPECT r15 = 0x0102030405060708
// EXPECT r16 = 0x1112131415161718
// EXPECT r17 = 0x30005
// EXPECT r18 = 0x8a0b0c0d0e0f1011
// EXPECT r19 = 0x3003e
// EXPECT r20 = 0x8a0b0c0d0e0f1011
// EXPECT r21 = 0x0807060504030201
// EXPECT r22 = 0x0000803f
// EXPECT r23 = 0x000000000000f03f
// EXPECT r24 = 0x0f0e0d0c0b8a05c0
// EXPECT r25 = 0x1110
// EXPECT r26 = 0x3e00030000000000
// EXPECT r27 = 0x11100f0e0d0c0b8a
.text
.global _start
_start:
    movl r2=0x10a000
    movl r3=0x0000803f
    ;;
    st4 [r2]=r3
    adds r4=0x08,r2
    movl r3=0x000000000000f03f
    ;;
    st8 [r4]=r3
    adds r4=0x10,r2
    movl r3=0x0807060504030201
    ;;
    st8 [r4]=r3
    adds r4=0x18,r2
    movl r3=0x000000400000803f
    ;;
    st8 [r4]=r3
    adds r4=0x20,r2
    movl r3=0x000000000000f03f
    ;;
    st8 [r4]=r3
    adds r4=0x28,r2
    movl r3=0x0000000000000040
    ;;
    st8 [r4]=r3
    adds r4=0x30,r2
    movl r3=0x0807060504030201
    ;;
    st8 [r4]=r3
    adds r4=0x38,r2
    movl r3=0x1817161514131211
    ;;
    st8 [r4]=r3
    adds r4=0x40,r2
    movl r3=0x0f0e0d0c0b8a05c0
    ;;
    st8 [r4]=r3
    adds r4=0x48,r2
    movl r3=0x1110
    ;;
    st2 [r4]=r3
    adds r4=0x50,r2
    movl r3=0x3e00030000000000
    ;;
    st8 [r4]=r3
    adds r4=0x58,r2
    movl r3=0x11100f0e0d0c0b8a
    ;;
    st8 [r4]=r3
    sum 0x2
    ;;
    ldfs f6=[r2]
    adds r4=0x08,r2
    ;;
    ldfd f7=[r4]
    adds r4=0x10,r2
    ;;
    ldf8 f8=[r4]
    ;;
    getf.s r8=f6
    getf.d r9=f7
    getf.sig r10=f8
    adds r4=0x18,r2
    ;;
    ldfps f6,f7=[r4]
    adds r4=0x20,r2
    ;;
    ldfpd f8,f9=[r4]
    adds r4=0x30,r2
    ;;
    ldfp8 f10,f11=[r4]
    adds r4=0x40,r2
    ;;
    ldfe f12=[r4]
    adds r4=0x50,r2
    ;;
    ldf.fill f13=[r4]
    ;;
    getf.s r11=f6
    getf.s r12=f7
    getf.d r13=f8
    getf.d r14=f9
    getf.sig r15=f10
    getf.sig r16=f11
    getf.exp r17=f12
    getf.sig r18=f12
    getf.exp r19=f13
    getf.sig r20=f13
    movl r3=0x0102030405060708
    ;;
    setf.sig f14=r3
    adds r4=0x60,r2
    ;;
    stf8 [r4]=f14
    adds r4=0x68,r2
    ;;
    stfs [r4]=f1
    adds r4=0x70,r2
    ;;
    stfd [r4]=f1
    adds r4=0x80,r2
    ;;
    stfe [r4]=f12
    adds r4=0x90,r2
    ;;
    stf.spill [r4]=f13
    rum 0x2
    ;;
    adds r4=0x60,r2
    ;;
    ld8 r21=[r4]
    adds r4=0x68,r2
    ;;
    ld4 r22=[r4]
    adds r4=0x70,r2
    ;;
    ld8 r23=[r4]
    adds r4=0x80,r2
    ;;
    ld8 r24=[r4]
    adds r4=0x88,r2
    ;;
    ld2 r25=[r4]
    adds r4=0x90,r2
    ;;
    ld8 r26=[r4]
    adds r4=0x98,r2
    ;;
    ld8 r27=[r4]
    break.i 0x1ffffe
