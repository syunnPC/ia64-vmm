/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x4f6341279a23bef6   add R = r2, r3
// EXPECT r9 = 0x4f6341279a23bef7   add R = r2, r3, 1
// EXPECT r10 = 0xb09cbed865dc4108   sub R = r2, r3
// EXPECT r11 = 0xb09cbed865dc4107   sub R = r2, r3, 1
// EXPECT r12 = 0xb09cbed865dc4127   sub R = 30, r3
// EXPECT r13 = 0x16   and R = 30, r3
// EXPECT r14 = 0x4f6341279a23beff   or R = 30, r3
// EXPECT r15 = 0x4f6341279a23bee9   xor R = 30, r3
// EXPECT r16 = 0x8   andcm R = 30, r3
// EXPECT r17 = 0x4f6341279a23bef7   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0xb09cbed865dc4108   xor R = r2, r3
// EXPECT r20 = 0xb09cbed865dc4108   andcm R = r2, r3
// EXPECT r21 = 0x4f6341279a23cd45   adds R = 3662, r3
// EXPECT r22 = 0x10dd8   addl R = 69080, r1
// EXPECT r23 = 0x4f6341279a23bef5   shladd R = r2, 1, r3
// EXPECT r24 = 0x400000009a23bef5   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x4f6341279a23bef3   shladd R = r2, 2, r3
// EXPECT r26 = 0x400000009a23bef3   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x4f6341279a23beef   shladd R = r2, 3, r3
// EXPECT r28 = 0x400000009a23beef   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x4f6341279a23bee7   shladd R = r2, 4, r3
// EXPECT r30 = 0x400000009a23bee7   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x400000009a23bef6   addp4 R = r2, r3
// EXPECT r32 = 0x400000009a23cd45   addp4 R = 3662, r3
// EXPECT r33 = 0xffffffff00000000   shl R = r2, r4
// EXPECT r34 = 0x4f634127   shr.u R = r3, r4
// EXPECT r35 = 0x4f634127   shr R = r3, r4
// EXPECT r36 = 0x27b   extr.u R = r3, 53, 12
// EXPECT r37 = 0x27b   extr R = r3, 53, 12
// EXPECT r38 = 0xffe0000000000000   dep.z R = r2, 53, 12
// EXPECT r39 = 0x640000000000000   dep.z R = 50, 53, 12
// EXPECT r40 = 0xffe341279a23bef7   dep R = r2, r3, 53, 16
// EXPECT r41 = 0xffe341279a23bef7   dep R = -1, r3, 53, 12
// EXPECT r42 = 0xfffffd3d8d049e68   shrp R = r2, r3, 22
// EXPECT r43 = 0xffffffffffc00000   shl R = r2, 22
// EXPECT r44 = 0x13d8d049e68   shr.u R = r3, 22
// EXPECT r45 = 0x13d8d049e68   shr R = r3, 22
// EXPECT r46 = 0x9   extr.u R = r3, 37, 6
// EXPECT r47 = 0x9   extr R = r3, 37, 6
// EXPECT r48 = 0x7e000000000   dep.z R = r2, 37, 6
// EXPECT r49 = 0x20000000000   dep.z R = 16, 37, 6
// EXPECT r50 = 0x4f6341e79a23bef7   dep R = r2, r3, 37, 3
// EXPECT r51 = 0x4f6340079a23bef7   dep R = 0, r3, 37, 6
// EXPECT r52 = 0xfffffffffffe9ec6   shrp R = r2, r3, 47
// EXPECT r53 = 0xffff800000000000   shl R = r2, 47
// EXPECT r54 = 0x9ec6   shr.u R = r3, 47
// EXPECT r55 = 0x9ec6   shr R = r3, 47
// EXPECT r56 = 0x13d8d049e   extr.u R = r3, 30, 63
// EXPECT r57 = 0x13d8d049e   extr R = r3, 30, 63
// EXPECT r58 = 0xffffffffc0000000   dep.z R = r2, 30, 63
// EXPECT r59 = 0xfffffff3c0000000   dep.z R = -49, 30, 63
// EXPECT r60 = 0x4f6341ffda23bef7   dep R = r2, r3, 30, 10
// EXPECT r61 = 0xffffffffda23bef7   dep R = -1, r3, 30, 63
// EXPECT r62 = 0xffffffffff4f6341   shrp R = r2, r3, 40
// EXPECT r63 = 0xffffff0000000000   shl R = r2, 40
// EXPECT r64 = 0x4f6341   shr.u R = r3, 40
// EXPECT r65 = 0x4f6341   shr R = r3, 40
// EXPECT r66 = 0x13   extr.u R = r3, 58, 59
// EXPECT r67 = 0x13   extr R = r3, 58, 59
// EXPECT r68 = 0xfc00000000000000   dep.z R = r2, 58, 59
// EXPECT r69 = 0x8400000000000000   dep.z R = -95, 58, 59
// EXPECT r70 = 0xff6341279a23bef7   dep R = r2, r3, 58, 6
// EXPECT r71 = 0xff6341279a23bef7   dep R = -1, r3, 58, 59
// EXPECT r72 = 0xa7b1a093cd11df7b   shrp R = r2, r3, 1
// EXPECT r73 = 0xfffffffffffffffe   shl R = r2, 1
// EXPECT r74 = 0x27b1a093cd11df7b   shr.u R = r3, 1
// EXPECT r75 = 0x27b1a093cd11df7b   shr R = r3, 1
// EXPECT r76 = 0x4f   extr.u R = r3, 56, 63
// EXPECT r77 = 0x4f   extr R = r3, 56, 63
// EXPECT r78 = 0xff00000000000000   dep.z R = r2, 56, 63
// EXPECT r79 = 0x8700000000000000   dep.z R = -121, 56, 63
// EXPECT r80 = 0xff6341279a23bef7   dep R = r2, r3, 56, 16
// EXPECT r81 = 0x6341279a23bef7   dep R = 0, r3, 56, 63
// EXPECT r82 = 0xffffffffffffffd3   shrp R = r2, r3, 58
// EXPECT r83 = 0xfc00000000000000   shl R = r2, 58
// EXPECT r84 = 0x13   shr.u R = r3, 58
// EXPECT r85 = 0x13   shr R = r3, 58
// EXPECT r86 = 0x13d8d049e688ef   extr.u R = r3, 10, 64
// EXPECT r87 = 0x13d8d049e688ef   extr R = r3, 10, 64
// EXPECT r88 = 0xfffffffffffffc00   dep.z R = r2, 10, 64
// EXPECT r89 = 0xfffffffffffe2c00   dep.z R = -117, 10, 64
// EXPECT r90 = 0x4f6341279a23fef7   dep R = r2, r3, 10, 5
// EXPECT r91 = 0x2f7   dep R = 0, r3, 10, 64
// EXPECT r92 = 0xffffffffffffe9ec   shrp R = r2, r3, 51
// EXPECT r93 = 0xfff8000000000000   shl R = r2, 51
// EXPECT r94 = 0x9ec   shr.u R = r3, 51
// EXPECT r95 = 0x9ec   shr R = r3, 51
// EXPECT r96 = 0xfffffffffffffff7   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffbef7   sxt2 R = r3
// EXPECT r98 = 0xffffffff9a23bef7   sxt4 R = r3
// EXPECT r99 = 0xf7   zxt1 R = r3
// EXPECT r100 = 0xbef7   zxt2 R = r3
// EXPECT r101 = 0x9a23bef7   zxt4 R = r3
// EXPECT r102 = 0x23   popcnt R = r3
// EXPECT p6 = 1   cmp.gtu P1, P2 = -12, r3
// EXPECT p7 = 0
// EXPECT p8 = 1   cmp4.gt P1, P2 = -70, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   tbit.z P1, P2 = r3, 1
// EXPECT p11 = 1
// EXPECT p12 = 1   cmp.ltu P1, P2 = 76, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   tbit.z P1, P2 = r3, 32
// EXPECT p17 = 1
// EXPECT p18 = 1   cmp.le P1, P2 = 35, r3
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp.ge P1, P2 = 35, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p23 = 1
// EXPECT p24 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p25 = 1
// EXPECT p26 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p27 = 0
// EXPECT p28 = 0   cmp4.lt P1, P2 = -4, r3
// EXPECT p29 = 1
// EXPECT p30 = 0   cmp4.le P1, P2 = 73, r3
// EXPECT p31 = 1
// EXPECT p32 = 1   cmp4.gtu P1, P2 = -104, r3
// EXPECT p33 = 0
// EXPECT p34 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p35 = 0
// EXPECT p36 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p37 = 1
// EXPECT p38 = 1   cmp4.ge P1, P2 = 73, r3
// EXPECT p39 = 0
// EXPECT p40 = 0   cmp.eq P1, P2 = 61, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 1   tbit.z P1, P2 = r3, 30
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp.gt P1, P2 = -92, r3
// EXPECT p49 = 1
// EXPECT p50 = 0   cmp4.eq P1, P2 = -81, r3
// EXPECT p51 = 1
// EXPECT p52 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p53 = 0
// EXPECT p54 = 1   cmp4.gt P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp.ne P1, P2 = -112, r3
// EXPECT p59 = 0
// EXPECT p60 = 0   cmp4.ltu P1, P2 = -124, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp4.ge P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0xffffffffffffffff
  movl r3 = 0x4f6341279a23bef7
  movl r4 = 0x20
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 30, r3
  and r13 = 30, r3
  or r14 = 30, r3
  xor r15 = 30, r3
  andcm r16 = 30, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 3662, r3
  addl r22 = 69080, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 3662, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 53, 12
  extr r37 = r3, 53, 12
  dep.z r38 = r2, 53, 12
  dep.z r39 = 50, 53, 12
  dep r40 = r2, r3, 53, 16
  dep r41 = -1, r3, 53, 12
  shrp r42 = r2, r3, 22
  shl r43 = r2, 22
  shr.u r44 = r3, 22
  shr r45 = r3, 22
  extr.u r46 = r3, 37, 6
  extr r47 = r3, 37, 6
  dep.z r48 = r2, 37, 6
  dep.z r49 = 16, 37, 6
  dep r50 = r2, r3, 37, 3
  dep r51 = 0, r3, 37, 6
  shrp r52 = r2, r3, 47
  shl r53 = r2, 47
  shr.u r54 = r3, 47
  shr r55 = r3, 47
  extr.u r56 = r3, 30, 63
  extr r57 = r3, 30, 63
  dep.z r58 = r2, 30, 63
  dep.z r59 = -49, 30, 63
  dep r60 = r2, r3, 30, 10
  dep r61 = -1, r3, 30, 63
  shrp r62 = r2, r3, 40
  shl r63 = r2, 40
  shr.u r64 = r3, 40
  shr r65 = r3, 40
  extr.u r66 = r3, 58, 59
  extr r67 = r3, 58, 59
  dep.z r68 = r2, 58, 59
  dep.z r69 = -95, 58, 59
  dep r70 = r2, r3, 58, 6
  dep r71 = -1, r3, 58, 59
  shrp r72 = r2, r3, 1
  shl r73 = r2, 1
  shr.u r74 = r3, 1
  shr r75 = r3, 1
  extr.u r76 = r3, 56, 63
  extr r77 = r3, 56, 63
  dep.z r78 = r2, 56, 63
  dep.z r79 = -121, 56, 63
  dep r80 = r2, r3, 56, 16
  dep r81 = 0, r3, 56, 63
  shrp r82 = r2, r3, 58
  shl r83 = r2, 58
  shr.u r84 = r3, 58
  shr r85 = r3, 58
  extr.u r86 = r3, 10, 64
  extr r87 = r3, 10, 64
  dep.z r88 = r2, 10, 64
  dep.z r89 = -117, 10, 64
  dep r90 = r2, r3, 10, 5
  dep r91 = 0, r3, 10, 64
  shrp r92 = r2, r3, 51
  shl r93 = r2, 51
  shr.u r94 = r3, 51
  shr r95 = r3, 51
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.gtu p6, p7 = -12, r3
  cmp4.gt p8, p9 = -70, r3
  tbit.z p10, p11 = r3, 1
  cmp.ltu p12, p13 = 76, r3
  cmp.ltu p14, p15 = r2, r3
  tbit.z p16, p17 = r3, 32
  cmp.le p18, p19 = 35, r3
  cmp.ge p20, p21 = 35, r3
  cmp4.lt p22, p23 = r2, r3
  cmp4.ltu p24, p25 = r2, r3
  cmp.ne p26, p27 = r2, r3
  cmp4.lt p28, p29 = -4, r3
  cmp4.le p30, p31 = 73, r3
  cmp4.gtu p32, p33 = -104, r3
  cmp4.gtu p34, p35 = r2, r3
  cmp.ge p36, p37 = r2, r3
  cmp4.ge p38, p39 = 73, r3
  cmp.eq p40, p41 = 61, r3
  cmp.eq p42, p43 = r2, r3
  cmp4.ne p44, p45 = r2, r3
  tbit.z p46, p47 = r3, 30
  cmp.gt p48, p49 = -92, r3
  cmp4.eq p50, p51 = -81, r3
  cmp.le p52, p53 = r2, r3
  cmp4.gt p54, p55 = r2, r3
  cmp.gt p56, p57 = r2, r3
  cmp.ne p58, p59 = -112, r3
  cmp4.ltu p60, p61 = -124, r3
  cmp4.ge p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
