/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xffffffff   add R = r2, r3
// EXPECT r9 = 0x100000000   add R = r2, r3, 1
// EXPECT r10 = 0x100000001   sub R = r2, r3
// EXPECT r11 = 0x100000000   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffffffffffa1   sub R = -96, r3
// EXPECT r13 = 0xffffffffffffffa0   and R = -96, r3
// EXPECT r14 = 0xffffffffffffffff   or R = -96, r3
// EXPECT r15 = 0x5f   xor R = -96, r3
// EXPECT r16 = 0x0   andcm R = -96, r3
// EXPECT r17 = 0x100000000   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0xfffffffeffffffff   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0x1173   adds R = 4468, r3
// EXPECT r22 = 0x104842   addl R = 1067074, r1
// EXPECT r23 = 0x1ffffffff   shladd R = r2, 1, r3
// EXPECT r24 = 0x60000000ffffffff   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x3ffffffff   shladd R = r2, 2, r3
// EXPECT r26 = 0x60000000ffffffff   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x7ffffffff   shladd R = r2, 3, r3
// EXPECT r28 = 0x60000000ffffffff   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xfffffffff   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000ffffffff   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x60000000ffffffff   addp4 R = r2, r3
// EXPECT r32 = 0x6000000000001173   addp4 R = 4468, r3
// EXPECT r33 = 0x200000000   shl R = r2, r4
// EXPECT r34 = 0x7fffffffffffffff   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0xff   extr.u R = r3, 13, 8
// EXPECT r37 = 0xffffffffffffffff   extr R = r3, 13, 8
// EXPECT r38 = 0x0   dep.z R = r2, 13, 8
// EXPECT r39 = 0x5a000   dep.z R = 45, 13, 8
// EXPECT r40 = 0xffffffffffe01fff   dep R = r2, r3, 13, 8
// EXPECT r41 = 0xffffffffffe01fff   dep R = 0, r3, 13, 8
// EXPECT r42 = 0x200000001   shrp R = r2, r3, 63
// EXPECT r43 = 0x0   shl R = r2, 63
// EXPECT r44 = 0x1   shr.u R = r3, 63
// EXPECT r45 = 0xffffffffffffffff   shr R = r3, 63
// EXPECT r46 = 0x7ffff   extr.u R = r3, 26, 19
// EXPECT r47 = 0xffffffffffffffff   extr R = r3, 26, 19
// EXPECT r48 = 0x0   dep.z R = r2, 26, 19
// EXPECT r49 = 0x1ffe80000000   dep.z R = -96, 26, 19
// EXPECT r50 = 0xfffffffff3ffffff   dep R = r2, r3, 26, 2
// EXPECT r51 = 0xffffffffffffffff   dep R = -1, r3, 26, 19
// EXPECT r52 = 0xffffffffff   shrp R = r2, r3, 24
// EXPECT r53 = 0x100000000000000   shl R = r2, 24
// EXPECT r54 = 0xffffffffff   shr.u R = r3, 24
// EXPECT r55 = 0xffffffffffffffff   shr R = r3, 24
// EXPECT r56 = 0x3fffffffffff   extr.u R = r3, 18, 59
// EXPECT r57 = 0xffffffffffffffff   extr R = r3, 18, 59
// EXPECT r58 = 0x4000000000000   dep.z R = r2, 18, 59
// EXPECT r59 = 0x18c0000   dep.z R = 99, 18, 59
// EXPECT r60 = 0xfffffffff003ffff   dep R = r2, r3, 18, 10
// EXPECT r61 = 0xffffffffffffffff   dep R = -1, r3, 18, 59
// EXPECT r62 = 0x400000003ff   shrp R = r2, r3, 54
// EXPECT r63 = 0x0   shl R = r2, 54
// EXPECT r64 = 0x3ff   shr.u R = r3, 54
// EXPECT r65 = 0xffffffffffffffff   shr R = r3, 54
// EXPECT r66 = 0x1fffff   extr.u R = r3, 17, 21
// EXPECT r67 = 0xffffffffffffffff   extr R = r3, 17, 21
// EXPECT r68 = 0x0   dep.z R = r2, 17, 21
// EXPECT r69 = 0x3fff620000   dep.z R = -79, 17, 21
// EXPECT r70 = 0xfffffffff001ffff   dep R = r2, r3, 17, 11
// EXPECT r71 = 0xffffffffffffffff   dep R = -1, r3, 17, 21
// EXPECT r72 = 0x200000001   shrp R = r2, r3, 63
// EXPECT r73 = 0x0   shl R = r2, 63
// EXPECT r74 = 0x1   shr.u R = r3, 63
// EXPECT r75 = 0xffffffffffffffff   shr R = r3, 63
// EXPECT r76 = 0x7fffffffff   extr.u R = r3, 25, 40
// EXPECT r77 = 0xffffffffffffffff   extr R = r3, 25, 40
// EXPECT r78 = 0x200000000000000   dep.z R = r2, 25, 40
// EXPECT r79 = 0xffffffff98000000   dep.z R = -52, 25, 40
// EXPECT r80 = 0xffffffe001ffffff   dep R = r2, r3, 25, 12
// EXPECT r81 = 0xffffffffffffffff   dep R = -1, r3, 25, 40
// EXPECT r82 = 0x7fffffffffffff   shrp R = r2, r3, 9
// EXPECT r83 = 0x20000000000   shl R = r2, 9
// EXPECT r84 = 0x7fffffffffffff   shr.u R = r3, 9
// EXPECT r85 = 0xffffffffffffffff   shr R = r3, 9
// EXPECT r86 = 0xfffffffff   extr.u R = r3, 28, 45
// EXPECT r87 = 0xffffffffffffffff   extr R = r3, 28, 45
// EXPECT r88 = 0x1000000000000000   dep.z R = r2, 28, 45
// EXPECT r89 = 0xffffffffc0000000   dep.z R = -4, 28, 45
// EXPECT r90 = 0xffffffffefffffff   dep R = r2, r3, 28, 1
// EXPECT r91 = 0xffffffffffffffff   dep R = -1, r3, 28, 45
// EXPECT r92 = 0x200000001fffff   shrp R = r2, r3, 43
// EXPECT r93 = 0x0   shl R = r2, 43
// EXPECT r94 = 0x1fffff   shr.u R = r3, 43
// EXPECT r95 = 0xffffffffffffffff   shr R = r3, 43
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0xffffffffffffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0xffffffff   zxt4 R = r3
// EXPECT r102 = 0x40   popcnt R = r3
// EXPECT p6 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p7 = 0
// EXPECT p8 = 1   cmp4.ge P1, P2 = r2, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp4.le P1, P2 = r2, r3
// EXPECT p11 = 1
// EXPECT p12 = 0   tbit.z P1, P2 = r3, 17
// EXPECT p13 = 1
// EXPECT p14 = 1   tbit.nz P1, P2 = r3, 29
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp.lt P1, P2 = -111, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   cmp4.ge P1, P2 = 66, r3
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp.ltu P1, P2 = r2, r3
// EXPECT p25 = 0
// EXPECT p26 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p27 = 0
// EXPECT p28 = 1   tbit.nz P1, P2 = r3, 55
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p31 = 1
// EXPECT p32 = 0   cmp4.gt P1, P2 = -69, r3
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p35 = 1
// EXPECT p36 = 1   cmp4.ne P1, P2 = -43, r3
// EXPECT p37 = 0
// EXPECT p38 = 0   cmp.le P1, P2 = 2, r3
// EXPECT p39 = 1
// EXPECT p40 = 1   cmp4.lt P1, P2 = -115, r3
// EXPECT p41 = 0
// EXPECT p42 = 1   cmp4.le P1, P2 = -46, r3
// EXPECT p43 = 0
// EXPECT p44 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   cmp.gtu P1, P2 = 85, r3
// EXPECT p47 = 1
// EXPECT p48 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   tbit.nz P1, P2 = r3, 21
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp.geu P1, P2 = 74, r3
// EXPECT p53 = 1
// EXPECT p54 = 0   tbit.z P1, P2 = r3, 21
// EXPECT p55 = 1
// EXPECT p56 = 0   cmp4.gtu P1, P2 = -122, r3
// EXPECT p57 = 1
// EXPECT p58 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p59 = 1
// EXPECT p60 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p61 = 0
// EXPECT p62 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x100000000
  movl r3 = 0xffffffffffffffff
  movl r4 = 0x1
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -96, r3
  and r13 = -96, r3
  or r14 = -96, r3
  xor r15 = -96, r3
  andcm r16 = -96, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 4468, r3
  addl r22 = 1067074, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 4468, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 13, 8
  extr r37 = r3, 13, 8
  dep.z r38 = r2, 13, 8
  dep.z r39 = 45, 13, 8
  dep r40 = r2, r3, 13, 8
  dep r41 = 0, r3, 13, 8
  shrp r42 = r2, r3, 63
  shl r43 = r2, 63
  shr.u r44 = r3, 63
  shr r45 = r3, 63
  extr.u r46 = r3, 26, 19
  extr r47 = r3, 26, 19
  dep.z r48 = r2, 26, 19
  dep.z r49 = -96, 26, 19
  dep r50 = r2, r3, 26, 2
  dep r51 = -1, r3, 26, 19
  shrp r52 = r2, r3, 24
  shl r53 = r2, 24
  shr.u r54 = r3, 24
  shr r55 = r3, 24
  extr.u r56 = r3, 18, 59
  extr r57 = r3, 18, 59
  dep.z r58 = r2, 18, 59
  dep.z r59 = 99, 18, 59
  dep r60 = r2, r3, 18, 10
  dep r61 = -1, r3, 18, 59
  shrp r62 = r2, r3, 54
  shl r63 = r2, 54
  shr.u r64 = r3, 54
  shr r65 = r3, 54
  extr.u r66 = r3, 17, 21
  extr r67 = r3, 17, 21
  dep.z r68 = r2, 17, 21
  dep.z r69 = -79, 17, 21
  dep r70 = r2, r3, 17, 11
  dep r71 = -1, r3, 17, 21
  shrp r72 = r2, r3, 63
  shl r73 = r2, 63
  shr.u r74 = r3, 63
  shr r75 = r3, 63
  extr.u r76 = r3, 25, 40
  extr r77 = r3, 25, 40
  dep.z r78 = r2, 25, 40
  dep.z r79 = -52, 25, 40
  dep r80 = r2, r3, 25, 12
  dep r81 = -1, r3, 25, 40
  shrp r82 = r2, r3, 9
  shl r83 = r2, 9
  shr.u r84 = r3, 9
  shr r85 = r3, 9
  extr.u r86 = r3, 28, 45
  extr r87 = r3, 28, 45
  dep.z r88 = r2, 28, 45
  dep.z r89 = -4, 28, 45
  dep r90 = r2, r3, 28, 1
  dep r91 = -1, r3, 28, 45
  shrp r92 = r2, r3, 43
  shl r93 = r2, 43
  shr.u r94 = r3, 43
  shr r95 = r3, 43
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.ltu p6, p7 = r2, r3
  cmp4.ge p8, p9 = r2, r3
  cmp4.le p10, p11 = r2, r3
  tbit.z p12, p13 = r3, 17
  tbit.nz p14, p15 = r3, 29
  cmp.lt p16, p17 = -111, r3
  cmp4.ge p18, p19 = 66, r3
  cmp.lt p20, p21 = r2, r3
  cmp.geu p22, p23 = r2, r3
  cmp.ltu p24, p25 = r2, r3
  cmp.ne p26, p27 = r2, r3
  tbit.nz p28, p29 = r3, 55
  cmp4.geu p30, p31 = r2, r3
  cmp4.gt p32, p33 = -69, r3
  cmp4.gtu p34, p35 = r2, r3
  cmp4.ne p36, p37 = -43, r3
  cmp.le p38, p39 = 2, r3
  cmp4.lt p40, p41 = -115, r3
  cmp4.le p42, p43 = -46, r3
  cmp.ge p44, p45 = r2, r3
  cmp.gtu p46, p47 = 85, r3
  cmp4.ne p48, p49 = r2, r3
  tbit.nz p50, p51 = r3, 21
  cmp.geu p52, p53 = 74, r3
  tbit.z p54, p55 = r3, 21
  cmp4.gtu p56, p57 = -122, r3
  cmp.eq p58, p59 = r2, r3
  cmp4.leu p60, p61 = r2, r3
  cmp.leu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
