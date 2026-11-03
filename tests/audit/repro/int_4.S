/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x7ffffffd   add R = r2, r3
// EXPECT r9 = 0x7ffffffe   add R = r2, r3, 1
// EXPECT r10 = 0x80000001   sub R = r2, r3
// EXPECT r11 = 0x80000000   sub R = r2, r3, 1
// EXPECT r12 = 0x2e   sub R = 44, r3
// EXPECT r13 = 0x2c   and R = 44, r3
// EXPECT r14 = 0xfffffffffffffffe   or R = 44, r3
// EXPECT r15 = 0xffffffffffffffd2   xor R = 44, r3
// EXPECT r16 = 0x0   andcm R = 44, r3
// EXPECT r17 = 0x7ffffffe   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0xffffffff80000001   xor R = r2, r3
// EXPECT r20 = 0x1   andcm R = r2, r3
// EXPECT r21 = 0x11a5   adds R = 4519, r3
// EXPECT r22 = 0xffffffffffffdb16   addl R = -9450, r1
// EXPECT r23 = 0xfffffffc   shladd R = r2, 1, r3
// EXPECT r24 = 0x60000000fffffffc   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x1fffffffa   shladd R = r2, 2, r3
// EXPECT r26 = 0x60000000fffffffa   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x3fffffff6   shladd R = r2, 3, r3
// EXPECT r28 = 0x60000000fffffff6   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x7ffffffee   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000ffffffee   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x600000007ffffffd   addp4 R = r2, r3
// EXPECT r32 = 0x60000000000011a5   addp4 R = 4519, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0xffffffff   extr.u R = r3, 22, 32
// EXPECT r37 = 0xffffffffffffffff   extr R = r3, 22, 32
// EXPECT r38 = 0x1fffffffc00000   dep.z R = r2, 22, 32
// EXPECT r39 = 0x1c800000   dep.z R = 114, 22, 32
// EXPECT r40 = 0xfffffffffffffffe   dep R = r2, r3, 22, 9
// EXPECT r41 = 0xffc00000003ffffe   dep R = 0, r3, 22, 32
// EXPECT r42 = 0x1ffffffffffffff   shrp R = r2, r3, 38
// EXPECT r43 = 0xffffffc000000000   shl R = r2, 38
// EXPECT r44 = 0x3ffffff   shr.u R = r3, 38
// EXPECT r45 = 0xffffffffffffffff   shr R = r3, 38
// EXPECT r46 = 0x3ffffffffe   extr.u R = r3, 0, 38
// EXPECT r47 = 0xfffffffffffffffe   extr R = r3, 0, 38
// EXPECT r48 = 0x7fffffff   dep.z R = r2, 0, 38
// EXPECT r49 = 0x1f   dep.z R = 31, 0, 38
// EXPECT r50 = 0xffffffffffffffff   dep R = r2, r3, 0, 7
// EXPECT r51 = 0xffffffffffffffff   dep R = -1, r3, 0, 38
// EXPECT r52 = 0x1ffffffffff   shrp R = r2, r3, 54
// EXPECT r53 = 0xffc0000000000000   shl R = r2, 54
// EXPECT r54 = 0x3ff   shr.u R = r3, 54
// EXPECT r55 = 0xffffffffffffffff   shr R = r3, 54
// EXPECT r56 = 0xfffffff   extr.u R = r3, 36, 56
// EXPECT r57 = 0xffffffffffffffff   extr R = r3, 36, 56
// EXPECT r58 = 0xfffffff000000000   dep.z R = r2, 36, 56
// EXPECT r59 = 0x67000000000   dep.z R = 103, 36, 56
// EXPECT r60 = 0xfffffffffffffffe   dep R = r2, r3, 36, 6
// EXPECT r61 = 0xffffffffe   dep R = 0, r3, 36, 56
// EXPECT r62 = 0xffffffffffffff   shrp R = r2, r3, 39
// EXPECT r63 = 0xffffff8000000000   shl R = r2, 39
// EXPECT r64 = 0x1ffffff   shr.u R = r3, 39
// EXPECT r65 = 0xffffffffffffffff   shr R = r3, 39
// EXPECT r66 = 0x3f   extr.u R = r3, 33, 6
// EXPECT r67 = 0xffffffffffffffff   extr R = r3, 33, 6
// EXPECT r68 = 0x7e00000000   dep.z R = r2, 33, 6
// EXPECT r69 = 0x5200000000   dep.z R = -87, 33, 6
// EXPECT r70 = 0xfffffffffffffffe   dep R = r2, r3, 33, 2
// EXPECT r71 = 0xfffffffffffffffe   dep R = -1, r3, 33, 6
// EXPECT r72 = 0xfffffffffffffff   shrp R = r2, r3, 35
// EXPECT r73 = 0xfffffff800000000   shl R = r2, 35
// EXPECT r74 = 0x1fffffff   shr.u R = r3, 35
// EXPECT r75 = 0xffffffffffffffff   shr R = r3, 35
// EXPECT r76 = 0xf   extr.u R = r3, 60, 44
// EXPECT r77 = 0xffffffffffffffff   extr R = r3, 60, 44
// EXPECT r78 = 0xf000000000000000   dep.z R = r2, 60, 44
// EXPECT r79 = 0xa000000000000000   dep.z R = -54, 60, 44
// EXPECT r80 = 0xfffffffffffffffe   dep R = r2, r3, 60, 7
// EXPECT r81 = 0xffffffffffffffe   dep R = 0, r3, 60, 44
// EXPECT r82 = 0x7ffffffffff   shrp R = r2, r3, 52
// EXPECT r83 = 0xfff0000000000000   shl R = r2, 52
// EXPECT r84 = 0xfff   shr.u R = r3, 52
// EXPECT r85 = 0xffffffffffffffff   shr R = r3, 52
// EXPECT r86 = 0x7fffffffff   extr.u R = r3, 25, 57
// EXPECT r87 = 0xffffffffffffffff   extr R = r3, 25, 57
// EXPECT r88 = 0xfffffffe000000   dep.z R = r2, 25, 57
// EXPECT r89 = 0x1a000000   dep.z R = 13, 25, 57
// EXPECT r90 = 0xfffffffffffffffe   dep R = r2, r3, 25, 6
// EXPECT r91 = 0xfffffffffffffffe   dep R = -1, r3, 25, 57
// EXPECT r92 = 0xffffffffff   shrp R = r2, r3, 55
// EXPECT r93 = 0xff80000000000000   shl R = r2, 55
// EXPECT r94 = 0x1ff   shr.u R = r3, 55
// EXPECT r95 = 0xffffffffffffffff   shr R = r3, 55
// EXPECT r96 = 0xfffffffffffffffe   sxt1 R = r3
// EXPECT r97 = 0xfffffffffffffffe   sxt2 R = r3
// EXPECT r98 = 0xfffffffffffffffe   sxt4 R = r3
// EXPECT r99 = 0xfe   zxt1 R = r3
// EXPECT r100 = 0xfffe   zxt2 R = r3
// EXPECT r101 = 0xfffffffe   zxt4 R = r3
// EXPECT r102 = 0x3f   popcnt R = r3
// EXPECT p6 = 0   tbit.z P1, P2 = r3, 41
// EXPECT p7 = 1
// EXPECT p8 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp4.geu P1, P2 = -119, r3
// EXPECT p11 = 1
// EXPECT p12 = 0   tbit.z P1, P2 = r3, 36
// EXPECT p13 = 1
// EXPECT p14 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p17 = 1
// EXPECT p18 = 0   tbit.z P1, P2 = r3, 41
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p21 = 0
// EXPECT p22 = 0   cmp4.le P1, P2 = r2, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   tbit.nz P1, P2 = r3, 36
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p31 = 1
// EXPECT p32 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p33 = 0
// EXPECT p34 = 0   cmp4.eq P1, P2 = -38, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p37 = 1
// EXPECT p38 = 1   cmp4.ne P1, P2 = 20, r3
// EXPECT p39 = 0
// EXPECT p40 = 0   cmp.eq P1, P2 = 36, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp.lt P1, P2 = 37, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp.leu P1, P2 = -11, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp4.lt P1, P2 = 107, r3
// EXPECT p49 = 1
// EXPECT p50 = 1   tbit.nz P1, P2 = r3, 19
// EXPECT p51 = 0
// EXPECT p52 = 1   cmp4.ltu P1, P2 = -115, r3
// EXPECT p53 = 0
// EXPECT p54 = 1   cmp.le P1, P2 = -97, r3
// EXPECT p55 = 0
// EXPECT p56 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp.ne P1, P2 = -27, r3
// EXPECT p59 = 0
// EXPECT p60 = 1   cmp4.le P1, P2 = -107, r3
// EXPECT p61 = 0
// EXPECT p62 = 1   cmp.ltu P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x7fffffff
  movl r3 = 0xfffffffffffffffe
  movl r4 = 0xffffffffffffffff
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 44, r3
  and r13 = 44, r3
  or r14 = 44, r3
  xor r15 = 44, r3
  andcm r16 = 44, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 4519, r3
  addl r22 = -9450, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 4519, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 22, 32
  extr r37 = r3, 22, 32
  dep.z r38 = r2, 22, 32
  dep.z r39 = 114, 22, 32
  dep r40 = r2, r3, 22, 9
  dep r41 = 0, r3, 22, 32
  shrp r42 = r2, r3, 38
  shl r43 = r2, 38
  shr.u r44 = r3, 38
  shr r45 = r3, 38
  extr.u r46 = r3, 0, 38
  extr r47 = r3, 0, 38
  dep.z r48 = r2, 0, 38
  dep.z r49 = 31, 0, 38
  dep r50 = r2, r3, 0, 7
  dep r51 = -1, r3, 0, 38
  shrp r52 = r2, r3, 54
  shl r53 = r2, 54
  shr.u r54 = r3, 54
  shr r55 = r3, 54
  extr.u r56 = r3, 36, 56
  extr r57 = r3, 36, 56
  dep.z r58 = r2, 36, 56
  dep.z r59 = 103, 36, 56
  dep r60 = r2, r3, 36, 6
  dep r61 = 0, r3, 36, 56
  shrp r62 = r2, r3, 39
  shl r63 = r2, 39
  shr.u r64 = r3, 39
  shr r65 = r3, 39
  extr.u r66 = r3, 33, 6
  extr r67 = r3, 33, 6
  dep.z r68 = r2, 33, 6
  dep.z r69 = -87, 33, 6
  dep r70 = r2, r3, 33, 2
  dep r71 = -1, r3, 33, 6
  shrp r72 = r2, r3, 35
  shl r73 = r2, 35
  shr.u r74 = r3, 35
  shr r75 = r3, 35
  extr.u r76 = r3, 60, 44
  extr r77 = r3, 60, 44
  dep.z r78 = r2, 60, 44
  dep.z r79 = -54, 60, 44
  dep r80 = r2, r3, 60, 7
  dep r81 = 0, r3, 60, 44
  shrp r82 = r2, r3, 52
  shl r83 = r2, 52
  shr.u r84 = r3, 52
  shr r85 = r3, 52
  extr.u r86 = r3, 25, 57
  extr r87 = r3, 25, 57
  dep.z r88 = r2, 25, 57
  dep.z r89 = 13, 25, 57
  dep r90 = r2, r3, 25, 6
  dep r91 = -1, r3, 25, 57
  shrp r92 = r2, r3, 55
  shl r93 = r2, 55
  shr.u r94 = r3, 55
  shr r95 = r3, 55
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  tbit.z p6, p7 = r3, 41
  cmp4.ltu p8, p9 = r2, r3
  cmp4.geu p10, p11 = -119, r3
  tbit.z p12, p13 = r3, 36
  cmp.lt p14, p15 = r2, r3
  cmp.geu p16, p17 = r2, r3
  tbit.z p18, p19 = r3, 41
  cmp.gt p20, p21 = r2, r3
  cmp4.le p22, p23 = r2, r3
  tbit.nz p24, p25 = r3, 36
  cmp.le p26, p27 = r2, r3
  cmp.ge p28, p29 = r2, r3
  cmp4.lt p30, p31 = r2, r3
  cmp.leu p32, p33 = r2, r3
  cmp4.eq p34, p35 = -38, r3
  cmp.eq p36, p37 = r2, r3
  cmp4.ne p38, p39 = 20, r3
  cmp.eq p40, p41 = 36, r3
  cmp.gtu p42, p43 = r2, r3
  cmp.lt p44, p45 = 37, r3
  cmp.leu p46, p47 = -11, r3
  cmp4.lt p48, p49 = 107, r3
  tbit.nz p50, p51 = r3, 19
  cmp4.ltu p52, p53 = -115, r3
  cmp.le p54, p55 = -97, r3
  cmp4.geu p56, p57 = r2, r3
  cmp.ne p58, p59 = -27, r3
  cmp4.le p60, p61 = -107, r3
  cmp.ltu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
