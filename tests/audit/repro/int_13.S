/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xfffffffd   add R = r2, r3
// EXPECT r9 = 0xfffffffe   add R = r2, r3, 1
// EXPECT r10 = 0xfffffffeffffffff   sub R = r2, r3
// EXPECT r11 = 0xfffffffefffffffe   sub R = r2, r3, 1
// EXPECT r12 = 0xfffffffeffffffbd   sub R = -68, r3
// EXPECT r13 = 0xffffffbc   and R = -68, r3
// EXPECT r14 = 0xffffffffffffffff   or R = -68, r3
// EXPECT r15 = 0xffffffff00000043   xor R = -68, r3
// EXPECT r16 = 0xffffffff00000000   andcm R = -68, r3
// EXPECT r17 = 0xfffffffe   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0xffffffff00000001   xor R = r2, r3
// EXPECT r20 = 0xffffffff00000000   andcm R = r2, r3
// EXPECT r21 = 0x100000c48   adds R = 3145, r3
// EXPECT r22 = 0x1a65f   addl R = 108127, r1
// EXPECT r23 = 0xfffffffb   shladd R = r2, 1, r3
// EXPECT r24 = 0x60000000fffffffb   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0xfffffff7   shladd R = r2, 2, r3
// EXPECT r26 = 0x60000000fffffff7   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xffffffef   shladd R = r2, 3, r3
// EXPECT r28 = 0x60000000ffffffef   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xffffffdf   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000ffffffdf   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x60000000fffffffd   addp4 R = r2, r3
// EXPECT r32 = 0x6000000000000c48   addp4 R = 3145, r3
// EXPECT r33 = 0xff80000000000000   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 57, 18
// EXPECT r37 = 0x0   extr R = r3, 57, 18
// EXPECT r38 = 0xfc00000000000000   dep.z R = r2, 57, 18
// EXPECT r39 = 0xc200000000000000   dep.z R = 97, 57, 18
// EXPECT r40 = 0xfc000000ffffffff   dep R = r2, r3, 57, 15
// EXPECT r41 = 0xffffffff   dep R = 0, r3, 57, 18
// EXPECT r42 = 0xfffffffff0000000   shrp R = r2, r3, 37
// EXPECT r43 = 0xffffffc000000000   shl R = r2, 37
// EXPECT r44 = 0x0   shr.u R = r3, 37
// EXPECT r45 = 0x0   shr R = r3, 37
// EXPECT r46 = 0x7f   extr.u R = r3, 25, 23
// EXPECT r47 = 0x7f   extr R = r3, 25, 23
// EXPECT r48 = 0xfffffc000000   dep.z R = r2, 25, 23
// EXPECT r49 = 0x6c000000   dep.z R = 54, 25, 23
// EXPECT r50 = 0x3fdffffff   dep R = r2, r3, 25, 9
// EXPECT r51 = 0xffffffffffff   dep R = -1, r3, 25, 23
// EXPECT r52 = 0xffffffffffffff80   shrp R = r2, r3, 58
// EXPECT r53 = 0xf800000000000000   shl R = r2, 58
// EXPECT r54 = 0x0   shr.u R = r3, 58
// EXPECT r55 = 0x0   shr R = r3, 58
// EXPECT r56 = 0x0   extr.u R = r3, 33, 36
// EXPECT r57 = 0x0   extr R = r3, 33, 36
// EXPECT r58 = 0xfffffffc00000000   dep.z R = r2, 33, 36
// EXPECT r59 = 0x9400000000   dep.z R = 74, 33, 36
// EXPECT r60 = 0x3cffffffff   dep R = r2, r3, 33, 5
// EXPECT r61 = 0xfffffffeffffffff   dep R = -1, r3, 33, 36
// EXPECT r62 = 0xfffffff800000003   shrp R = r2, r3, 30
// EXPECT r63 = 0xffffffff80000000   shl R = r2, 30
// EXPECT r64 = 0x3   shr.u R = r3, 30
// EXPECT r65 = 0x3   shr R = r3, 30
// EXPECT r66 = 0x3   extr.u R = r3, 30, 25
// EXPECT r67 = 0x3   extr R = r3, 30, 25
// EXPECT r68 = 0x7fffff80000000   dep.z R = r2, 30, 25
// EXPECT r69 = 0xec0000000   dep.z R = 59, 30, 25
// EXPECT r70 = 0x7bfffffff   dep R = r2, r3, 30, 5
// EXPECT r71 = 0x3fffffff   dep R = 0, r3, 30, 25
// EXPECT r72 = 0xfffffffffffffc00   shrp R = r2, r3, 55
// EXPECT r73 = 0xff00000000000000   shl R = r2, 55
// EXPECT r74 = 0x0   shr.u R = r3, 55
// EXPECT r75 = 0x0   shr R = r3, 55
// EXPECT r76 = 0x0   extr.u R = r3, 59, 50
// EXPECT r77 = 0x0   extr R = r3, 59, 50
// EXPECT r78 = 0xf000000000000000   dep.z R = r2, 59, 50
// EXPECT r79 = 0x800000000000000   dep.z R = -127, 59, 50
// EXPECT r80 = 0xf0000000ffffffff   dep R = r2, r3, 59, 14
// EXPECT r81 = 0xffffffff   dep R = 0, r3, 59, 50
// EXPECT r82 = 0xffffffe00000000f   shrp R = r2, r3, 28
// EXPECT r83 = 0xffffffffe0000000   shl R = r2, 28
// EXPECT r84 = 0xf   shr.u R = r3, 28
// EXPECT r85 = 0xf   shr R = r3, 28
// EXPECT r86 = 0x3fff   extr.u R = r3, 18, 64
// EXPECT r87 = 0x3fff   extr R = r3, 18, 64
// EXPECT r88 = 0xfffffffffff80000   dep.z R = r2, 18, 64
// EXPECT r89 = 0x18c0000   dep.z R = 99, 18, 64
// EXPECT r90 = 0xfffbffff   dep R = r2, r3, 18, 9
// EXPECT r91 = 0x3ffff   dep R = 0, r3, 18, 64
// EXPECT r92 = 0xffffffffffffe000   shrp R = r2, r3, 52
// EXPECT r93 = 0xffe0000000000000   shl R = r2, 52
// EXPECT r94 = 0x0   shr.u R = r3, 52
// EXPECT r95 = 0x0   shr R = r3, 52
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0xffffffffffffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0xffffffff   zxt4 R = r3
// EXPECT r102 = 0x20   popcnt R = r3
// EXPECT p6 = 1   tbit.z P1, P2 = r3, 54
// EXPECT p7 = 0
// EXPECT p8 = 1   cmp.lt P1, P2 = -25, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp4.gt P1, P2 = -71, r3
// EXPECT p11 = 1
// EXPECT p12 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p13 = 1
// EXPECT p14 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp.geu P1, P2 = -109, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   cmp.ltu P1, P2 = 63, r3
// EXPECT p19 = 0
// EXPECT p20 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p21 = 0
// EXPECT p22 = 1   tbit.nz P1, P2 = r3, 4
// EXPECT p23 = 0
// EXPECT p24 = 0   cmp.ge P1, P2 = -107, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp.lt P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p31 = 0
// EXPECT p32 = 1   tbit.z P1, P2 = r3, 56
// EXPECT p33 = 0
// EXPECT p34 = 1   cmp4.ne P1, P2 = 46, r3
// EXPECT p35 = 0
// EXPECT p36 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p37 = 0
// EXPECT p38 = 0   tbit.nz P1, P2 = r3, 56
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp4.gtu P1, P2 = -13, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp4.ge P1, P2 = -43, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp.gtu P1, P2 = -20, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p49 = 1
// EXPECT p50 = 0   tbit.nz P1, P2 = r3, 54
// EXPECT p51 = 1
// EXPECT p52 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p53 = 1
// EXPECT p54 = 1   cmp4.ltu P1, P2 = 23, r3
// EXPECT p55 = 0
// EXPECT p56 = 1   tbit.z P1, P2 = r3, 43
// EXPECT p57 = 0
// EXPECT p58 = 0   cmp4.eq P1, P2 = 55, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp.le P1, P2 = -58, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0xfffffffffffffffe
  movl r3 = 0xffffffff
  movl r4 = 0x36
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -68, r3
  and r13 = -68, r3
  or r14 = -68, r3
  xor r15 = -68, r3
  andcm r16 = -68, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 3145, r3
  addl r22 = 108127, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 3145, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 57, 18
  extr r37 = r3, 57, 18
  dep.z r38 = r2, 57, 18
  dep.z r39 = 97, 57, 18
  dep r40 = r2, r3, 57, 15
  dep r41 = 0, r3, 57, 18
  shrp r42 = r2, r3, 37
  shl r43 = r2, 37
  shr.u r44 = r3, 37
  shr r45 = r3, 37
  extr.u r46 = r3, 25, 23
  extr r47 = r3, 25, 23
  dep.z r48 = r2, 25, 23
  dep.z r49 = 54, 25, 23
  dep r50 = r2, r3, 25, 9
  dep r51 = -1, r3, 25, 23
  shrp r52 = r2, r3, 58
  shl r53 = r2, 58
  shr.u r54 = r3, 58
  shr r55 = r3, 58
  extr.u r56 = r3, 33, 36
  extr r57 = r3, 33, 36
  dep.z r58 = r2, 33, 36
  dep.z r59 = 74, 33, 36
  dep r60 = r2, r3, 33, 5
  dep r61 = -1, r3, 33, 36
  shrp r62 = r2, r3, 30
  shl r63 = r2, 30
  shr.u r64 = r3, 30
  shr r65 = r3, 30
  extr.u r66 = r3, 30, 25
  extr r67 = r3, 30, 25
  dep.z r68 = r2, 30, 25
  dep.z r69 = 59, 30, 25
  dep r70 = r2, r3, 30, 5
  dep r71 = 0, r3, 30, 25
  shrp r72 = r2, r3, 55
  shl r73 = r2, 55
  shr.u r74 = r3, 55
  shr r75 = r3, 55
  extr.u r76 = r3, 59, 50
  extr r77 = r3, 59, 50
  dep.z r78 = r2, 59, 50
  dep.z r79 = -127, 59, 50
  dep r80 = r2, r3, 59, 14
  dep r81 = 0, r3, 59, 50
  shrp r82 = r2, r3, 28
  shl r83 = r2, 28
  shr.u r84 = r3, 28
  shr r85 = r3, 28
  extr.u r86 = r3, 18, 64
  extr r87 = r3, 18, 64
  dep.z r88 = r2, 18, 64
  dep.z r89 = 99, 18, 64
  dep r90 = r2, r3, 18, 9
  dep r91 = 0, r3, 18, 64
  shrp r92 = r2, r3, 52
  shl r93 = r2, 52
  shr.u r94 = r3, 52
  shr r95 = r3, 52
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  tbit.z p6, p7 = r3, 54
  cmp.lt p8, p9 = -25, r3
  cmp4.gt p10, p11 = -71, r3
  cmp.eq p12, p13 = r2, r3
  cmp.le p14, p15 = r2, r3
  cmp.geu p16, p17 = -109, r3
  cmp.ltu p18, p19 = 63, r3
  cmp4.ltu p20, p21 = r2, r3
  tbit.nz p22, p23 = r3, 4
  cmp.ge p24, p25 = -107, r3
  cmp4.ge p26, p27 = r2, r3
  cmp.lt p28, p29 = r2, r3
  cmp4.leu p30, p31 = r2, r3
  tbit.z p32, p33 = r3, 56
  cmp4.ne p34, p35 = 46, r3
  cmp.ne p36, p37 = r2, r3
  tbit.nz p38, p39 = r3, 56
  cmp.leu p40, p41 = r2, r3
  cmp4.gtu p42, p43 = -13, r3
  cmp4.ge p44, p45 = -43, r3
  cmp.gtu p46, p47 = -20, r3
  cmp4.geu p48, p49 = r2, r3
  tbit.nz p50, p51 = r3, 54
  cmp.ge p52, p53 = r2, r3
  cmp4.ltu p54, p55 = 23, r3
  tbit.z p56, p57 = r3, 43
  cmp4.eq p58, p59 = 55, r3
  cmp.ltu p60, p61 = r2, r3
  cmp.le p62, p63 = -58, r3
  ;;
  break.i 0x1ffffe
