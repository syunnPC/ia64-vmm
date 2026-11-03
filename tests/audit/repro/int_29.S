/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x6a5dcf79   add R = r2, r3
// EXPECT r9 = 0x6a5dcf7a   add R = r2, r3, 1
// EXPECT r10 = 0x6a5dcf75   sub R = r2, r3
// EXPECT r11 = 0x6a5dcf74   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffffffffffe2   sub R = -28, r3
// EXPECT r13 = 0x0   and R = -28, r3
// EXPECT r14 = 0xffffffffffffffe6   or R = -28, r3
// EXPECT r15 = 0xffffffffffffffe6   xor R = -28, r3
// EXPECT r16 = 0xffffffffffffffe4   andcm R = -28, r3
// EXPECT r17 = 0x2   and R = r2, r3
// EXPECT r18 = 0x6a5dcf77   or R = r2, r3
// EXPECT r19 = 0x6a5dcf75   xor R = r2, r3
// EXPECT r20 = 0x6a5dcf75   andcm R = r2, r3
// EXPECT r21 = 0x1614   adds R = 5650, r3
// EXPECT r22 = 0x6312a   addl R = 405802, r1
// EXPECT r23 = 0xd4bb9ef0   shladd R = r2, 1, r3
// EXPECT r24 = 0xd4bb9ef0   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x1a9773dde   shladd R = r2, 2, r3
// EXPECT r26 = 0xa9773dde   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x352ee7bba   shladd R = r2, 3, r3
// EXPECT r28 = 0x52ee7bba   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x6a5dcf772   shladd R = r2, 4, r3
// EXPECT r30 = 0xa5dcf772   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x6a5dcf79   addp4 R = r2, r3
// EXPECT r32 = 0x1614   addp4 R = 5650, r3
// EXPECT r33 = 0x6a5dcf770000000   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 35, 29
// EXPECT r37 = 0x0   extr R = r3, 35, 29
// EXPECT r38 = 0x52ee7bb800000000   dep.z R = r2, 35, 29
// EXPECT r39 = 0x29000000000   dep.z R = 82, 35, 29
// EXPECT r40 = 0x67bb800000002   dep R = r2, r3, 35, 16
// EXPECT r41 = 0x2   dep R = 0, r3, 35, 29
// EXPECT r42 = 0xd4bb9eee000000   shrp R = r2, r3, 39
// EXPECT r43 = 0x2ee7bb8000000000   shl R = r2, 39
// EXPECT r44 = 0x0   shr.u R = r3, 39
// EXPECT r45 = 0x0   shr R = r3, 39
// EXPECT r46 = 0x0   extr.u R = r3, 38, 61
// EXPECT r47 = 0x0   extr R = r3, 38, 61
// EXPECT r48 = 0x9773ddc000000000   dep.z R = r2, 38, 61
// EXPECT r49 = 0x12c000000000   dep.z R = 75, 38, 61
// EXPECT r50 = 0x1c000000002   dep R = r2, r3, 38, 4
// EXPECT r51 = 0xffffffc000000002   dep R = -1, r3, 38, 61
// EXPECT r52 = 0xe7bb800000000000   shrp R = r2, r3, 17
// EXPECT r53 = 0xd4bb9eee0000   shl R = r2, 17
// EXPECT r54 = 0x0   shr.u R = r3, 17
// EXPECT r55 = 0x0   shr R = r3, 17
// EXPECT r56 = 0x0   extr.u R = r3, 33, 20
// EXPECT r57 = 0x0   extr R = r3, 33, 20
// EXPECT r58 = 0x1b9eee00000000   dep.z R = r2, 33, 20
// EXPECT r59 = 0x5c00000000   dep.z R = 46, 33, 20
// EXPECT r60 = 0xe00000002   dep R = r2, r3, 33, 3
// EXPECT r61 = 0x1ffffe00000002   dep R = -1, r3, 33, 20
// EXPECT r62 = 0xdcf7700000000000   shrp R = r2, r3, 20
// EXPECT r63 = 0x6a5dcf7700000   shl R = r2, 20
// EXPECT r64 = 0x0   shr.u R = r3, 20
// EXPECT r65 = 0x0   shr R = r3, 20
// EXPECT r66 = 0x0   extr.u R = r3, 53, 55
// EXPECT r67 = 0x0   extr R = r3, 53, 55
// EXPECT r68 = 0xeee0000000000000   dep.z R = r2, 53, 55
// EXPECT r69 = 0xdc0000000000000   dep.z R = 110, 53, 55
// EXPECT r70 = 0x6ee0000000000002   dep R = r2, r3, 53, 10
// EXPECT r71 = 0x2   dep R = 0, r3, 53, 55
// EXPECT r72 = 0x352ee7bb800000   shrp R = r2, r3, 41
// EXPECT r73 = 0xbb9eee0000000000   shl R = r2, 41
// EXPECT r74 = 0x0   shr.u R = r3, 41
// EXPECT r75 = 0x0   shr R = r3, 41
// EXPECT r76 = 0x0   extr.u R = r3, 26, 36
// EXPECT r77 = 0x0   extr R = r3, 26, 36
// EXPECT r78 = 0x1a9773ddc000000   dep.z R = r2, 26, 36
// EXPECT r79 = 0x3ffffffe1c000000   dep.z R = -121, 26, 36
// EXPECT r80 = 0x4000002   dep R = r2, r3, 26, 1
// EXPECT r81 = 0x3ffffffffc000002   dep R = -1, r3, 26, 36
// EXPECT r82 = 0x352ee7bb800   shrp R = r2, r3, 53
// EXPECT r83 = 0xeee0000000000000   shl R = r2, 53
// EXPECT r84 = 0x0   shr.u R = r3, 53
// EXPECT r85 = 0x0   shr R = r3, 53
// EXPECT r86 = 0x0   extr.u R = r3, 17, 56
// EXPECT r87 = 0x0   extr R = r3, 17, 56
// EXPECT r88 = 0xd4bb9eee0000   dep.z R = r2, 17, 56
// EXPECT r89 = 0xffffffffff9a0000   dep.z R = -51, 17, 56
// EXPECT r90 = 0x6ee0002   dep R = r2, r3, 17, 10
// EXPECT r91 = 0x2   dep R = 0, r3, 17, 56
// EXPECT r92 = 0xdc00000000000000   shrp R = r2, r3, 6
// EXPECT r93 = 0x1a9773ddc0   shl R = r2, 6
// EXPECT r94 = 0x0   shr.u R = r3, 6
// EXPECT r95 = 0x0   shr R = r3, 6
// EXPECT r96 = 0x2   sxt1 R = r3
// EXPECT r97 = 0x2   sxt2 R = r3
// EXPECT r98 = 0x2   sxt4 R = r3
// EXPECT r99 = 0x2   zxt1 R = r3
// EXPECT r100 = 0x2   zxt2 R = r3
// EXPECT r101 = 0x2   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p7 = 0
// EXPECT p8 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp.gt P1, P2 = -124, r3
// EXPECT p11 = 1
// EXPECT p12 = 0   tbit.nz P1, P2 = r3, 14
// EXPECT p13 = 1
// EXPECT p14 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp.gtu P1, P2 = -51, r3
// EXPECT p17 = 0
// EXPECT p18 = 0   cmp4.eq P1, P2 = 61, r3
// EXPECT p19 = 1
// EXPECT p20 = 1   tbit.z P1, P2 = r3, 48
// EXPECT p21 = 0
// EXPECT p22 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p23 = 1
// EXPECT p24 = 0   cmp.ltu P1, P2 = -125, r3
// EXPECT p25 = 1
// EXPECT p26 = 1   cmp4.ge P1, P2 = r2, r3
// EXPECT p27 = 0
// EXPECT p28 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p31 = 1
// EXPECT p32 = 0   tbit.nz P1, P2 = r3, 4
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p35 = 1
// EXPECT p36 = 1   cmp.ne P1, P2 = 29, r3
// EXPECT p37 = 0
// EXPECT p38 = 1   cmp4.gtu P1, P2 = -117, r3
// EXPECT p39 = 0
// EXPECT p40 = 1   tbit.z P1, P2 = r3, 4
// EXPECT p41 = 0
// EXPECT p42 = 1   cmp.le P1, P2 = -99, r3
// EXPECT p43 = 0
// EXPECT p44 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 1   cmp.lt P1, P2 = -67, r3
// EXPECT p47 = 0
// EXPECT p48 = 1   cmp4.le P1, P2 = -30, r3
// EXPECT p49 = 0
// EXPECT p50 = 0   cmp4.leu P1, P2 = r2, r3
// EXPECT p51 = 1
// EXPECT p52 = 1   tbit.z P1, P2 = r3, 14
// EXPECT p53 = 0
// EXPECT p54 = 1   cmp4.gt P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 1   cmp4.lt P1, P2 = -95, r3
// EXPECT p57 = 0
// EXPECT p58 = 0   cmp.eq P1, P2 = -108, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   tbit.nz P1, P2 = r3, 48
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x6a5dcf77
  movl r3 = 0x2
  movl r4 = 0x1c
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -28, r3
  and r13 = -28, r3
  or r14 = -28, r3
  xor r15 = -28, r3
  andcm r16 = -28, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 5650, r3
  addl r22 = 405802, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 5650, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 35, 29
  extr r37 = r3, 35, 29
  dep.z r38 = r2, 35, 29
  dep.z r39 = 82, 35, 29
  dep r40 = r2, r3, 35, 16
  dep r41 = 0, r3, 35, 29
  shrp r42 = r2, r3, 39
  shl r43 = r2, 39
  shr.u r44 = r3, 39
  shr r45 = r3, 39
  extr.u r46 = r3, 38, 61
  extr r47 = r3, 38, 61
  dep.z r48 = r2, 38, 61
  dep.z r49 = 75, 38, 61
  dep r50 = r2, r3, 38, 4
  dep r51 = -1, r3, 38, 61
  shrp r52 = r2, r3, 17
  shl r53 = r2, 17
  shr.u r54 = r3, 17
  shr r55 = r3, 17
  extr.u r56 = r3, 33, 20
  extr r57 = r3, 33, 20
  dep.z r58 = r2, 33, 20
  dep.z r59 = 46, 33, 20
  dep r60 = r2, r3, 33, 3
  dep r61 = -1, r3, 33, 20
  shrp r62 = r2, r3, 20
  shl r63 = r2, 20
  shr.u r64 = r3, 20
  shr r65 = r3, 20
  extr.u r66 = r3, 53, 55
  extr r67 = r3, 53, 55
  dep.z r68 = r2, 53, 55
  dep.z r69 = 110, 53, 55
  dep r70 = r2, r3, 53, 10
  dep r71 = 0, r3, 53, 55
  shrp r72 = r2, r3, 41
  shl r73 = r2, 41
  shr.u r74 = r3, 41
  shr r75 = r3, 41
  extr.u r76 = r3, 26, 36
  extr r77 = r3, 26, 36
  dep.z r78 = r2, 26, 36
  dep.z r79 = -121, 26, 36
  dep r80 = r2, r3, 26, 1
  dep r81 = -1, r3, 26, 36
  shrp r82 = r2, r3, 53
  shl r83 = r2, 53
  shr.u r84 = r3, 53
  shr r85 = r3, 53
  extr.u r86 = r3, 17, 56
  extr r87 = r3, 17, 56
  dep.z r88 = r2, 17, 56
  dep.z r89 = -51, 17, 56
  dep r90 = r2, r3, 17, 10
  dep r91 = 0, r3, 17, 56
  shrp r92 = r2, r3, 6
  shl r93 = r2, 6
  shr.u r94 = r3, 6
  shr r95 = r3, 6
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.gtu p6, p7 = r2, r3
  cmp.gt p8, p9 = r2, r3
  cmp.gt p10, p11 = -124, r3
  tbit.nz p12, p13 = r3, 14
  cmp4.gtu p14, p15 = r2, r3
  cmp.gtu p16, p17 = -51, r3
  cmp4.eq p18, p19 = 61, r3
  tbit.z p20, p21 = r3, 48
  cmp.eq p22, p23 = r2, r3
  cmp.ltu p24, p25 = -125, r3
  cmp4.ge p26, p27 = r2, r3
  cmp.ge p28, p29 = r2, r3
  cmp4.eq p30, p31 = r2, r3
  tbit.nz p32, p33 = r3, 4
  cmp4.ltu p34, p35 = r2, r3
  cmp.ne p36, p37 = 29, r3
  cmp4.gtu p38, p39 = -117, r3
  tbit.z p40, p41 = r3, 4
  cmp.le p42, p43 = -99, r3
  cmp.geu p44, p45 = r2, r3
  cmp.lt p46, p47 = -67, r3
  cmp4.le p48, p49 = -30, r3
  cmp4.leu p50, p51 = r2, r3
  tbit.z p52, p53 = r3, 14
  cmp4.gt p54, p55 = r2, r3
  cmp4.lt p56, p57 = -95, r3
  cmp.eq p58, p59 = -108, r3
  tbit.nz p60, p61 = r3, 48
  cmp4.ne p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
