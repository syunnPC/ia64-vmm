/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x141747c23   add R = r2, r3
// EXPECT r9 = 0x141747c24   add R = r2, r3, 1
// EXPECT r10 = 0xffffffff41747c23   sub R = r2, r3
// EXPECT r11 = 0xffffffff41747c22   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffff0000004e   sub R = 78, r3
// EXPECT r13 = 0x0   and R = 78, r3
// EXPECT r14 = 0x10000004e   or R = 78, r3
// EXPECT r15 = 0x10000004e   xor R = 78, r3
// EXPECT r16 = 0x4e   andcm R = 78, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x141747c23   or R = r2, r3
// EXPECT r19 = 0x141747c23   xor R = r2, r3
// EXPECT r20 = 0x41747c23   andcm R = r2, r3
// EXPECT r21 = 0xffffe8e0   adds R = -5920, r3
// EXPECT r22 = 0xfffffffffff04427   addl R = -1031129, r1
// EXPECT r23 = 0x182e8f846   shladd R = r2, 1, r3
// EXPECT r24 = 0x82e8f846   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x205d1f08c   shladd R = r2, 2, r3
// EXPECT r26 = 0x5d1f08c   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x30ba3e118   shladd R = r2, 3, r3
// EXPECT r28 = 0xba3e118   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x51747c230   shladd R = r2, 4, r3
// EXPECT r30 = 0x1747c230   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x41747c23   addp4 R = r2, r3
// EXPECT r32 = 0xffffe8e0   addp4 R = -5920, r3
// EXPECT r33 = 0x82e8f846   shl R = r2, r4
// EXPECT r34 = 0x80000000   shr.u R = r3, r4
// EXPECT r35 = 0x80000000   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 5, 23
// EXPECT r37 = 0x0   extr R = r3, 5, 23
// EXPECT r38 = 0xe8f8460   dep.z R = r2, 5, 23
// EXPECT r39 = 0xffff4e0   dep.z R = -89, 5, 23
// EXPECT r40 = 0x100000460   dep R = r2, r3, 5, 8
// EXPECT r41 = 0x10fffffe0   dep R = -1, r3, 5, 23
// EXPECT r42 = 0x20ba3e1180   shrp R = r2, r3, 57
// EXPECT r43 = 0x4600000000000000   shl R = r2, 57
// EXPECT r44 = 0x0   shr.u R = r3, 57
// EXPECT r45 = 0x0   shr R = r3, 57
// EXPECT r46 = 0x0   extr.u R = r3, 55, 64
// EXPECT r47 = 0x0   extr R = r3, 55, 64
// EXPECT r48 = 0x1180000000000000   dep.z R = r2, 55, 64
// EXPECT r49 = 0x3b80000000000000   dep.z R = 119, 55, 64
// EXPECT r50 = 0x1180000100000000   dep R = r2, r3, 55, 15
// EXPECT r51 = 0x100000000   dep R = 0, r3, 55, 64
// EXPECT r52 = 0x82e8f846   shrp R = r2, r3, 63
// EXPECT r53 = 0x8000000000000000   shl R = r2, 63
// EXPECT r54 = 0x0   shr.u R = r3, 63
// EXPECT r55 = 0x0   shr R = r3, 63
// EXPECT r56 = 0x0   extr.u R = r3, 36, 22
// EXPECT r57 = 0x0   extr R = r3, 36, 22
// EXPECT r58 = 0x347c23000000000   dep.z R = r2, 36, 22
// EXPECT r59 = 0x3fff98000000000   dep.z R = -104, 36, 22
// EXPECT r60 = 0x3c23100000000   dep R = r2, r3, 36, 14
// EXPECT r61 = 0x3fffff100000000   dep R = -1, r3, 36, 22
// EXPECT r62 = 0x105d1f08c0   shrp R = r2, r3, 58
// EXPECT r63 = 0x8c00000000000000   shl R = r2, 58
// EXPECT r64 = 0x0   shr.u R = r3, 58
// EXPECT r65 = 0x0   shr R = r3, 58
// EXPECT r66 = 0x40000000   extr.u R = r3, 2, 62
// EXPECT r67 = 0x40000000   extr R = r3, 2, 62
// EXPECT r68 = 0x105d1f08c   dep.z R = r2, 2, 62
// EXPECT r69 = 0xffffffffffffffc0   dep.z R = -16, 2, 62
// EXPECT r70 = 0x10000f08c   dep R = r2, r3, 2, 14
// EXPECT r71 = 0xfffffffffffffffc   dep R = -1, r3, 2, 62
// EXPECT r72 = 0x2300000001000000   shrp R = r2, r3, 8
// EXPECT r73 = 0x41747c2300   shl R = r2, 8
// EXPECT r74 = 0x1000000   shr.u R = r3, 8
// EXPECT r75 = 0x1000000   shr R = r3, 8
// EXPECT r76 = 0x0   extr.u R = r3, 16, 16
// EXPECT r77 = 0x0   extr R = r3, 16, 16
// EXPECT r78 = 0x7c230000   dep.z R = r2, 16, 16
// EXPECT r79 = 0xffaf0000   dep.z R = -81, 16, 16
// EXPECT r80 = 0x100230000   dep R = r2, r3, 16, 8
// EXPECT r81 = 0x100000000   dep R = 0, r3, 16, 16
// EXPECT r82 = 0x3e11800000008000   shrp R = r2, r3, 17
// EXPECT r83 = 0x82e8f8460000   shl R = r2, 17
// EXPECT r84 = 0x8000   shr.u R = r3, 17
// EXPECT r85 = 0x8000   shr R = r3, 17
// EXPECT r86 = 0x10   extr.u R = r3, 28, 19
// EXPECT r87 = 0x10   extr R = r3, 28, 19
// EXPECT r88 = 0x47c230000000   dep.z R = r2, 28, 19
// EXPECT r89 = 0x3f0000000   dep.z R = 63, 28, 19
// EXPECT r90 = 0x230000000   dep R = r2, r3, 28, 7
// EXPECT r91 = 0x0   dep R = 0, r3, 28, 19
// EXPECT r92 = 0x20ba3e1180   shrp R = r2, r3, 57
// EXPECT r93 = 0x4600000000000000   shl R = r2, 57
// EXPECT r94 = 0x0   shr.u R = r3, 57
// EXPECT r95 = 0x0   shr R = r3, 57
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0x0   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x0   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 0   cmp4.gt P1, P2 = -11, r3
// EXPECT p7 = 1
// EXPECT p8 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p9 = 0
// EXPECT p10 = 1   tbit.z P1, P2 = r3, 4
// EXPECT p11 = 0
// EXPECT p12 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp.ge P1, P2 = 101, r3
// EXPECT p15 = 1
// EXPECT p16 = 1   cmp.leu P1, P2 = 106, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   cmp4.geu P1, P2 = 4, r3
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 1   cmp.lt P1, P2 = 57, r3
// EXPECT p23 = 0
// EXPECT p24 = 1   cmp.ltu P1, P2 = r2, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp.le P1, P2 = 126, r3
// EXPECT p29 = 0
// EXPECT p30 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p31 = 0
// EXPECT p32 = 1   cmp4.ne P1, P2 = -72, r3
// EXPECT p33 = 0
// EXPECT p34 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p35 = 0
// EXPECT p36 = 1   tbit.z P1, P2 = r3, 33
// EXPECT p37 = 0
// EXPECT p38 = 1   tbit.z P1, P2 = r3, 14
// EXPECT p39 = 0
// EXPECT p40 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   tbit.nz P1, P2 = r3, 4
// EXPECT p43 = 1
// EXPECT p44 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   cmp4.leu P1, P2 = r2, r3
// EXPECT p47 = 1
// EXPECT p48 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   cmp4.gt P1, P2 = r2, r3
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp4.le P1, P2 = r2, r3
// EXPECT p53 = 1
// EXPECT p54 = 0   tbit.nz P1, P2 = r3, 14
// EXPECT p55 = 1
// EXPECT p56 = 0   cmp4.leu P1, P2 = -114, r3
// EXPECT p57 = 1
// EXPECT p58 = 0   cmp4.le P1, P2 = 34, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   cmp4.eq P1, P2 = 53, r3
// EXPECT p61 = 1
// EXPECT p62 = 0   cmp.geu P1, P2 = 64, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0x41747c23
  movl r3 = 0x100000000
  movl r4 = 0x1
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 78, r3
  and r13 = 78, r3
  or r14 = 78, r3
  xor r15 = 78, r3
  andcm r16 = 78, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -5920, r3
  addl r22 = -1031129, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -5920, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 5, 23
  extr r37 = r3, 5, 23
  dep.z r38 = r2, 5, 23
  dep.z r39 = -89, 5, 23
  dep r40 = r2, r3, 5, 8
  dep r41 = -1, r3, 5, 23
  shrp r42 = r2, r3, 57
  shl r43 = r2, 57
  shr.u r44 = r3, 57
  shr r45 = r3, 57
  extr.u r46 = r3, 55, 64
  extr r47 = r3, 55, 64
  dep.z r48 = r2, 55, 64
  dep.z r49 = 119, 55, 64
  dep r50 = r2, r3, 55, 15
  dep r51 = 0, r3, 55, 64
  shrp r52 = r2, r3, 63
  shl r53 = r2, 63
  shr.u r54 = r3, 63
  shr r55 = r3, 63
  extr.u r56 = r3, 36, 22
  extr r57 = r3, 36, 22
  dep.z r58 = r2, 36, 22
  dep.z r59 = -104, 36, 22
  dep r60 = r2, r3, 36, 14
  dep r61 = -1, r3, 36, 22
  shrp r62 = r2, r3, 58
  shl r63 = r2, 58
  shr.u r64 = r3, 58
  shr r65 = r3, 58
  extr.u r66 = r3, 2, 62
  extr r67 = r3, 2, 62
  dep.z r68 = r2, 2, 62
  dep.z r69 = -16, 2, 62
  dep r70 = r2, r3, 2, 14
  dep r71 = -1, r3, 2, 62
  shrp r72 = r2, r3, 8
  shl r73 = r2, 8
  shr.u r74 = r3, 8
  shr r75 = r3, 8
  extr.u r76 = r3, 16, 16
  extr r77 = r3, 16, 16
  dep.z r78 = r2, 16, 16
  dep.z r79 = -81, 16, 16
  dep r80 = r2, r3, 16, 8
  dep r81 = 0, r3, 16, 16
  shrp r82 = r2, r3, 17
  shl r83 = r2, 17
  shr.u r84 = r3, 17
  shr r85 = r3, 17
  extr.u r86 = r3, 28, 19
  extr r87 = r3, 28, 19
  dep.z r88 = r2, 28, 19
  dep.z r89 = 63, 28, 19
  dep r90 = r2, r3, 28, 7
  dep r91 = 0, r3, 28, 19
  shrp r92 = r2, r3, 57
  shl r93 = r2, 57
  shr.u r94 = r3, 57
  shr r95 = r3, 57
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.gt p6, p7 = -11, r3
  cmp.leu p8, p9 = r2, r3
  tbit.z p10, p11 = r3, 4
  cmp.ne p12, p13 = r2, r3
  cmp.ge p14, p15 = 101, r3
  cmp.leu p16, p17 = 106, r3
  cmp4.geu p18, p19 = 4, r3
  cmp4.eq p20, p21 = r2, r3
  cmp.lt p22, p23 = 57, r3
  cmp.ltu p24, p25 = r2, r3
  cmp4.ltu p26, p27 = r2, r3
  cmp.le p28, p29 = 126, r3
  cmp4.gtu p30, p31 = r2, r3
  cmp4.ne p32, p33 = -72, r3
  cmp.le p34, p35 = r2, r3
  tbit.z p36, p37 = r3, 33
  tbit.z p38, p39 = r3, 14
  cmp.geu p40, p41 = r2, r3
  tbit.nz p42, p43 = r3, 4
  cmp4.ne p44, p45 = r2, r3
  cmp4.leu p46, p47 = r2, r3
  cmp4.geu p48, p49 = r2, r3
  cmp4.gt p50, p51 = r2, r3
  cmp4.le p52, p53 = r2, r3
  tbit.nz p54, p55 = r3, 14
  cmp4.leu p56, p57 = -114, r3
  cmp4.le p58, p59 = 34, r3
  cmp4.eq p60, p61 = 53, r3
  cmp.geu p62, p63 = 64, r3
  ;;
  break.i 0x1ffffe
