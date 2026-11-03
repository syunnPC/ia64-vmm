/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x17fffffff   add R = r2, r3
// EXPECT r9 = 0x180000000   add R = r2, r3, 1
// EXPECT r10 = 0x80000001   sub R = r2, r3
// EXPECT r11 = 0x80000000   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffff8000005d   sub R = 92, r3
// EXPECT r13 = 0x5c   and R = 92, r3
// EXPECT r14 = 0x7fffffff   or R = 92, r3
// EXPECT r15 = 0x7fffffa3   xor R = 92, r3
// EXPECT r16 = 0x0   andcm R = 92, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x17fffffff   or R = r2, r3
// EXPECT r19 = 0x17fffffff   xor R = r2, r3
// EXPECT r20 = 0x100000000   andcm R = r2, r3
// EXPECT r21 = 0x800000f4   adds R = 245, r3
// EXPECT r22 = 0x90af2   addl R = 592626, r1
// EXPECT r23 = 0x27fffffff   shladd R = r2, 1, r3
// EXPECT r24 = 0x200000007fffffff   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x47fffffff   shladd R = r2, 2, r3
// EXPECT r26 = 0x200000007fffffff   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x87fffffff   shladd R = r2, 3, r3
// EXPECT r28 = 0x200000007fffffff   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x107fffffff   shladd R = r2, 4, r3
// EXPECT r30 = 0x200000007fffffff   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x200000007fffffff   addp4 R = r2, r3
// EXPECT r32 = 0x20000000800000f4   addp4 R = 245, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x3f   extr.u R = r3, 0, 6
// EXPECT r37 = 0xffffffffffffffff   extr R = r3, 0, 6
// EXPECT r38 = 0x0   dep.z R = r2, 0, 6
// EXPECT r39 = 0x20   dep.z R = -32, 0, 6
// EXPECT r40 = 0x7fff0000   dep R = r2, r3, 0, 16
// EXPECT r41 = 0x7fffffff   dep R = -1, r3, 0, 6
// EXPECT r42 = 0x100000000000000   shrp R = r2, r3, 40
// EXPECT r43 = 0x0   shl R = r2, 40
// EXPECT r44 = 0x0   shr.u R = r3, 40
// EXPECT r45 = 0x0   shr R = r3, 40
// EXPECT r46 = 0x0   extr.u R = r3, 36, 50
// EXPECT r47 = 0x0   extr R = r3, 36, 50
// EXPECT r48 = 0x0   dep.z R = r2, 36, 50
// EXPECT r49 = 0x33000000000   dep.z R = 51, 36, 50
// EXPECT r50 = 0x7fffffff   dep R = r2, r3, 36, 16
// EXPECT r51 = 0x7fffffff   dep R = 0, r3, 36, 50
// EXPECT r52 = 0x20000000000   shrp R = r2, r3, 55
// EXPECT r53 = 0x0   shl R = r2, 55
// EXPECT r54 = 0x0   shr.u R = r3, 55
// EXPECT r55 = 0x0   shr R = r3, 55
// EXPECT r56 = 0x3fffff   extr.u R = r3, 9, 23
// EXPECT r57 = 0x3fffff   extr R = r3, 9, 23
// EXPECT r58 = 0x0   dep.z R = r2, 9, 23
// EXPECT r59 = 0x3000   dep.z R = 24, 9, 23
// EXPECT r60 = 0x7ff801ff   dep R = r2, r3, 9, 10
// EXPECT r61 = 0x1ff   dep R = 0, r3, 9, 23
// EXPECT r62 = 0x7ffff   shrp R = r2, r3, 12
// EXPECT r63 = 0x100000000000   shl R = r2, 12
// EXPECT r64 = 0x7ffff   shr.u R = r3, 12
// EXPECT r65 = 0x7ffff   shr R = r3, 12
// EXPECT r66 = 0x0   extr.u R = r3, 50, 61
// EXPECT r67 = 0x0   extr R = r3, 50, 61
// EXPECT r68 = 0x0   dep.z R = r2, 50, 61
// EXPECT r69 = 0xfeb4000000000000   dep.z R = -83, 50, 61
// EXPECT r70 = 0x7fffffff   dep R = r2, r3, 50, 14
// EXPECT r71 = 0x7fffffff   dep R = 0, r3, 50, 61
// EXPECT r72 = 0x1fffffff   shrp R = r2, r3, 2
// EXPECT r73 = 0x400000000   shl R = r2, 2
// EXPECT r74 = 0x1fffffff   shr.u R = r3, 2
// EXPECT r75 = 0x1fffffff   shr R = r3, 2
// EXPECT r76 = 0x0   extr.u R = r3, 53, 1
// EXPECT r77 = 0x0   extr R = r3, 53, 1
// EXPECT r78 = 0x0   dep.z R = r2, 53, 1
// EXPECT r79 = 0x20000000000000   dep.z R = 65, 53, 1
// EXPECT r80 = 0x7fffffff   dep R = r2, r3, 53, 5
// EXPECT r81 = 0x7fffffff   dep R = 0, r3, 53, 1
// EXPECT r82 = 0xffff   shrp R = r2, r3, 15
// EXPECT r83 = 0x800000000000   shl R = r2, 15
// EXPECT r84 = 0xffff   shr.u R = r3, 15
// EXPECT r85 = 0xffff   shr R = r3, 15
// EXPECT r86 = 0x0   extr.u R = r3, 56, 26
// EXPECT r87 = 0x0   extr R = r3, 56, 26
// EXPECT r88 = 0x0   dep.z R = r2, 56, 26
// EXPECT r89 = 0xf400000000000000   dep.z R = -12, 56, 26
// EXPECT r90 = 0x7fffffff   dep R = r2, r3, 56, 8
// EXPECT r91 = 0x7fffffff   dep R = 0, r3, 56, 26
// EXPECT r92 = 0xfffff   shrp R = r2, r3, 11
// EXPECT r93 = 0x80000000000   shl R = r2, 11
// EXPECT r94 = 0xfffff   shr.u R = r3, 11
// EXPECT r95 = 0xfffff   shr R = r3, 11
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0x7fffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0x7fffffff   zxt4 R = r3
// EXPECT r102 = 0x1f   popcnt R = r3
// EXPECT p6 = 0   cmp.ge P1, P2 = 57, r3
// EXPECT p7 = 1
// EXPECT p8 = 0   tbit.nz P1, P2 = r3, 58
// EXPECT p9 = 1
// EXPECT p10 = 0   cmp.gt P1, P2 = -37, r3
// EXPECT p11 = 1
// EXPECT p12 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p13 = 1
// EXPECT p14 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp.leu P1, P2 = -56, r3
// EXPECT p17 = 1
// EXPECT p18 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp.lt P1, P2 = 15, r3
// EXPECT p25 = 0
// EXPECT p26 = 1   cmp4.le P1, P2 = -20, r3
// EXPECT p27 = 0
// EXPECT p28 = 1   cmp4.geu P1, P2 = -69, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   tbit.z P1, P2 = r3, 13
// EXPECT p31 = 1
// EXPECT p32 = 0   cmp.eq P1, P2 = 122, r3
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   tbit.nz P1, P2 = r3, 45
// EXPECT p37 = 1
// EXPECT p38 = 1   tbit.z P1, P2 = r3, 58
// EXPECT p39 = 0
// EXPECT p40 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p41 = 0
// EXPECT p42 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp4.eq P1, P2 = 18, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp.gtu P1, P2 = -41, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp4.ge P1, P2 = -76, r3
// EXPECT p49 = 1
// EXPECT p50 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p51 = 1
// EXPECT p52 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p53 = 0
// EXPECT p54 = 1   tbit.z P1, P2 = r3, 58
// EXPECT p55 = 0
// EXPECT p56 = 0   cmp4.gt P1, P2 = -15, r3
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p59 = 0
// EXPECT p60 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p61 = 0
// EXPECT p62 = 1   cmp4.lt P1, P2 = -108, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x100000000
  movl r3 = 0x7fffffff
  movl r4 = 0x7f
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 92, r3
  and r13 = 92, r3
  or r14 = 92, r3
  xor r15 = 92, r3
  andcm r16 = 92, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 245, r3
  addl r22 = 592626, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 245, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 0, 6
  extr r37 = r3, 0, 6
  dep.z r38 = r2, 0, 6
  dep.z r39 = -32, 0, 6
  dep r40 = r2, r3, 0, 16
  dep r41 = -1, r3, 0, 6
  shrp r42 = r2, r3, 40
  shl r43 = r2, 40
  shr.u r44 = r3, 40
  shr r45 = r3, 40
  extr.u r46 = r3, 36, 50
  extr r47 = r3, 36, 50
  dep.z r48 = r2, 36, 50
  dep.z r49 = 51, 36, 50
  dep r50 = r2, r3, 36, 16
  dep r51 = 0, r3, 36, 50
  shrp r52 = r2, r3, 55
  shl r53 = r2, 55
  shr.u r54 = r3, 55
  shr r55 = r3, 55
  extr.u r56 = r3, 9, 23
  extr r57 = r3, 9, 23
  dep.z r58 = r2, 9, 23
  dep.z r59 = 24, 9, 23
  dep r60 = r2, r3, 9, 10
  dep r61 = 0, r3, 9, 23
  shrp r62 = r2, r3, 12
  shl r63 = r2, 12
  shr.u r64 = r3, 12
  shr r65 = r3, 12
  extr.u r66 = r3, 50, 61
  extr r67 = r3, 50, 61
  dep.z r68 = r2, 50, 61
  dep.z r69 = -83, 50, 61
  dep r70 = r2, r3, 50, 14
  dep r71 = 0, r3, 50, 61
  shrp r72 = r2, r3, 2
  shl r73 = r2, 2
  shr.u r74 = r3, 2
  shr r75 = r3, 2
  extr.u r76 = r3, 53, 1
  extr r77 = r3, 53, 1
  dep.z r78 = r2, 53, 1
  dep.z r79 = 65, 53, 1
  dep r80 = r2, r3, 53, 5
  dep r81 = 0, r3, 53, 1
  shrp r82 = r2, r3, 15
  shl r83 = r2, 15
  shr.u r84 = r3, 15
  shr r85 = r3, 15
  extr.u r86 = r3, 56, 26
  extr r87 = r3, 56, 26
  dep.z r88 = r2, 56, 26
  dep.z r89 = -12, 56, 26
  dep r90 = r2, r3, 56, 8
  dep r91 = 0, r3, 56, 26
  shrp r92 = r2, r3, 11
  shl r93 = r2, 11
  shr.u r94 = r3, 11
  shr r95 = r3, 11
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.ge p6, p7 = 57, r3
  tbit.nz p8, p9 = r3, 58
  cmp.gt p10, p11 = -37, r3
  cmp.le p12, p13 = r2, r3
  cmp4.eq p14, p15 = r2, r3
  cmp.leu p16, p17 = -56, r3
  cmp.geu p18, p19 = r2, r3
  cmp4.gtu p20, p21 = r2, r3
  cmp.lt p22, p23 = r2, r3
  cmp.lt p24, p25 = 15, r3
  cmp4.le p26, p27 = -20, r3
  cmp4.geu p28, p29 = -69, r3
  tbit.z p30, p31 = r3, 13
  cmp.eq p32, p33 = 122, r3
  cmp.ltu p34, p35 = r2, r3
  tbit.nz p36, p37 = r3, 45
  tbit.z p38, p39 = r3, 58
  cmp.gt p40, p41 = r2, r3
  cmp4.ge p42, p43 = r2, r3
  cmp4.eq p44, p45 = 18, r3
  cmp.gtu p46, p47 = -41, r3
  cmp4.ge p48, p49 = -76, r3
  cmp.leu p50, p51 = r2, r3
  cmp4.ltu p52, p53 = r2, r3
  tbit.z p54, p55 = r3, 58
  cmp4.gt p56, p57 = -15, r3
  cmp.gtu p58, p59 = r2, r3
  cmp4.ne p60, p61 = r2, r3
  cmp4.lt p62, p63 = -108, r3
  ;;
  break.i 0x1ffffe
