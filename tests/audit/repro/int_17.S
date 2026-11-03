/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x800000007ffffffe   add R = r2, r3
// EXPECT r9 = 0x800000007fffffff   add R = r2, r3, 1
// EXPECT r10 = 0x7fffffff80000000   sub R = r2, r3
// EXPECT r11 = 0x7fffffff7fffffff   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffff80000073   sub R = 114, r3
// EXPECT r13 = 0x72   and R = 114, r3
// EXPECT r14 = 0x7fffffff   or R = 114, r3
// EXPECT r15 = 0x7fffff8d   xor R = 114, r3
// EXPECT r16 = 0x0   andcm R = 114, r3
// EXPECT r17 = 0x7fffffff   and R = r2, r3
// EXPECT r18 = 0x7fffffffffffffff   or R = r2, r3
// EXPECT r19 = 0x7fffffff80000000   xor R = r2, r3
// EXPECT r20 = 0x7fffffff80000000   andcm R = r2, r3
// EXPECT r21 = 0x7ffff21f   adds R = -3552, r3
// EXPECT r22 = 0x144f6e   addl R = 1331054, r1
// EXPECT r23 = 0x7ffffffd   shladd R = r2, 1, r3
// EXPECT r24 = 0x200000007ffffffd   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x7ffffffb   shladd R = r2, 2, r3
// EXPECT r26 = 0x200000007ffffffb   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x7ffffff7   shladd R = r2, 3, r3
// EXPECT r28 = 0x200000007ffffff7   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x7fffffef   shladd R = r2, 4, r3
// EXPECT r30 = 0x200000007fffffef   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x200000007ffffffe   addp4 R = r2, r3
// EXPECT r32 = 0x200000007ffff21f   addp4 R = -3552, r3
// EXPECT r33 = 0xfffffffffffffffe   shl R = r2, r4
// EXPECT r34 = 0x3fffffff   shr.u R = r3, r4
// EXPECT r35 = 0x3fffffff   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 43, 3
// EXPECT r37 = 0x0   extr R = r3, 43, 3
// EXPECT r38 = 0x380000000000   dep.z R = r2, 43, 3
// EXPECT r39 = 0x280000000000   dep.z R = 93, 43, 3
// EXPECT r40 = 0x7ff8007fffffff   dep R = r2, r3, 43, 12
// EXPECT r41 = 0x7fffffff   dep R = 0, r3, 43, 3
// EXPECT r42 = 0xfffffffffff80000   shrp R = r2, r3, 45
// EXPECT r43 = 0xffffe00000000000   shl R = r2, 45
// EXPECT r44 = 0x0   shr.u R = r3, 45
// EXPECT r45 = 0x0   shr R = r3, 45
// EXPECT r46 = 0x1ffffff   extr.u R = r3, 6, 62
// EXPECT r47 = 0x1ffffff   extr R = r3, 6, 62
// EXPECT r48 = 0xffffffffffffffc0   dep.z R = r2, 6, 62
// EXPECT r49 = 0xfc0   dep.z R = 63, 6, 62
// EXPECT r50 = 0x7fffffff   dep R = r2, r3, 6, 1
// EXPECT r51 = 0xffffffffffffffff   dep R = -1, r3, 6, 62
// EXPECT r52 = 0xfffffffc00000001   shrp R = r2, r3, 30
// EXPECT r53 = 0xffffffffc0000000   shl R = r2, 30
// EXPECT r54 = 0x1   shr.u R = r3, 30
// EXPECT r55 = 0x1   shr R = r3, 30
// EXPECT r56 = 0x1ffff   extr.u R = r3, 14, 28
// EXPECT r57 = 0x1ffff   extr R = r3, 14, 28
// EXPECT r58 = 0x3ffffffc000   dep.z R = r2, 14, 28
// EXPECT r59 = 0x3ffffffc000   dep.z R = -1, 14, 28
// EXPECT r60 = 0x7fffffff   dep R = r2, r3, 14, 12
// EXPECT r61 = 0x3fff   dep R = 0, r3, 14, 28
// EXPECT r62 = 0xffffffffc0000000   shrp R = r2, r3, 34
// EXPECT r63 = 0xfffffffc00000000   shl R = r2, 34
// EXPECT r64 = 0x0   shr.u R = r3, 34
// EXPECT r65 = 0x0   shr R = r3, 34
// EXPECT r66 = 0x7fffff   extr.u R = r3, 8, 36
// EXPECT r67 = 0x7fffff   extr R = r3, 8, 36
// EXPECT r68 = 0xfffffffff00   dep.z R = r2, 8, 36
// EXPECT r69 = 0xfffffffe200   dep.z R = -30, 8, 36
// EXPECT r70 = 0x7fffffff   dep R = r2, r3, 8, 5
// EXPECT r71 = 0xfffffffffff   dep R = -1, r3, 8, 36
// EXPECT r72 = 0xfffffffffe000000   shrp R = r2, r3, 39
// EXPECT r73 = 0xffffff8000000000   shl R = r2, 39
// EXPECT r74 = 0x0   shr.u R = r3, 39
// EXPECT r75 = 0x0   shr R = r3, 39
// EXPECT r76 = 0x0   extr.u R = r3, 44, 41
// EXPECT r77 = 0x0   extr R = r3, 44, 41
// EXPECT r78 = 0xfffff00000000000   dep.z R = r2, 44, 41
// EXPECT r79 = 0xfffd700000000000   dep.z R = -41, 44, 41
// EXPECT r80 = 0x7ff0007fffffff   dep R = r2, r3, 44, 11
// EXPECT r81 = 0x7fffffff   dep R = 0, r3, 44, 41
// EXPECT r82 = 0xffffffffc0000000   shrp R = r2, r3, 34
// EXPECT r83 = 0xfffffffc00000000   shl R = r2, 34
// EXPECT r84 = 0x0   shr.u R = r3, 34
// EXPECT r85 = 0x0   shr R = r3, 34
// EXPECT r86 = 0x0   extr.u R = r3, 42, 2
// EXPECT r87 = 0x0   extr R = r3, 42, 2
// EXPECT r88 = 0xc0000000000   dep.z R = r2, 42, 2
// EXPECT r89 = 0x40000000000   dep.z R = -103, 42, 2
// EXPECT r90 = 0x3ffc007fffffff   dep R = r2, r3, 42, 12
// EXPECT r91 = 0x7fffffff   dep R = 0, r3, 42, 2
// EXPECT r92 = 0xffffffffffffffe0   shrp R = r2, r3, 59
// EXPECT r93 = 0xf800000000000000   shl R = r2, 59
// EXPECT r94 = 0x0   shr.u R = r3, 59
// EXPECT r95 = 0x0   shr R = r3, 59
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0x7fffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0x7fffffff   zxt4 R = r3
// EXPECT r102 = 0x1f   popcnt R = r3
// EXPECT p6 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p7 = 1
// EXPECT p8 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p9 = 1
// EXPECT p10 = 1   cmp.lt P1, P2 = -101, r3
// EXPECT p11 = 0
// EXPECT p12 = 0   tbit.nz P1, P2 = r3, 40
// EXPECT p13 = 1
// EXPECT p14 = 1   cmp.geu P1, P2 = -58, r3
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   cmp4.lt P1, P2 = 72, r3
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp4.leu P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p25 = 0
// EXPECT p26 = 1   tbit.z P1, P2 = r3, 40
// EXPECT p27 = 0
// EXPECT p28 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp.gtu P1, P2 = 120, r3
// EXPECT p31 = 1
// EXPECT p32 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p33 = 0
// EXPECT p34 = 0   tbit.nz P1, P2 = r3, 40
// EXPECT p35 = 1
// EXPECT p36 = 0   tbit.z P1, P2 = r3, 6
// EXPECT p37 = 1
// EXPECT p38 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p39 = 0
// EXPECT p40 = 0   cmp.eq P1, P2 = 11, r3
// EXPECT p41 = 1
// EXPECT p42 = 1   cmp4.ne P1, P2 = 120, r3
// EXPECT p43 = 0
// EXPECT p44 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 1   cmp4.gtu P1, P2 = -72, r3
// EXPECT p47 = 0
// EXPECT p48 = 1   cmp4.leu P1, P2 = 0, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp4.ltu P1, P2 = -35, r3
// EXPECT p53 = 1
// EXPECT p54 = 0   cmp.ltu P1, P2 = -27, r3
// EXPECT p55 = 1
// EXPECT p56 = 0   cmp.ge P1, P2 = -118, r3
// EXPECT p57 = 1
// EXPECT p58 = 0   cmp.gt P1, P2 = -90, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   cmp4.gt P1, P2 = -51, r3
// EXPECT p61 = 1
// EXPECT p62 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0x7fffffffffffffff
  movl r3 = 0x7fffffff
  movl r4 = 0x1
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 114, r3
  and r13 = 114, r3
  or r14 = 114, r3
  xor r15 = 114, r3
  andcm r16 = 114, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -3552, r3
  addl r22 = 1331054, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -3552, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 43, 3
  extr r37 = r3, 43, 3
  dep.z r38 = r2, 43, 3
  dep.z r39 = 93, 43, 3
  dep r40 = r2, r3, 43, 12
  dep r41 = 0, r3, 43, 3
  shrp r42 = r2, r3, 45
  shl r43 = r2, 45
  shr.u r44 = r3, 45
  shr r45 = r3, 45
  extr.u r46 = r3, 6, 62
  extr r47 = r3, 6, 62
  dep.z r48 = r2, 6, 62
  dep.z r49 = 63, 6, 62
  dep r50 = r2, r3, 6, 1
  dep r51 = -1, r3, 6, 62
  shrp r52 = r2, r3, 30
  shl r53 = r2, 30
  shr.u r54 = r3, 30
  shr r55 = r3, 30
  extr.u r56 = r3, 14, 28
  extr r57 = r3, 14, 28
  dep.z r58 = r2, 14, 28
  dep.z r59 = -1, 14, 28
  dep r60 = r2, r3, 14, 12
  dep r61 = 0, r3, 14, 28
  shrp r62 = r2, r3, 34
  shl r63 = r2, 34
  shr.u r64 = r3, 34
  shr r65 = r3, 34
  extr.u r66 = r3, 8, 36
  extr r67 = r3, 8, 36
  dep.z r68 = r2, 8, 36
  dep.z r69 = -30, 8, 36
  dep r70 = r2, r3, 8, 5
  dep r71 = -1, r3, 8, 36
  shrp r72 = r2, r3, 39
  shl r73 = r2, 39
  shr.u r74 = r3, 39
  shr r75 = r3, 39
  extr.u r76 = r3, 44, 41
  extr r77 = r3, 44, 41
  dep.z r78 = r2, 44, 41
  dep.z r79 = -41, 44, 41
  dep r80 = r2, r3, 44, 11
  dep r81 = 0, r3, 44, 41
  shrp r82 = r2, r3, 34
  shl r83 = r2, 34
  shr.u r84 = r3, 34
  shr r85 = r3, 34
  extr.u r86 = r3, 42, 2
  extr r87 = r3, 42, 2
  dep.z r88 = r2, 42, 2
  dep.z r89 = -103, 42, 2
  dep r90 = r2, r3, 42, 12
  dep r91 = 0, r3, 42, 2
  shrp r92 = r2, r3, 59
  shl r93 = r2, 59
  shr.u r94 = r3, 59
  shr r95 = r3, 59
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.leu p6, p7 = r2, r3
  cmp4.ltu p8, p9 = r2, r3
  cmp.lt p10, p11 = -101, r3
  tbit.nz p12, p13 = r3, 40
  cmp.geu p14, p15 = -58, r3
  cmp.ge p16, p17 = r2, r3
  cmp4.lt p18, p19 = 72, r3
  cmp4.leu p20, p21 = r2, r3
  cmp.lt p22, p23 = r2, r3
  cmp4.gtu p24, p25 = r2, r3
  tbit.z p26, p27 = r3, 40
  cmp4.geu p28, p29 = r2, r3
  cmp.gtu p30, p31 = 120, r3
  cmp.gtu p32, p33 = r2, r3
  tbit.nz p34, p35 = r3, 40
  tbit.z p36, p37 = r3, 6
  cmp.geu p38, p39 = r2, r3
  cmp.eq p40, p41 = 11, r3
  cmp4.ne p42, p43 = 120, r3
  cmp4.le p44, p45 = r2, r3
  cmp4.gtu p46, p47 = -72, r3
  cmp4.leu p48, p49 = 0, r3
  cmp.gt p50, p51 = r2, r3
  cmp4.ltu p52, p53 = -35, r3
  cmp.ltu p54, p55 = -27, r3
  cmp.ge p56, p57 = -118, r3
  cmp.gt p58, p59 = -90, r3
  cmp4.gt p60, p61 = -51, r3
  cmp4.ge p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
