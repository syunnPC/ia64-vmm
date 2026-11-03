/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x180000000   add R = r2, r3
// EXPECT r9 = 0x180000001   add R = r2, r3, 1
// EXPECT r10 = 0xffffffff80000000   sub R = r2, r3
// EXPECT r11 = 0xffffffff7fffffff   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffff0000001b   sub R = 27, r3
// EXPECT r13 = 0x0   and R = 27, r3
// EXPECT r14 = 0x10000001b   or R = 27, r3
// EXPECT r15 = 0x10000001b   xor R = 27, r3
// EXPECT r16 = 0x1b   andcm R = 27, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x180000000   or R = r2, r3
// EXPECT r19 = 0x180000000   xor R = r2, r3
// EXPECT r20 = 0x80000000   andcm R = r2, r3
// EXPECT r21 = 0x100001275   adds R = 4725, r3
// EXPECT r22 = 0x11cc23   addl R = 1166371, r1
// EXPECT r23 = 0x200000000   shladd R = r2, 1, r3
// EXPECT r24 = 0x0   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x300000000   shladd R = r2, 2, r3
// EXPECT r26 = 0x0   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x500000000   shladd R = r2, 3, r3
// EXPECT r28 = 0x0   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x900000000   shladd R = r2, 4, r3
// EXPECT r30 = 0x0   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x80000000   addp4 R = r2, r3
// EXPECT r32 = 0x1275   addp4 R = 4725, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x200   extr.u R = r3, 23, 15
// EXPECT r37 = 0x200   extr R = r3, 23, 15
// EXPECT r38 = 0x0   dep.z R = r2, 23, 15
// EXPECT r39 = 0x3fc8000000   dep.z R = -112, 23, 15
// EXPECT r40 = 0x0   dep R = r2, r3, 23, 12
// EXPECT r41 = 0x0   dep R = 0, r3, 23, 15
// EXPECT r42 = 0x1000000   shrp R = r2, r3, 8
// EXPECT r43 = 0x8000000000   shl R = r2, 8
// EXPECT r44 = 0x1000000   shr.u R = r3, 8
// EXPECT r45 = 0x1000000   shr R = r3, 8
// EXPECT r46 = 0x0   extr.u R = r3, 19, 12
// EXPECT r47 = 0x0   extr R = r3, 19, 12
// EXPECT r48 = 0x0   dep.z R = r2, 19, 12
// EXPECT r49 = 0x7de80000   dep.z R = -67, 19, 12
// EXPECT r50 = 0x100000000   dep R = r2, r3, 19, 11
// EXPECT r51 = 0x100000000   dep R = 0, r3, 19, 12
// EXPECT r52 = 0x2000000   shrp R = r2, r3, 7
// EXPECT r53 = 0x4000000000   shl R = r2, 7
// EXPECT r54 = 0x2000000   shr.u R = r3, 7
// EXPECT r55 = 0x2000000   shr R = r3, 7
// EXPECT r56 = 0x0   extr.u R = r3, 52, 36
// EXPECT r57 = 0x0   extr R = r3, 52, 36
// EXPECT r58 = 0x0   dep.z R = r2, 52, 36
// EXPECT r59 = 0x320000000000000   dep.z R = 50, 52, 36
// EXPECT r60 = 0x100000000   dep R = r2, r3, 52, 7
// EXPECT r61 = 0x100000000   dep R = 0, r3, 52, 36
// EXPECT r62 = 0x400000000000   shrp R = r2, r3, 49
// EXPECT r63 = 0x0   shl R = r2, 49
// EXPECT r64 = 0x0   shr.u R = r3, 49
// EXPECT r65 = 0x0   shr R = r3, 49
// EXPECT r66 = 0x0   extr.u R = r3, 58, 61
// EXPECT r67 = 0x0   extr R = r3, 58, 61
// EXPECT r68 = 0x0   dep.z R = r2, 58, 61
// EXPECT r69 = 0xdc00000000000000   dep.z R = 55, 58, 61
// EXPECT r70 = 0x100000000   dep R = r2, r3, 58, 15
// EXPECT r71 = 0xfc00000100000000   dep R = -1, r3, 58, 61
// EXPECT r72 = 0x200000000   shrp R = r2, r3, 62
// EXPECT r73 = 0x0   shl R = r2, 62
// EXPECT r74 = 0x0   shr.u R = r3, 62
// EXPECT r75 = 0x0   shr R = r3, 62
// EXPECT r76 = 0x0   extr.u R = r3, 8, 2
// EXPECT r77 = 0x0   extr R = r3, 8, 2
// EXPECT r78 = 0x0   dep.z R = r2, 8, 2
// EXPECT r79 = 0x200   dep.z R = 118, 8, 2
// EXPECT r80 = 0x100000000   dep R = r2, r3, 8, 13
// EXPECT r81 = 0x100000300   dep R = -1, r3, 8, 2
// EXPECT r82 = 0x2000000000000   shrp R = r2, r3, 46
// EXPECT r83 = 0x0   shl R = r2, 46
// EXPECT r84 = 0x0   shr.u R = r3, 46
// EXPECT r85 = 0x0   shr R = r3, 46
// EXPECT r86 = 0x40000   extr.u R = r3, 14, 32
// EXPECT r87 = 0x40000   extr R = r3, 14, 32
// EXPECT r88 = 0x200000000000   dep.z R = r2, 14, 32
// EXPECT r89 = 0x3fffffe1c000   dep.z R = -121, 14, 32
// EXPECT r90 = 0x100000000   dep R = r2, r3, 14, 15
// EXPECT r91 = 0x3fffffffc000   dep R = -1, r3, 14, 32
// EXPECT r92 = 0x1000   shrp R = r2, r3, 20
// EXPECT r93 = 0x8000000000000   shl R = r2, 20
// EXPECT r94 = 0x1000   shr.u R = r3, 20
// EXPECT r95 = 0x1000   shr R = r3, 20
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0x0   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x0   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 0   cmp4.ltu P1, P2 = -118, r3
// EXPECT p7 = 1
// EXPECT p8 = 1   tbit.z P1, P2 = r3, 45
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp4.eq P1, P2 = 113, r3
// EXPECT p11 = 1
// EXPECT p12 = 1   tbit.z P1, P2 = r3, 40
// EXPECT p13 = 0
// EXPECT p14 = 1   cmp4.ne P1, P2 = 118, r3
// EXPECT p15 = 0
// EXPECT p16 = 0   cmp4.ge P1, P2 = -74, r3
// EXPECT p17 = 1
// EXPECT p18 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p21 = 0
// EXPECT p22 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p23 = 1
// EXPECT p24 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp4.leu P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp.lt P1, P2 = -111, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp.ge P1, P2 = 80, r3
// EXPECT p31 = 1
// EXPECT p32 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp.ltu P1, P2 = -84, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   tbit.nz P1, P2 = r3, 22
// EXPECT p39 = 1
// EXPECT p40 = 1   cmp.ltu P1, P2 = r2, r3
// EXPECT p41 = 0
// EXPECT p42 = 0   cmp.geu P1, P2 = 5, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   tbit.nz P1, P2 = r3, 40
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p47 = 0
// EXPECT p48 = 1   tbit.z P1, P2 = r3, 22
// EXPECT p49 = 0
// EXPECT p50 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p51 = 0
// EXPECT p52 = 1   cmp4.geu P1, P2 = -34, r3
// EXPECT p53 = 0
// EXPECT p54 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p55 = 1
// EXPECT p56 = 0   cmp.gt P1, P2 = 15, r3
// EXPECT p57 = 1
// EXPECT p58 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p59 = 1
// EXPECT p60 = 1   cmp.leu P1, P2 = 108, r3
// EXPECT p61 = 0
// EXPECT p62 = 0   cmp4.lt P1, P2 = 70, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0x80000000
  movl r3 = 0x100000000
  movl r4 = 0x40
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 27, r3
  and r13 = 27, r3
  or r14 = 27, r3
  xor r15 = 27, r3
  andcm r16 = 27, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 4725, r3
  addl r22 = 1166371, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 4725, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 23, 15
  extr r37 = r3, 23, 15
  dep.z r38 = r2, 23, 15
  dep.z r39 = -112, 23, 15
  dep r40 = r2, r3, 23, 12
  dep r41 = 0, r3, 23, 15
  shrp r42 = r2, r3, 8
  shl r43 = r2, 8
  shr.u r44 = r3, 8
  shr r45 = r3, 8
  extr.u r46 = r3, 19, 12
  extr r47 = r3, 19, 12
  dep.z r48 = r2, 19, 12
  dep.z r49 = -67, 19, 12
  dep r50 = r2, r3, 19, 11
  dep r51 = 0, r3, 19, 12
  shrp r52 = r2, r3, 7
  shl r53 = r2, 7
  shr.u r54 = r3, 7
  shr r55 = r3, 7
  extr.u r56 = r3, 52, 36
  extr r57 = r3, 52, 36
  dep.z r58 = r2, 52, 36
  dep.z r59 = 50, 52, 36
  dep r60 = r2, r3, 52, 7
  dep r61 = 0, r3, 52, 36
  shrp r62 = r2, r3, 49
  shl r63 = r2, 49
  shr.u r64 = r3, 49
  shr r65 = r3, 49
  extr.u r66 = r3, 58, 61
  extr r67 = r3, 58, 61
  dep.z r68 = r2, 58, 61
  dep.z r69 = 55, 58, 61
  dep r70 = r2, r3, 58, 15
  dep r71 = -1, r3, 58, 61
  shrp r72 = r2, r3, 62
  shl r73 = r2, 62
  shr.u r74 = r3, 62
  shr r75 = r3, 62
  extr.u r76 = r3, 8, 2
  extr r77 = r3, 8, 2
  dep.z r78 = r2, 8, 2
  dep.z r79 = 118, 8, 2
  dep r80 = r2, r3, 8, 13
  dep r81 = -1, r3, 8, 2
  shrp r82 = r2, r3, 46
  shl r83 = r2, 46
  shr.u r84 = r3, 46
  shr r85 = r3, 46
  extr.u r86 = r3, 14, 32
  extr r87 = r3, 14, 32
  dep.z r88 = r2, 14, 32
  dep.z r89 = -121, 14, 32
  dep r90 = r2, r3, 14, 15
  dep r91 = -1, r3, 14, 32
  shrp r92 = r2, r3, 20
  shl r93 = r2, 20
  shr.u r94 = r3, 20
  shr r95 = r3, 20
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.ltu p6, p7 = -118, r3
  tbit.z p8, p9 = r3, 45
  cmp4.eq p10, p11 = 113, r3
  tbit.z p12, p13 = r3, 40
  cmp4.ne p14, p15 = 118, r3
  cmp4.ge p16, p17 = -74, r3
  cmp4.gt p18, p19 = r2, r3
  cmp.ne p20, p21 = r2, r3
  cmp.eq p22, p23 = r2, r3
  cmp.gtu p24, p25 = r2, r3
  cmp4.leu p26, p27 = r2, r3
  cmp.lt p28, p29 = -111, r3
  cmp.ge p30, p31 = 80, r3
  cmp4.ge p32, p33 = r2, r3
  cmp.ltu p34, p35 = -84, r3
  cmp.gt p36, p37 = r2, r3
  tbit.nz p38, p39 = r3, 22
  cmp.ltu p40, p41 = r2, r3
  cmp.geu p42, p43 = 5, r3
  tbit.nz p44, p45 = r3, 40
  cmp4.le p46, p47 = r2, r3
  tbit.z p48, p49 = r3, 22
  cmp4.lt p50, p51 = r2, r3
  cmp4.geu p52, p53 = -34, r3
  cmp.geu p54, p55 = r2, r3
  cmp.gt p56, p57 = 15, r3
  cmp.ge p58, p59 = r2, r3
  cmp.leu p60, p61 = 108, r3
  cmp4.lt p62, p63 = 70, r3
  ;;
  break.i 0x1ffffe
