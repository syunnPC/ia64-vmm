/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xa7244dd180000000   add R = r2, r3
// EXPECT r9 = 0xa7244dd180000001   add R = r2, r3, 1
// EXPECT r10 = 0xa7244dcf80000000   sub R = r2, r3
// EXPECT r11 = 0xa7244dcf7fffffff   sub R = r2, r3, 1
// EXPECT r12 = 0xfffffffeffffffa8   sub R = -88, r3
// EXPECT r13 = 0x100000000   and R = -88, r3
// EXPECT r14 = 0xffffffffffffffa8   or R = -88, r3
// EXPECT r15 = 0xfffffffeffffffa8   xor R = -88, r3
// EXPECT r16 = 0xfffffffeffffffa8   andcm R = -88, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0xa7244dd180000000   or R = r2, r3
// EXPECT r19 = 0xa7244dd180000000   xor R = r2, r3
// EXPECT r20 = 0xa7244dd080000000   andcm R = r2, r3
// EXPECT r21 = 0xfffff41a   adds R = -3046, r3
// EXPECT r22 = 0xb9e95   addl R = 761493, r1
// EXPECT r23 = 0x4e489ba200000000   shladd R = r2, 1, r3
// EXPECT r24 = 0x0   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x9c91374300000000   shladd R = r2, 2, r3
// EXPECT r26 = 0x0   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x39226e8500000000   shladd R = r2, 3, r3
// EXPECT r28 = 0x0   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x7244dd0900000000   shladd R = r2, 4, r3
// EXPECT r30 = 0x0   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x80000000   addp4 R = r2, r3
// EXPECT r32 = 0xfffff41a   addp4 R = -3046, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 21, 10
// EXPECT r37 = 0x0   extr R = r3, 21, 10
// EXPECT r38 = 0x0   dep.z R = r2, 21, 10
// EXPECT r39 = 0xe800000   dep.z R = 116, 21, 10
// EXPECT r40 = 0x0   dep R = r2, r3, 21, 12
// EXPECT r41 = 0x100000000   dep R = 0, r3, 21, 10
// EXPECT r42 = 0x8400000000000000   shrp R = r2, r3, 37
// EXPECT r43 = 0x0   shl R = r2, 37
// EXPECT r44 = 0x0   shr.u R = r3, 37
// EXPECT r45 = 0x0   shr R = r3, 37
// EXPECT r46 = 0x0   extr.u R = r3, 20, 10
// EXPECT r47 = 0x0   extr R = r3, 20, 10
// EXPECT r48 = 0x0   dep.z R = r2, 20, 10
// EXPECT r49 = 0x3c200000   dep.z R = -62, 20, 10
// EXPECT r50 = 0x0   dep R = r2, r3, 20, 15
// EXPECT r51 = 0x13ff00000   dep R = -1, r3, 20, 10
// EXPECT r52 = 0xe840000000000000   shrp R = r2, r3, 41
// EXPECT r53 = 0x0   shl R = r2, 41
// EXPECT r54 = 0x0   shr.u R = r3, 41
// EXPECT r55 = 0x0   shr R = r3, 41
// EXPECT r56 = 0x0   extr.u R = r3, 50, 50
// EXPECT r57 = 0x0   extr R = r3, 50, 50
// EXPECT r58 = 0x0   dep.z R = r2, 50, 50
// EXPECT r59 = 0xfe48000000000000   dep.z R = -110, 50, 50
// EXPECT r60 = 0x100000000   dep R = r2, r3, 50, 9
// EXPECT r61 = 0xfffc000100000000   dep R = -1, r3, 50, 50
// EXPECT r62 = 0x8000000   shrp R = r2, r3, 5
// EXPECT r63 = 0xe489ba1000000000   shl R = r2, 5
// EXPECT r64 = 0x8000000   shr.u R = r3, 5
// EXPECT r65 = 0x8000000   shr R = r3, 5
// EXPECT r66 = 0x40000   extr.u R = r3, 14, 44
// EXPECT r67 = 0x40000   extr R = r3, 14, 44
// EXPECT r68 = 0x374200000000000   dep.z R = r2, 14, 44
// EXPECT r69 = 0x1c8000   dep.z R = 114, 14, 44
// EXPECT r70 = 0x100000000   dep R = r2, r3, 14, 4
// EXPECT r71 = 0x0   dep R = 0, r3, 14, 44
// EXPECT r72 = 0x4dd0800000000000   shrp R = r2, r3, 48
// EXPECT r73 = 0x0   shl R = r2, 48
// EXPECT r74 = 0x0   shr.u R = r3, 48
// EXPECT r75 = 0x0   shr R = r3, 48
// EXPECT r76 = 0x0   extr.u R = r3, 51, 26
// EXPECT r77 = 0x0   extr R = r3, 51, 26
// EXPECT r78 = 0x0   dep.z R = r2, 51, 26
// EXPECT r79 = 0x310000000000000   dep.z R = 98, 51, 26
// EXPECT r80 = 0x100000000   dep R = r2, r3, 51, 13
// EXPECT r81 = 0x100000000   dep R = 0, r3, 51, 26
// EXPECT r82 = 0x4000000000000000   shrp R = r2, r3, 33
// EXPECT r83 = 0x0   shl R = r2, 33
// EXPECT r84 = 0x0   shr.u R = r3, 33
// EXPECT r85 = 0x0   shr R = r3, 33
// EXPECT r86 = 0x10   extr.u R = r3, 28, 32
// EXPECT r87 = 0x10   extr R = r3, 28, 32
// EXPECT r88 = 0x800000000000000   dep.z R = r2, 28, 32
// EXPECT r89 = 0xffffffd80000000   dep.z R = -40, 28, 32
// EXPECT r90 = 0x100000000   dep R = r2, r3, 28, 1
// EXPECT r91 = 0x0   dep R = 0, r3, 28, 32
// EXPECT r92 = 0x100000   shrp R = r2, r3, 12
// EXPECT r93 = 0x44dd080000000000   shl R = r2, 12
// EXPECT r94 = 0x100000   shr.u R = r3, 12
// EXPECT r95 = 0x100000   shr R = r3, 12
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0x0   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x0   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 1   tbit.z P1, P2 = r3, 10
// EXPECT p7 = 0
// EXPECT p8 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p11 = 1
// EXPECT p12 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p13 = 0
// EXPECT p14 = 1   cmp4.geu P1, P2 = -78, r3
// EXPECT p15 = 0
// EXPECT p16 = 1   tbit.z P1, P2 = r3, 44
// EXPECT p17 = 0
// EXPECT p18 = 1   cmp4.le P1, P2 = -42, r3
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p23 = 0
// EXPECT p24 = 1   cmp4.ne P1, P2 = -101, r3
// EXPECT p25 = 0
// EXPECT p26 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p27 = 0
// EXPECT p28 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p29 = 1
// EXPECT p30 = 0   tbit.nz P1, P2 = r3, 10
// EXPECT p31 = 1
// EXPECT p32 = 0   tbit.nz P1, P2 = r3, 44
// EXPECT p33 = 1
// EXPECT p34 = 1   cmp.gtu P1, P2 = -44, r3
// EXPECT p35 = 0
// EXPECT p36 = 0   cmp.geu P1, P2 = 98, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.ltu P1, P2 = -63, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp4.leu P1, P2 = 47, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp.gt P1, P2 = 110, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp.eq P1, P2 = -69, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp4.gtu P1, P2 = 20, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp.leu P1, P2 = -123, r3
// EXPECT p49 = 1
// EXPECT p50 = 1   tbit.z P1, P2 = r3, 58
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp.ge P1, P2 = 21, r3
// EXPECT p53 = 1
// EXPECT p54 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 1   tbit.z P1, P2 = r3, 44
// EXPECT p57 = 0
// EXPECT p58 = 1   cmp.lt P1, P2 = -116, r3
// EXPECT p59 = 0
// EXPECT p60 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp.ne P1, P2 = -39, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0xa7244dd080000000
  movl r3 = 0x100000000
  movl r4 = 0x40
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -88, r3
  and r13 = -88, r3
  or r14 = -88, r3
  xor r15 = -88, r3
  andcm r16 = -88, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -3046, r3
  addl r22 = 761493, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -3046, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 21, 10
  extr r37 = r3, 21, 10
  dep.z r38 = r2, 21, 10
  dep.z r39 = 116, 21, 10
  dep r40 = r2, r3, 21, 12
  dep r41 = 0, r3, 21, 10
  shrp r42 = r2, r3, 37
  shl r43 = r2, 37
  shr.u r44 = r3, 37
  shr r45 = r3, 37
  extr.u r46 = r3, 20, 10
  extr r47 = r3, 20, 10
  dep.z r48 = r2, 20, 10
  dep.z r49 = -62, 20, 10
  dep r50 = r2, r3, 20, 15
  dep r51 = -1, r3, 20, 10
  shrp r52 = r2, r3, 41
  shl r53 = r2, 41
  shr.u r54 = r3, 41
  shr r55 = r3, 41
  extr.u r56 = r3, 50, 50
  extr r57 = r3, 50, 50
  dep.z r58 = r2, 50, 50
  dep.z r59 = -110, 50, 50
  dep r60 = r2, r3, 50, 9
  dep r61 = -1, r3, 50, 50
  shrp r62 = r2, r3, 5
  shl r63 = r2, 5
  shr.u r64 = r3, 5
  shr r65 = r3, 5
  extr.u r66 = r3, 14, 44
  extr r67 = r3, 14, 44
  dep.z r68 = r2, 14, 44
  dep.z r69 = 114, 14, 44
  dep r70 = r2, r3, 14, 4
  dep r71 = 0, r3, 14, 44
  shrp r72 = r2, r3, 48
  shl r73 = r2, 48
  shr.u r74 = r3, 48
  shr r75 = r3, 48
  extr.u r76 = r3, 51, 26
  extr r77 = r3, 51, 26
  dep.z r78 = r2, 51, 26
  dep.z r79 = 98, 51, 26
  dep r80 = r2, r3, 51, 13
  dep r81 = 0, r3, 51, 26
  shrp r82 = r2, r3, 33
  shl r83 = r2, 33
  shr.u r84 = r3, 33
  shr r85 = r3, 33
  extr.u r86 = r3, 28, 32
  extr r87 = r3, 28, 32
  dep.z r88 = r2, 28, 32
  dep.z r89 = -40, 28, 32
  dep r90 = r2, r3, 28, 1
  dep r91 = 0, r3, 28, 32
  shrp r92 = r2, r3, 12
  shl r93 = r2, 12
  shr.u r94 = r3, 12
  shr r95 = r3, 12
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  tbit.z p6, p7 = r3, 10
  cmp4.lt p8, p9 = r2, r3
  cmp.eq p10, p11 = r2, r3
  cmp4.le p12, p13 = r2, r3
  cmp4.geu p14, p15 = -78, r3
  tbit.z p16, p17 = r3, 44
  cmp4.le p18, p19 = -42, r3
  cmp.ltu p20, p21 = r2, r3
  cmp.gtu p22, p23 = r2, r3
  cmp4.ne p24, p25 = -101, r3
  cmp4.gtu p26, p27 = r2, r3
  cmp.leu p28, p29 = r2, r3
  tbit.nz p30, p31 = r3, 10
  tbit.nz p32, p33 = r3, 44
  cmp.gtu p34, p35 = -44, r3
  cmp.geu p36, p37 = 98, r3
  cmp.ltu p38, p39 = -63, r3
  cmp4.leu p40, p41 = 47, r3
  cmp.gt p42, p43 = 110, r3
  cmp.eq p44, p45 = -69, r3
  cmp4.gtu p46, p47 = 20, r3
  cmp.leu p48, p49 = -123, r3
  tbit.z p50, p51 = r3, 58
  cmp.ge p52, p53 = 21, r3
  cmp4.ne p54, p55 = r2, r3
  tbit.z p56, p57 = r3, 44
  cmp.lt p58, p59 = -116, r3
  cmp4.gt p60, p61 = r2, r3
  cmp.ne p62, p63 = -39, r3
  ;;
  break.i 0x1ffffe
