/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x84533245   add R = r2, r3
// EXPECT r9 = 0x84533246   add R = r2, r3, 1
// EXPECT r10 = 0xffffffff7baccdb7   sub R = r2, r3
// EXPECT r11 = 0xffffffff7baccdb6   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffff7baccdbb   sub R = 2, r3
// EXPECT r13 = 0x2   and R = 2, r3
// EXPECT r14 = 0x84533247   or R = 2, r3
// EXPECT r15 = 0x84533245   xor R = 2, r3
// EXPECT r16 = 0x0   andcm R = 2, r3
// EXPECT r17 = 0x84533246   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0xffffffff7baccdb9   xor R = r2, r3
// EXPECT r20 = 0xffffffff7baccdb8   andcm R = r2, r3
// EXPECT r21 = 0x84532676   adds R = -3025, r3
// EXPECT r22 = 0x9d6e5   addl R = 644837, r1
// EXPECT r23 = 0x84533243   shladd R = r2, 1, r3
// EXPECT r24 = 0x4000000084533243   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x8453323f   shladd R = r2, 2, r3
// EXPECT r26 = 0x400000008453323f   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x84533237   shladd R = r2, 3, r3
// EXPECT r28 = 0x4000000084533237   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x84533227   shladd R = r2, 4, r3
// EXPECT r30 = 0x4000000084533227   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x4000000084533245   addp4 R = r2, r3
// EXPECT r32 = 0x4000000084532676   addp4 R = -3025, r3
// EXPECT r33 = 0xfffffffffffffffc   shl R = r2, r4
// EXPECT r34 = 0x42299923   shr.u R = r3, r4
// EXPECT r35 = 0x42299923   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 39, 10
// EXPECT r37 = 0x0   extr R = r3, 39, 10
// EXPECT r38 = 0x1ff0000000000   dep.z R = r2, 39, 10
// EXPECT r39 = 0x1d8000000000   dep.z R = 59, 39, 10
// EXPECT r40 = 0x10084533247   dep R = r2, r3, 39, 2
// EXPECT r41 = 0x84533247   dep R = 0, r3, 39, 10
// EXPECT r42 = 0xfffffffffe000000   shrp R = r2, r3, 40
// EXPECT r43 = 0xfffffe0000000000   shl R = r2, 40
// EXPECT r44 = 0x0   shr.u R = r3, 40
// EXPECT r45 = 0x0   shr R = r3, 40
// EXPECT r46 = 0x0   extr.u R = r3, 43, 9
// EXPECT r47 = 0x0   extr R = r3, 43, 9
// EXPECT r48 = 0xff00000000000   dep.z R = r2, 43, 9
// EXPECT r49 = 0xf80000000000   dep.z R = 31, 43, 9
// EXPECT r50 = 0x700084533247   dep R = r2, r3, 43, 4
// EXPECT r51 = 0x84533247   dep R = 0, r3, 43, 9
// EXPECT r52 = 0xffffc0000000108a   shrp R = r2, r3, 19
// EXPECT r53 = 0xfffffffffff00000   shl R = r2, 19
// EXPECT r54 = 0x108a   shr.u R = r3, 19
// EXPECT r55 = 0x108a   shr R = r3, 19
// EXPECT r56 = 0x0   extr.u R = r3, 62, 37
// EXPECT r57 = 0x0   extr R = r3, 62, 37
// EXPECT r58 = 0x8000000000000000   dep.z R = r2, 62, 37
// EXPECT r59 = 0x0   dep.z R = 8, 62, 37
// EXPECT r60 = 0x8000000084533247   dep R = r2, r3, 62, 9
// EXPECT r61 = 0x84533247   dep R = 0, r3, 62, 37
// EXPECT r62 = 0xffc0000000108a66   shrp R = r2, r3, 11
// EXPECT r63 = 0xfffffffffffff000   shl R = r2, 11
// EXPECT r64 = 0x108a66   shr.u R = r3, 11
// EXPECT r65 = 0x108a66   shr R = r3, 11
// EXPECT r66 = 0x0   extr.u R = r3, 63, 20
// EXPECT r67 = 0x0   extr R = r3, 63, 20
// EXPECT r68 = 0x0   dep.z R = r2, 63, 20
// EXPECT r69 = 0x8000000000000000   dep.z R = -55, 63, 20
// EXPECT r70 = 0x84533247   dep R = r2, r3, 63, 11
// EXPECT r71 = 0x8000000084533247   dep R = -1, r3, 63, 20
// EXPECT r72 = 0xffffff0000000042   shrp R = r2, r3, 25
// EXPECT r73 = 0xfffffffffc000000   shl R = r2, 25
// EXPECT r74 = 0x42   shr.u R = r3, 25
// EXPECT r75 = 0x42   shr R = r3, 25
// EXPECT r76 = 0x8453   extr.u R = r3, 16, 61
// EXPECT r77 = 0x8453   extr R = r3, 16, 61
// EXPECT r78 = 0xfffffffffffe0000   dep.z R = r2, 16, 61
// EXPECT r79 = 0x40000   dep.z R = 4, 16, 61
// EXPECT r80 = 0x84fe3247   dep R = r2, r3, 16, 8
// EXPECT r81 = 0x3247   dep R = 0, r3, 16, 61
// EXPECT r82 = 0xff800000002114cc   shrp R = r2, r3, 10
// EXPECT r83 = 0xfffffffffffff800   shl R = r2, 10
// EXPECT r84 = 0x2114cc   shr.u R = r3, 10
// EXPECT r85 = 0x2114cc   shr R = r3, 10
// EXPECT r86 = 0x4229   extr.u R = r3, 17, 34
// EXPECT r87 = 0x4229   extr R = r3, 17, 34
// EXPECT r88 = 0x7fffffffc0000   dep.z R = r2, 17, 34
// EXPECT r89 = 0x400000   dep.z R = 32, 17, 34
// EXPECT r90 = 0x85fd3247   dep R = r2, r3, 17, 8
// EXPECT r91 = 0x7ffffffff3247   dep R = -1, r3, 17, 34
// EXPECT r92 = 0xffffff8000000021   shrp R = r2, r3, 26
// EXPECT r93 = 0xfffffffff8000000   shl R = r2, 26
// EXPECT r94 = 0x21   shr.u R = r3, 26
// EXPECT r95 = 0x21   shr R = r3, 26
// EXPECT r96 = 0x47   sxt1 R = r3
// EXPECT r97 = 0x3247   sxt2 R = r3
// EXPECT r98 = 0xffffffff84533247   sxt4 R = r3
// EXPECT r99 = 0x47   zxt1 R = r3
// EXPECT r100 = 0x3247   zxt2 R = r3
// EXPECT r101 = 0x84533247   zxt4 R = r3
// EXPECT r102 = 0xd   popcnt R = r3
// EXPECT p6 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p7 = 0
// EXPECT p8 = 1   cmp4.leu P1, P2 = 32, r3
// EXPECT p9 = 0
// EXPECT p10 = 1   cmp4.ge P1, P2 = r2, r3
// EXPECT p11 = 0
// EXPECT p12 = 1   cmp4.gt P1, P2 = 96, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp4.lt P1, P2 = -111, r3
// EXPECT p17 = 1
// EXPECT p18 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p19 = 0
// EXPECT p20 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p21 = 0
// EXPECT p22 = 0   cmp.geu P1, P2 = 46, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp4.ne P1, P2 = 62, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   tbit.nz P1, P2 = r3, 10
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp.le P1, P2 = 57, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp4.eq P1, P2 = -1, r3
// EXPECT p31 = 1
// EXPECT p32 = 1   cmp.lt P1, P2 = r2, r3
// EXPECT p33 = 0
// EXPECT p34 = 0   cmp.eq P1, P2 = 28, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p39 = 1
// EXPECT p40 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p41 = 0
// EXPECT p42 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp.gt P1, P2 = 0, r3
// EXPECT p45 = 1
// EXPECT p46 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p47 = 1
// EXPECT p48 = 0   cmp4.le P1, P2 = 102, r3
// EXPECT p49 = 1
// EXPECT p50 = 0   cmp4.ltu P1, P2 = -99, r3
// EXPECT p51 = 1
// EXPECT p52 = 1   tbit.z P1, P2 = r3, 52
// EXPECT p53 = 0
// EXPECT p54 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 1   tbit.z P1, P2 = r3, 10
// EXPECT p57 = 0
// EXPECT p58 = 0   tbit.nz P1, P2 = r3, 41
// EXPECT p59 = 1
// EXPECT p60 = 1   cmp.leu P1, P2 = 38, r3
// EXPECT p61 = 0
// EXPECT p62 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0xfffffffffffffffe
  movl r3 = 0x84533247
  movl r4 = 0x1
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 2, r3
  and r13 = 2, r3
  or r14 = 2, r3
  xor r15 = 2, r3
  andcm r16 = 2, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -3025, r3
  addl r22 = 644837, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -3025, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 39, 10
  extr r37 = r3, 39, 10
  dep.z r38 = r2, 39, 10
  dep.z r39 = 59, 39, 10
  dep r40 = r2, r3, 39, 2
  dep r41 = 0, r3, 39, 10
  shrp r42 = r2, r3, 40
  shl r43 = r2, 40
  shr.u r44 = r3, 40
  shr r45 = r3, 40
  extr.u r46 = r3, 43, 9
  extr r47 = r3, 43, 9
  dep.z r48 = r2, 43, 9
  dep.z r49 = 31, 43, 9
  dep r50 = r2, r3, 43, 4
  dep r51 = 0, r3, 43, 9
  shrp r52 = r2, r3, 19
  shl r53 = r2, 19
  shr.u r54 = r3, 19
  shr r55 = r3, 19
  extr.u r56 = r3, 62, 37
  extr r57 = r3, 62, 37
  dep.z r58 = r2, 62, 37
  dep.z r59 = 8, 62, 37
  dep r60 = r2, r3, 62, 9
  dep r61 = 0, r3, 62, 37
  shrp r62 = r2, r3, 11
  shl r63 = r2, 11
  shr.u r64 = r3, 11
  shr r65 = r3, 11
  extr.u r66 = r3, 63, 20
  extr r67 = r3, 63, 20
  dep.z r68 = r2, 63, 20
  dep.z r69 = -55, 63, 20
  dep r70 = r2, r3, 63, 11
  dep r71 = -1, r3, 63, 20
  shrp r72 = r2, r3, 25
  shl r73 = r2, 25
  shr.u r74 = r3, 25
  shr r75 = r3, 25
  extr.u r76 = r3, 16, 61
  extr r77 = r3, 16, 61
  dep.z r78 = r2, 16, 61
  dep.z r79 = 4, 16, 61
  dep r80 = r2, r3, 16, 8
  dep r81 = 0, r3, 16, 61
  shrp r82 = r2, r3, 10
  shl r83 = r2, 10
  shr.u r84 = r3, 10
  shr r85 = r3, 10
  extr.u r86 = r3, 17, 34
  extr r87 = r3, 17, 34
  dep.z r88 = r2, 17, 34
  dep.z r89 = 32, 17, 34
  dep r90 = r2, r3, 17, 8
  dep r91 = -1, r3, 17, 34
  shrp r92 = r2, r3, 26
  shl r93 = r2, 26
  shr.u r94 = r3, 26
  shr r95 = r3, 26
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.gtu p6, p7 = r2, r3
  cmp4.leu p8, p9 = 32, r3
  cmp4.ge p10, p11 = r2, r3
  cmp4.gt p12, p13 = 96, r3
  cmp4.ltu p14, p15 = r2, r3
  cmp4.lt p16, p17 = -111, r3
  cmp.gtu p18, p19 = r2, r3
  cmp.geu p20, p21 = r2, r3
  cmp.geu p22, p23 = 46, r3
  cmp4.ne p24, p25 = 62, r3
  tbit.nz p26, p27 = r3, 10
  cmp.le p28, p29 = 57, r3
  cmp4.eq p30, p31 = -1, r3
  cmp.lt p32, p33 = r2, r3
  cmp.eq p34, p35 = 28, r3
  cmp.ge p36, p37 = r2, r3
  cmp.eq p38, p39 = r2, r3
  cmp4.ne p40, p41 = r2, r3
  cmp4.eq p42, p43 = r2, r3
  cmp.gt p44, p45 = 0, r3
  cmp.gt p46, p47 = r2, r3
  cmp4.le p48, p49 = 102, r3
  cmp4.ltu p50, p51 = -99, r3
  tbit.z p52, p53 = r3, 52
  cmp.le p54, p55 = r2, r3
  tbit.z p56, p57 = r3, 10
  tbit.nz p58, p59 = r3, 41
  cmp.leu p60, p61 = 38, r3
  cmp.leu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
