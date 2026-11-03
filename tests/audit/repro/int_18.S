/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x81fb589300000000   add R = r2, r3
// EXPECT r9 = 0x81fb589300000001   add R = r2, r3, 1
// EXPECT r10 = 0x7e04a76e00000000   sub R = r2, r3
// EXPECT r11 = 0x7e04a76dffffffff   sub R = r2, r3, 1
// EXPECT r12 = 0x7e04a76d7fffff83   sub R = -125, r3
// EXPECT r13 = 0x81fb589280000000   and R = -125, r3
// EXPECT r14 = 0xffffffffffffff83   or R = -125, r3
// EXPECT r15 = 0x7e04a76d7fffff83   xor R = -125, r3
// EXPECT r16 = 0x7e04a76d7fffff83   andcm R = -125, r3
// EXPECT r17 = 0x80000000   and R = r2, r3
// EXPECT r18 = 0x81fb589280000000   or R = r2, r3
// EXPECT r19 = 0x81fb589200000000   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0x81fb589280000bdf   adds R = 3039, r3
// EXPECT r22 = 0x13e3f7   addl R = 1303543, r1
// EXPECT r23 = 0x81fb589380000000   shladd R = r2, 1, r3
// EXPECT r24 = 0x4000000080000000   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x81fb589480000000   shladd R = r2, 2, r3
// EXPECT r26 = 0x4000000080000000   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x81fb589680000000   shladd R = r2, 3, r3
// EXPECT r28 = 0x4000000080000000   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x81fb589a80000000   shladd R = r2, 4, r3
// EXPECT r30 = 0x4000000080000000   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x4000000000000000   addp4 R = r2, r3
// EXPECT r32 = 0x4000000080000bdf   addp4 R = 3039, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x1   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0x81fb58928   extr.u R = r3, 28, 52
// EXPECT r37 = 0xfffffff81fb58928   extr R = r3, 28, 52
// EXPECT r38 = 0x800000000000000   dep.z R = r2, 28, 52
// EXPECT r39 = 0x310000000   dep.z R = 49, 28, 52
// EXPECT r40 = 0x81fb588000000000   dep R = r2, r3, 28, 9
// EXPECT r41 = 0xfffffffff0000000   dep R = -1, r3, 28, 52
// EXPECT r42 = 0x20000000207ed62   shrp R = r2, r3, 38
// EXPECT r43 = 0x0   shl R = r2, 38
// EXPECT r44 = 0x207ed62   shr.u R = r3, 38
// EXPECT r45 = 0xfffffffffe07ed62   shr R = r3, 38
// EXPECT r46 = 0xd624a0000   extr.u R = r3, 14, 37
// EXPECT r47 = 0xd624a0000   extr R = r3, 14, 37
// EXPECT r48 = 0x200000000000   dep.z R = r2, 14, 37
// EXPECT r49 = 0x1d0000   dep.z R = 116, 14, 37
// EXPECT r50 = 0x81fb589280000000   dep R = r2, r3, 14, 5
// EXPECT r51 = 0x81ffffffffffc000   dep R = -1, r3, 14, 37
// EXPECT r52 = 0x81fb5892800   shrp R = r2, r3, 20
// EXPECT r53 = 0x8000000000000   shl R = r2, 20
// EXPECT r54 = 0x81fb5892800   shr.u R = r3, 20
// EXPECT r55 = 0xfffff81fb5892800   shr R = r3, 20
// EXPECT r56 = 0x3589280   extr.u R = r3, 24, 27
// EXPECT r57 = 0x3589280   extr R = r3, 24, 27
// EXPECT r58 = 0x0   dep.z R = r2, 24, 27
// EXPECT r59 = 0x5f000000   dep.z R = 95, 24, 27
// EXPECT r60 = 0x81fb589280000000   dep R = r2, r3, 24, 5
// EXPECT r61 = 0x81ffffffff000000   dep R = -1, r3, 24, 27
// EXPECT r62 = 0x400000004   shrp R = r2, r3, 61
// EXPECT r63 = 0x0   shl R = r2, 61
// EXPECT r64 = 0x4   shr.u R = r3, 61
// EXPECT r65 = 0xfffffffffffffffc   shr R = r3, 61
// EXPECT r66 = 0x1280000   extr.u R = r3, 12, 26
// EXPECT r67 = 0x1280000   extr R = r3, 12, 26
// EXPECT r68 = 0x0   dep.z R = r2, 12, 26
// EXPECT r69 = 0x4000   dep.z R = 4, 12, 26
// EXPECT r70 = 0x81fb589280000000   dep R = r2, r3, 12, 11
// EXPECT r71 = 0x81fb58bffffff000   dep R = -1, r3, 12, 26
// EXPECT r72 = 0x103f6b12500000   shrp R = r2, r3, 11
// EXPECT r73 = 0x40000000000   shl R = r2, 11
// EXPECT r74 = 0x103f6b12500000   shr.u R = r3, 11
// EXPECT r75 = 0xfff03f6b12500000   shr R = r3, 11
// EXPECT r76 = 0x2   extr.u R = r3, 62, 19
// EXPECT r77 = 0xfffffffffffffffe   extr R = r3, 62, 19
// EXPECT r78 = 0x0   dep.z R = r2, 62, 19
// EXPECT r79 = 0x0   dep.z R = 116, 62, 19
// EXPECT r80 = 0x1fb589280000000   dep R = r2, r3, 62, 5
// EXPECT r81 = 0xc1fb589280000000   dep R = -1, r3, 62, 19
// EXPECT r82 = 0x207ed624a00000   shrp R = r2, r3, 10
// EXPECT r83 = 0x20000000000   shl R = r2, 10
// EXPECT r84 = 0x207ed624a00000   shr.u R = r3, 10
// EXPECT r85 = 0xffe07ed624a00000   shr R = r3, 10
// EXPECT r86 = 0x125   extr.u R = r3, 31, 12
// EXPECT r87 = 0x125   extr R = r3, 31, 12
// EXPECT r88 = 0x0   dep.z R = r2, 31, 12
// EXPECT r89 = 0x1a00000000   dep.z R = 52, 31, 12
// EXPECT r90 = 0x81fb588000000000   dep R = r2, r3, 31, 7
// EXPECT r91 = 0x81fb5fff80000000   dep R = -1, r3, 31, 12
// EXPECT r92 = 0x81fb589280000000   shrp R = r2, r3, 0
// EXPECT r93 = 0x80000000   shl R = r2, 0
// EXPECT r94 = 0x81fb589280000000   shr.u R = r3, 0
// EXPECT r95 = 0x81fb589280000000   shr R = r3, 0
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0xffffffff80000000   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x80000000   zxt4 R = r3
// EXPECT r102 = 0x10   popcnt R = r3
// EXPECT p6 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p7 = 1
// EXPECT p8 = 0   cmp4.gtu P1, P2 = 94, r3
// EXPECT p9 = 1
// EXPECT p10 = 0   tbit.nz P1, P2 = r3, 0
// EXPECT p11 = 1
// EXPECT p12 = 0   cmp4.le P1, P2 = 45, r3
// EXPECT p13 = 1
// EXPECT p14 = 0   tbit.z P1, P2 = r3, 54
// EXPECT p15 = 1
// EXPECT p16 = 1   cmp.ltu P1, P2 = r2, r3
// EXPECT p17 = 0
// EXPECT p18 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p21 = 0
// EXPECT p22 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p23 = 0
// EXPECT p24 = 1   cmp4.ltu P1, P2 = 43, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp4.lt P1, P2 = -55, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 1   tbit.z P1, P2 = r3, 20
// EXPECT p31 = 0
// EXPECT p32 = 1   cmp4.ge P1, P2 = r2, r3
// EXPECT p33 = 0
// EXPECT p34 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   cmp.le P1, P2 = 79, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp.lt P1, P2 = 116, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   tbit.nz P1, P2 = r3, 18
// EXPECT p43 = 1
// EXPECT p44 = 1   cmp4.ge P1, P2 = -119, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p47 = 1
// EXPECT p48 = 0   cmp.leu P1, P2 = -36, r3
// EXPECT p49 = 1
// EXPECT p50 = 1   cmp4.ne P1, P2 = 56, r3
// EXPECT p51 = 0
// EXPECT p52 = 1   cmp4.eq P1, P2 = r2, r3
// EXPECT p53 = 0
// EXPECT p54 = 1   tbit.z P1, P2 = r3, 0
// EXPECT p55 = 0
// EXPECT p56 = 0   cmp.gtu P1, P2 = 85, r3
// EXPECT p57 = 1
// EXPECT p58 = 0   cmp.eq P1, P2 = -60, r3
// EXPECT p59 = 1
// EXPECT p60 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p61 = 0
// EXPECT p62 = 1   cmp4.gt P1, P2 = -64, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x80000000
  movl r3 = 0x81fb589280000000
  movl r4 = 0x3f
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -125, r3
  and r13 = -125, r3
  or r14 = -125, r3
  xor r15 = -125, r3
  andcm r16 = -125, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 3039, r3
  addl r22 = 1303543, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 3039, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 28, 52
  extr r37 = r3, 28, 52
  dep.z r38 = r2, 28, 52
  dep.z r39 = 49, 28, 52
  dep r40 = r2, r3, 28, 9
  dep r41 = -1, r3, 28, 52
  shrp r42 = r2, r3, 38
  shl r43 = r2, 38
  shr.u r44 = r3, 38
  shr r45 = r3, 38
  extr.u r46 = r3, 14, 37
  extr r47 = r3, 14, 37
  dep.z r48 = r2, 14, 37
  dep.z r49 = 116, 14, 37
  dep r50 = r2, r3, 14, 5
  dep r51 = -1, r3, 14, 37
  shrp r52 = r2, r3, 20
  shl r53 = r2, 20
  shr.u r54 = r3, 20
  shr r55 = r3, 20
  extr.u r56 = r3, 24, 27
  extr r57 = r3, 24, 27
  dep.z r58 = r2, 24, 27
  dep.z r59 = 95, 24, 27
  dep r60 = r2, r3, 24, 5
  dep r61 = -1, r3, 24, 27
  shrp r62 = r2, r3, 61
  shl r63 = r2, 61
  shr.u r64 = r3, 61
  shr r65 = r3, 61
  extr.u r66 = r3, 12, 26
  extr r67 = r3, 12, 26
  dep.z r68 = r2, 12, 26
  dep.z r69 = 4, 12, 26
  dep r70 = r2, r3, 12, 11
  dep r71 = -1, r3, 12, 26
  shrp r72 = r2, r3, 11
  shl r73 = r2, 11
  shr.u r74 = r3, 11
  shr r75 = r3, 11
  extr.u r76 = r3, 62, 19
  extr r77 = r3, 62, 19
  dep.z r78 = r2, 62, 19
  dep.z r79 = 116, 62, 19
  dep r80 = r2, r3, 62, 5
  dep r81 = -1, r3, 62, 19
  shrp r82 = r2, r3, 10
  shl r83 = r2, 10
  shr.u r84 = r3, 10
  shr r85 = r3, 10
  extr.u r86 = r3, 31, 12
  extr r87 = r3, 31, 12
  dep.z r88 = r2, 31, 12
  dep.z r89 = 52, 31, 12
  dep r90 = r2, r3, 31, 7
  dep r91 = -1, r3, 31, 12
  shrp r92 = r2, r3, 0
  shl r93 = r2, 0
  shr.u r94 = r3, 0
  shr r95 = r3, 0
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.gt p6, p7 = r2, r3
  cmp4.gtu p8, p9 = 94, r3
  tbit.nz p10, p11 = r3, 0
  cmp4.le p12, p13 = 45, r3
  tbit.z p14, p15 = r3, 54
  cmp.ltu p16, p17 = r2, r3
  cmp.eq p18, p19 = r2, r3
  cmp4.geu p20, p21 = r2, r3
  cmp.ge p22, p23 = r2, r3
  cmp4.ltu p24, p25 = 43, r3
  cmp4.lt p26, p27 = -55, r3
  cmp4.leu p28, p29 = r2, r3
  tbit.z p30, p31 = r3, 20
  cmp4.ge p32, p33 = r2, r3
  cmp4.ltu p34, p35 = r2, r3
  cmp.le p36, p37 = 79, r3
  cmp.gtu p38, p39 = r2, r3
  cmp.lt p40, p41 = 116, r3
  tbit.nz p42, p43 = r3, 18
  cmp4.ge p44, p45 = -119, r3
  cmp4.gtu p46, p47 = r2, r3
  cmp.leu p48, p49 = -36, r3
  cmp4.ne p50, p51 = 56, r3
  cmp4.eq p52, p53 = r2, r3
  tbit.z p54, p55 = r3, 0
  cmp.gtu p56, p57 = 85, r3
  cmp.eq p58, p59 = -60, r3
  cmp.ne p60, p61 = r2, r3
  cmp4.gt p62, p63 = -64, r3
  ;;
  break.i 0x1ffffe
