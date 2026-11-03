/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x9535185700000000   add R = r2, r3
// EXPECT r9 = 0x9535185700000001   add R = r2, r3, 1
// EXPECT r10 = 0x6acae7a900000000   sub R = r2, r3
// EXPECT r11 = 0x6acae7a8ffffffff   sub R = r2, r3, 1
// EXPECT r12 = 0x6acae7a8fffffffe   sub R = -2, r3
// EXPECT r13 = 0x9535185700000000   and R = -2, r3
// EXPECT r14 = 0xfffffffffffffffe   or R = -2, r3
// EXPECT r15 = 0x6acae7a8fffffffe   xor R = -2, r3
// EXPECT r16 = 0x6acae7a8fffffffe   andcm R = -2, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x9535185700000000   or R = r2, r3
// EXPECT r19 = 0x9535185700000000   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0x95351856ffffe17e   adds R = -7810, r3
// EXPECT r22 = 0x10b0c3   addl R = 1093827, r1
// EXPECT r23 = 0x9535185700000000   shladd R = r2, 1, r3
// EXPECT r24 = 0x0   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x9535185700000000   shladd R = r2, 2, r3
// EXPECT r26 = 0x0   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x9535185700000000   shladd R = r2, 3, r3
// EXPECT r28 = 0x0   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x9535185700000000   shladd R = r2, 4, r3
// EXPECT r30 = 0x0   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x0   addp4 R = r2, r3
// EXPECT r32 = 0xffffe17e   addp4 R = -7810, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0xa   extr.u R = r3, 47, 4
// EXPECT r37 = 0xfffffffffffffffa   extr R = r3, 47, 4
// EXPECT r38 = 0x0   dep.z R = r2, 47, 4
// EXPECT r39 = 0x5000000000000   dep.z R = -54, 47, 4
// EXPECT r40 = 0x8000185700000000   dep R = r2, r3, 47, 16
// EXPECT r41 = 0x9530185700000000   dep R = 0, r3, 47, 4
// EXPECT r42 = 0x95351857   shrp R = r2, r3, 32
// EXPECT r43 = 0x0   shl R = r2, 32
// EXPECT r44 = 0x95351857   shr.u R = r3, 32
// EXPECT r45 = 0xffffffff95351857   shr R = r3, 32
// EXPECT r46 = 0x0   extr.u R = r3, 14, 17
// EXPECT r47 = 0x0   extr R = r3, 14, 17
// EXPECT r48 = 0x0   dep.z R = r2, 14, 17
// EXPECT r49 = 0x1a8000   dep.z R = 106, 14, 17
// EXPECT r50 = 0x9535185700000000   dep R = r2, r3, 14, 3
// EXPECT r51 = 0x9535185700000000   dep R = 0, r3, 14, 17
// EXPECT r52 = 0x254d   shrp R = r2, r3, 50
// EXPECT r53 = 0x0   shl R = r2, 50
// EXPECT r54 = 0x254d   shr.u R = r3, 50
// EXPECT r55 = 0xffffffffffffe54d   shr R = r3, 50
// EXPECT r56 = 0xa9a8c2   extr.u R = r3, 37, 24
// EXPECT r57 = 0xffffffffffa9a8c2   extr R = r3, 37, 24
// EXPECT r58 = 0x0   dep.z R = r2, 37, 24
// EXPECT r59 = 0x6c000000000   dep.z R = 54, 37, 24
// EXPECT r60 = 0x9530001700000000   dep R = r2, r3, 37, 15
// EXPECT r61 = 0x9ffffff700000000   dep R = -1, r3, 37, 24
// EXPECT r62 = 0x2   shrp R = r2, r3, 62
// EXPECT r63 = 0x0   shl R = r2, 62
// EXPECT r64 = 0x2   shr.u R = r3, 62
// EXPECT r65 = 0xfffffffffffffffe   shr R = r3, 62
// EXPECT r66 = 0x9535185   extr.u R = r3, 36, 34
// EXPECT r67 = 0xfffffffff9535185   extr R = r3, 36, 34
// EXPECT r68 = 0x0   dep.z R = r2, 36, 34
// EXPECT r69 = 0xfffffdb000000000   dep.z R = -37, 36, 34
// EXPECT r70 = 0x9534000700000000   dep R = r2, r3, 36, 14
// EXPECT r71 = 0xfffffff700000000   dep R = -1, r3, 36, 34
// EXPECT r72 = 0x95351857000   shrp R = r2, r3, 20
// EXPECT r73 = 0x0   shl R = r2, 20
// EXPECT r74 = 0x95351857000   shr.u R = r3, 20
// EXPECT r75 = 0xfffff95351857000   shr R = r3, 20
// EXPECT r76 = 0x26a   extr.u R = r3, 47, 11
// EXPECT r77 = 0x26a   extr R = r3, 47, 11
// EXPECT r78 = 0x0   dep.z R = r2, 47, 11
// EXPECT r79 = 0x3fd000000000000   dep.z R = -6, 47, 11
// EXPECT r80 = 0x9500185700000000   dep R = r2, r3, 47, 8
// EXPECT r81 = 0x97ff985700000000   dep R = -1, r3, 47, 11
// EXPECT r82 = 0x12a6a30ae0000000   shrp R = r2, r3, 3
// EXPECT r83 = 0x0   shl R = r2, 3
// EXPECT r84 = 0x12a6a30ae0000000   shr.u R = r3, 3
// EXPECT r85 = 0xf2a6a30ae0000000   shr R = r3, 3
// EXPECT r86 = 0x254d4615c000   extr.u R = r3, 18, 55
// EXPECT r87 = 0xffffe54d4615c000   extr R = r3, 18, 55
// EXPECT r88 = 0x0   dep.z R = r2, 18, 55
// EXPECT r89 = 0xffffffffffe80000   dep.z R = -6, 18, 55
// EXPECT r90 = 0x9535185700000000   dep R = r2, r3, 18, 12
// EXPECT r91 = 0x0   dep R = 0, r3, 18, 55
// EXPECT r92 = 0x12a6a30ae0000000   shrp R = r2, r3, 3
// EXPECT r93 = 0x0   shl R = r2, 3
// EXPECT r94 = 0x12a6a30ae0000000   shr.u R = r3, 3
// EXPECT r95 = 0xf2a6a30ae0000000   shr R = r3, 3
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0x0   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x0   zxt4 R = r3
// EXPECT r102 = 0xf   popcnt R = r3
// EXPECT p6 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p7 = 0
// EXPECT p8 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p9 = 1
// EXPECT p10 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p11 = 1
// EXPECT p12 = 1   cmp.leu P1, P2 = 98, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp.eq P1, P2 = 49, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp4.gt P1, P2 = -112, r3
// EXPECT p17 = 1
// EXPECT p18 = 0   cmp.geu P1, P2 = 98, r3
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp.gtu P1, P2 = -46, r3
// EXPECT p21 = 0
// EXPECT p22 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p25 = 0
// EXPECT p26 = 1   cmp.ltu P1, P2 = r2, r3
// EXPECT p27 = 0
// EXPECT p28 = 0   tbit.nz P1, P2 = r3, 59
// EXPECT p29 = 1
// EXPECT p30 = 1   cmp4.gtu P1, P2 = 46, r3
// EXPECT p31 = 0
// EXPECT p32 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p33 = 1
// EXPECT p34 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p35 = 0
// EXPECT p36 = 1   cmp4.geu P1, P2 = -115, r3
// EXPECT p37 = 0
// EXPECT p38 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp4.ne P1, P2 = r2, r3
// EXPECT p41 = 1
// EXPECT p42 = 1   cmp4.eq P1, P2 = r2, r3
// EXPECT p43 = 0
// EXPECT p44 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p45 = 1
// EXPECT p46 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p47 = 1
// EXPECT p48 = 1   cmp.gt P1, P2 = -95, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   tbit.nz P1, P2 = r3, 36
// EXPECT p51 = 0
// EXPECT p52 = 1   tbit.z P1, P2 = r3, 26
// EXPECT p53 = 0
// EXPECT p54 = 0   cmp4.le P1, P2 = 7, r3
// EXPECT p55 = 1
// EXPECT p56 = 0   tbit.nz P1, P2 = r3, 10
// EXPECT p57 = 1
// EXPECT p58 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p59 = 1
// EXPECT p60 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p61 = 0
// EXPECT p62 = 0   cmp.lt P1, P2 = -15, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0x0
  movl r3 = 0x9535185700000000
  movl r4 = 0x41
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -2, r3
  and r13 = -2, r3
  or r14 = -2, r3
  xor r15 = -2, r3
  andcm r16 = -2, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -7810, r3
  addl r22 = 1093827, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -7810, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 47, 4
  extr r37 = r3, 47, 4
  dep.z r38 = r2, 47, 4
  dep.z r39 = -54, 47, 4
  dep r40 = r2, r3, 47, 16
  dep r41 = 0, r3, 47, 4
  shrp r42 = r2, r3, 32
  shl r43 = r2, 32
  shr.u r44 = r3, 32
  shr r45 = r3, 32
  extr.u r46 = r3, 14, 17
  extr r47 = r3, 14, 17
  dep.z r48 = r2, 14, 17
  dep.z r49 = 106, 14, 17
  dep r50 = r2, r3, 14, 3
  dep r51 = 0, r3, 14, 17
  shrp r52 = r2, r3, 50
  shl r53 = r2, 50
  shr.u r54 = r3, 50
  shr r55 = r3, 50
  extr.u r56 = r3, 37, 24
  extr r57 = r3, 37, 24
  dep.z r58 = r2, 37, 24
  dep.z r59 = 54, 37, 24
  dep r60 = r2, r3, 37, 15
  dep r61 = -1, r3, 37, 24
  shrp r62 = r2, r3, 62
  shl r63 = r2, 62
  shr.u r64 = r3, 62
  shr r65 = r3, 62
  extr.u r66 = r3, 36, 34
  extr r67 = r3, 36, 34
  dep.z r68 = r2, 36, 34
  dep.z r69 = -37, 36, 34
  dep r70 = r2, r3, 36, 14
  dep r71 = -1, r3, 36, 34
  shrp r72 = r2, r3, 20
  shl r73 = r2, 20
  shr.u r74 = r3, 20
  shr r75 = r3, 20
  extr.u r76 = r3, 47, 11
  extr r77 = r3, 47, 11
  dep.z r78 = r2, 47, 11
  dep.z r79 = -6, 47, 11
  dep r80 = r2, r3, 47, 8
  dep r81 = -1, r3, 47, 11
  shrp r82 = r2, r3, 3
  shl r83 = r2, 3
  shr.u r84 = r3, 3
  shr r85 = r3, 3
  extr.u r86 = r3, 18, 55
  extr r87 = r3, 18, 55
  dep.z r88 = r2, 18, 55
  dep.z r89 = -6, 18, 55
  dep r90 = r2, r3, 18, 12
  dep r91 = 0, r3, 18, 55
  shrp r92 = r2, r3, 3
  shl r93 = r2, 3
  shr.u r94 = r3, 3
  shr r95 = r3, 3
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.ge p6, p7 = r2, r3
  cmp.le p8, p9 = r2, r3
  cmp4.ltu p10, p11 = r2, r3
  cmp.leu p12, p13 = 98, r3
  cmp.eq p14, p15 = 49, r3
  cmp4.gt p16, p17 = -112, r3
  cmp.geu p18, p19 = 98, r3
  cmp.gtu p20, p21 = -46, r3
  cmp4.lt p22, p23 = r2, r3
  cmp.ne p24, p25 = r2, r3
  cmp.ltu p26, p27 = r2, r3
  tbit.nz p28, p29 = r3, 59
  cmp4.gtu p30, p31 = 46, r3
  cmp.lt p32, p33 = r2, r3
  cmp4.le p34, p35 = r2, r3
  cmp4.geu p36, p37 = -115, r3
  cmp.geu p38, p39 = r2, r3
  cmp4.ne p40, p41 = r2, r3
  cmp4.eq p42, p43 = r2, r3
  cmp.eq p44, p45 = r2, r3
  cmp4.gt p46, p47 = r2, r3
  cmp.gt p48, p49 = -95, r3
  tbit.nz p50, p51 = r3, 36
  tbit.z p52, p53 = r3, 26
  cmp4.le p54, p55 = 7, r3
  tbit.nz p56, p57 = r3, 10
  cmp.gtu p58, p59 = r2, r3
  cmp.leu p60, p61 = r2, r3
  cmp.lt p62, p63 = -15, r3
  ;;
  break.i 0x1ffffe
