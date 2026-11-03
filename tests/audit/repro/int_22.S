/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xfffffffe   add R = r2, r3
// EXPECT r9 = 0xffffffff   add R = r2, r3, 1
// EXPECT r10 = 0xffffffff00000000   sub R = r2, r3
// EXPECT r11 = 0xfffffffeffffffff   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffff00000020   sub R = 31, r3
// EXPECT r13 = 0x1f   and R = 31, r3
// EXPECT r14 = 0xffffffff   or R = 31, r3
// EXPECT r15 = 0xffffffe0   xor R = 31, r3
// EXPECT r16 = 0x0   andcm R = 31, r3
// EXPECT r17 = 0xffffffff   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0xffffffff00000000   xor R = r2, r3
// EXPECT r20 = 0xffffffff00000000   andcm R = r2, r3
// EXPECT r21 = 0xfffff233   adds R = -3532, r3
// EXPECT r22 = 0x3c781   addl R = 247681, r1
// EXPECT r23 = 0xfffffffd   shladd R = r2, 1, r3
// EXPECT r24 = 0x60000000fffffffd   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0xfffffffb   shladd R = r2, 2, r3
// EXPECT r26 = 0x60000000fffffffb   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xfffffff7   shladd R = r2, 3, r3
// EXPECT r28 = 0x60000000fffffff7   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xffffffef   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000ffffffef   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x60000000fffffffe   addp4 R = r2, r3
// EXPECT r32 = 0x60000000fffff233   addp4 R = -3532, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 34, 24
// EXPECT r37 = 0x0   extr R = r3, 34, 24
// EXPECT r38 = 0x3fffffc00000000   dep.z R = r2, 34, 24
// EXPECT r39 = 0x17400000000   dep.z R = 93, 34, 24
// EXPECT r40 = 0xcffffffff   dep R = r2, r3, 34, 2
// EXPECT r41 = 0x3fffffcffffffff   dep R = -1, r3, 34, 24
// EXPECT r42 = 0xf00000000fffffff   shrp R = r2, r3, 4
// EXPECT r43 = 0xfffffffffffffff0   shl R = r2, 4
// EXPECT r44 = 0xfffffff   shr.u R = r3, 4
// EXPECT r45 = 0xfffffff   shr R = r3, 4
// EXPECT r46 = 0x0   extr.u R = r3, 53, 33
// EXPECT r47 = 0x0   extr R = r3, 53, 33
// EXPECT r48 = 0xffe0000000000000   dep.z R = r2, 53, 33
// EXPECT r49 = 0x220000000000000   dep.z R = 17, 53, 33
// EXPECT r50 = 0xffe00000ffffffff   dep R = r2, r3, 53, 14
// EXPECT r51 = 0xffe00000ffffffff   dep R = -1, r3, 53, 33
// EXPECT r52 = 0xfffffe00000001ff   shrp R = r2, r3, 23
// EXPECT r53 = 0xffffffffff800000   shl R = r2, 23
// EXPECT r54 = 0x1ff   shr.u R = r3, 23
// EXPECT r55 = 0x1ff   shr R = r3, 23
// EXPECT r56 = 0x0   extr.u R = r3, 39, 48
// EXPECT r57 = 0x0   extr R = r3, 39, 48
// EXPECT r58 = 0xffffff8000000000   dep.z R = r2, 39, 48
// EXPECT r59 = 0x258000000000   dep.z R = 75, 39, 48
// EXPECT r60 = 0x80ffffffff   dep R = r2, r3, 39, 1
// EXPECT r61 = 0xffffff80ffffffff   dep R = -1, r3, 39, 48
// EXPECT r62 = 0xfffffffffffffe00   shrp R = r2, r3, 55
// EXPECT r63 = 0xff80000000000000   shl R = r2, 55
// EXPECT r64 = 0x0   shr.u R = r3, 55
// EXPECT r65 = 0x0   shr R = r3, 55
// EXPECT r66 = 0x0   extr.u R = r3, 54, 12
// EXPECT r67 = 0x0   extr R = r3, 54, 12
// EXPECT r68 = 0xffc0000000000000   dep.z R = r2, 54, 12
// EXPECT r69 = 0xeb80000000000000   dep.z R = -82, 54, 12
// EXPECT r70 = 0xffc00000ffffffff   dep R = r2, r3, 54, 15
// EXPECT r71 = 0xffc00000ffffffff   dep R = -1, r3, 54, 12
// EXPECT r72 = 0xfff00000000fffff   shrp R = r2, r3, 12
// EXPECT r73 = 0xfffffffffffff000   shl R = r2, 12
// EXPECT r74 = 0xfffff   shr.u R = r3, 12
// EXPECT r75 = 0xfffff   shr R = r3, 12
// EXPECT r76 = 0x0   extr.u R = r3, 55, 52
// EXPECT r77 = 0x0   extr R = r3, 55, 52
// EXPECT r78 = 0xff80000000000000   dep.z R = r2, 55, 52
// EXPECT r79 = 0xce80000000000000   dep.z R = -99, 55, 52
// EXPECT r80 = 0x3f800000ffffffff   dep R = r2, r3, 55, 7
// EXPECT r81 = 0xffffffff   dep R = 0, r3, 55, 52
// EXPECT r82 = 0xffffffffffffe000   shrp R = r2, r3, 51
// EXPECT r83 = 0xfff8000000000000   shl R = r2, 51
// EXPECT r84 = 0x0   shr.u R = r3, 51
// EXPECT r85 = 0x0   shr R = r3, 51
// EXPECT r86 = 0x1fff   extr.u R = r3, 0, 13
// EXPECT r87 = 0xffffffffffffffff   extr R = r3, 0, 13
// EXPECT r88 = 0x1fff   dep.z R = r2, 0, 13
// EXPECT r89 = 0x6a   dep.z R = 106, 0, 13
// EXPECT r90 = 0xffffffff   dep R = r2, r3, 0, 7
// EXPECT r91 = 0xffffe000   dep R = 0, r3, 0, 13
// EXPECT r92 = 0xe00000001fffffff   shrp R = r2, r3, 3
// EXPECT r93 = 0xfffffffffffffff8   shl R = r2, 3
// EXPECT r94 = 0x1fffffff   shr.u R = r3, 3
// EXPECT r95 = 0x1fffffff   shr R = r3, 3
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0xffffffffffffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0xffffffff   zxt4 R = r3
// EXPECT r102 = 0x20   popcnt R = r3
// EXPECT p6 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p7 = 0
// EXPECT p8 = 0   tbit.nz P1, P2 = r3, 60
// EXPECT p9 = 1
// EXPECT p10 = 1   cmp.lt P1, P2 = r2, r3
// EXPECT p11 = 0
// EXPECT p12 = 1   tbit.z P1, P2 = r3, 63
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp4.ne P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 1   cmp.ltu P1, P2 = 40, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 1   tbit.nz P1, P2 = r3, 17
// EXPECT p23 = 0
// EXPECT p24 = 0   cmp.eq P1, P2 = 64, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp.leu P1, P2 = -31, r3
// EXPECT p27 = 1
// EXPECT p28 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p29 = 1
// EXPECT p30 = 0   tbit.z P1, P2 = r3, 17
// EXPECT p31 = 1
// EXPECT p32 = 1   tbit.z P1, P2 = r3, 36
// EXPECT p33 = 0
// EXPECT p34 = 0   cmp4.eq P1, P2 = -25, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p39 = 1
// EXPECT p40 = 1   cmp4.eq P1, P2 = r2, r3
// EXPECT p41 = 0
// EXPECT p42 = 0   cmp4.gtu P1, P2 = 107, r3
// EXPECT p43 = 1
// EXPECT p44 = 1   cmp.geu P1, P2 = -60, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   cmp.gt P1, P2 = 99, r3
// EXPECT p47 = 1
// EXPECT p48 = 0   cmp4.le P1, P2 = 82, r3
// EXPECT p49 = 1
// EXPECT p50 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p51 = 0
// EXPECT p52 = 1   cmp.lt P1, P2 = -37, r3
// EXPECT p53 = 0
// EXPECT p54 = 0   tbit.nz P1, P2 = r3, 36
// EXPECT p55 = 1
// EXPECT p56 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p57 = 0
// EXPECT p58 = 1   cmp4.ge P1, P2 = r2, r3
// EXPECT p59 = 0
// EXPECT p60 = 0   cmp4.ge P1, P2 = -106, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp.ne P1, P2 = 50, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0xffffffffffffffff
  movl r3 = 0xffffffff
  movl r4 = 0x41
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 31, r3
  and r13 = 31, r3
  or r14 = 31, r3
  xor r15 = 31, r3
  andcm r16 = 31, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -3532, r3
  addl r22 = 247681, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -3532, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 34, 24
  extr r37 = r3, 34, 24
  dep.z r38 = r2, 34, 24
  dep.z r39 = 93, 34, 24
  dep r40 = r2, r3, 34, 2
  dep r41 = -1, r3, 34, 24
  shrp r42 = r2, r3, 4
  shl r43 = r2, 4
  shr.u r44 = r3, 4
  shr r45 = r3, 4
  extr.u r46 = r3, 53, 33
  extr r47 = r3, 53, 33
  dep.z r48 = r2, 53, 33
  dep.z r49 = 17, 53, 33
  dep r50 = r2, r3, 53, 14
  dep r51 = -1, r3, 53, 33
  shrp r52 = r2, r3, 23
  shl r53 = r2, 23
  shr.u r54 = r3, 23
  shr r55 = r3, 23
  extr.u r56 = r3, 39, 48
  extr r57 = r3, 39, 48
  dep.z r58 = r2, 39, 48
  dep.z r59 = 75, 39, 48
  dep r60 = r2, r3, 39, 1
  dep r61 = -1, r3, 39, 48
  shrp r62 = r2, r3, 55
  shl r63 = r2, 55
  shr.u r64 = r3, 55
  shr r65 = r3, 55
  extr.u r66 = r3, 54, 12
  extr r67 = r3, 54, 12
  dep.z r68 = r2, 54, 12
  dep.z r69 = -82, 54, 12
  dep r70 = r2, r3, 54, 15
  dep r71 = -1, r3, 54, 12
  shrp r72 = r2, r3, 12
  shl r73 = r2, 12
  shr.u r74 = r3, 12
  shr r75 = r3, 12
  extr.u r76 = r3, 55, 52
  extr r77 = r3, 55, 52
  dep.z r78 = r2, 55, 52
  dep.z r79 = -99, 55, 52
  dep r80 = r2, r3, 55, 7
  dep r81 = 0, r3, 55, 52
  shrp r82 = r2, r3, 51
  shl r83 = r2, 51
  shr.u r84 = r3, 51
  shr r85 = r3, 51
  extr.u r86 = r3, 0, 13
  extr r87 = r3, 0, 13
  dep.z r88 = r2, 0, 13
  dep.z r89 = 106, 0, 13
  dep r90 = r2, r3, 0, 7
  dep r91 = 0, r3, 0, 13
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
  cmp4.leu p6, p7 = r2, r3
  tbit.nz p8, p9 = r3, 60
  cmp.lt p10, p11 = r2, r3
  tbit.z p12, p13 = r3, 63
  cmp4.ne p14, p15 = r2, r3
  cmp.ltu p16, p17 = 40, r3
  cmp4.geu p18, p19 = r2, r3
  cmp.ge p20, p21 = r2, r3
  tbit.nz p22, p23 = r3, 17
  cmp.eq p24, p25 = 64, r3
  cmp.leu p26, p27 = -31, r3
  cmp4.gt p28, p29 = r2, r3
  tbit.z p30, p31 = r3, 17
  tbit.z p32, p33 = r3, 36
  cmp4.eq p34, p35 = -25, r3
  cmp.ltu p36, p37 = r2, r3
  cmp.leu p38, p39 = r2, r3
  cmp4.eq p40, p41 = r2, r3
  cmp4.gtu p42, p43 = 107, r3
  cmp.geu p44, p45 = -60, r3
  cmp.gt p46, p47 = 99, r3
  cmp4.le p48, p49 = 82, r3
  cmp.le p50, p51 = r2, r3
  cmp.lt p52, p53 = -37, r3
  tbit.nz p54, p55 = r3, 36
  cmp.ne p56, p57 = r2, r3
  cmp4.ge p58, p59 = r2, r3
  cmp4.ge p60, p61 = -106, r3
  cmp.ne p62, p63 = 50, r3
  ;;
  break.i 0x1ffffe
