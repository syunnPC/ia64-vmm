/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xfffffffffffffffe   add R = r2, r3
// EXPECT r9 = 0xffffffffffffffff   add R = r2, r3, 1
// EXPECT r10 = 0x0   sub R = r2, r3
// EXPECT r11 = 0xffffffffffffffff   sub R = r2, r3, 1
// EXPECT r12 = 0xb   sub R = 10, r3
// EXPECT r13 = 0xa   and R = 10, r3
// EXPECT r14 = 0xffffffffffffffff   or R = 10, r3
// EXPECT r15 = 0xfffffffffffffff5   xor R = 10, r3
// EXPECT r16 = 0x0   andcm R = 10, r3
// EXPECT r17 = 0xffffffffffffffff   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0x0   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0xfffffffffffffdc3   adds R = -572, r3
// EXPECT r22 = 0x19771b   addl R = 1668891, r1
// EXPECT r23 = 0xfffffffffffffffd   shladd R = r2, 1, r3
// EXPECT r24 = 0x60000000fffffffd   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0xfffffffffffffffb   shladd R = r2, 2, r3
// EXPECT r26 = 0x60000000fffffffb   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xfffffffffffffff7   shladd R = r2, 3, r3
// EXPECT r28 = 0x60000000fffffff7   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xffffffffffffffef   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000ffffffef   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x60000000fffffffe   addp4 R = r2, r3
// EXPECT r32 = 0x60000000fffffdc3   addp4 R = -572, r3
// EXPECT r33 = 0xffffffff80000000   shl R = r2, r4
// EXPECT r34 = 0x1ffffffff   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0x3ffffff   extr.u R = r3, 16, 26
// EXPECT r37 = 0xffffffffffffffff   extr R = r3, 16, 26
// EXPECT r38 = 0x3ffffff0000   dep.z R = r2, 16, 26
// EXPECT r39 = 0x3ffff860000   dep.z R = -122, 16, 26
// EXPECT r40 = 0xffffffffffffffff   dep R = r2, r3, 16, 10
// EXPECT r41 = 0xffffffffffffffff   dep R = -1, r3, 16, 26
// EXPECT r42 = 0xffffffffffffffff   shrp R = r2, r3, 10
// EXPECT r43 = 0xfffffffffffffc00   shl R = r2, 10
// EXPECT r44 = 0x3fffffffffffff   shr.u R = r3, 10
// EXPECT r45 = 0xffffffffffffffff   shr R = r3, 10
// EXPECT r46 = 0xffff   extr.u R = r3, 35, 16
// EXPECT r47 = 0xffffffffffffffff   extr R = r3, 35, 16
// EXPECT r48 = 0x7fff800000000   dep.z R = r2, 35, 16
// EXPECT r49 = 0x2a800000000   dep.z R = 85, 35, 16
// EXPECT r50 = 0xffffffffffffffff   dep R = r2, r3, 35, 6
// EXPECT r51 = 0xfff80007ffffffff   dep R = 0, r3, 35, 16
// EXPECT r52 = 0xffffffffffffffff   shrp R = r2, r3, 47
// EXPECT r53 = 0xffff800000000000   shl R = r2, 47
// EXPECT r54 = 0x1ffff   shr.u R = r3, 47
// EXPECT r55 = 0xffffffffffffffff   shr R = r3, 47
// EXPECT r56 = 0x1f   extr.u R = r3, 8, 5
// EXPECT r57 = 0xffffffffffffffff   extr R = r3, 8, 5
// EXPECT r58 = 0x1f00   dep.z R = r2, 8, 5
// EXPECT r59 = 0x1b00   dep.z R = 91, 8, 5
// EXPECT r60 = 0xffffffffffffffff   dep R = r2, r3, 8, 16
// EXPECT r61 = 0xffffffffffffffff   dep R = -1, r3, 8, 5
// EXPECT r62 = 0xffffffffffffffff   shrp R = r2, r3, 20
// EXPECT r63 = 0xfffffffffff00000   shl R = r2, 20
// EXPECT r64 = 0xfffffffffff   shr.u R = r3, 20
// EXPECT r65 = 0xffffffffffffffff   shr R = r3, 20
// EXPECT r66 = 0xffff   extr.u R = r3, 48, 33
// EXPECT r67 = 0xffffffffffffffff   extr R = r3, 48, 33
// EXPECT r68 = 0xffff000000000000   dep.z R = r2, 48, 33
// EXPECT r69 = 0x45000000000000   dep.z R = 69, 48, 33
// EXPECT r70 = 0xffffffffffffffff   dep R = r2, r3, 48, 6
// EXPECT r71 = 0xffffffffffff   dep R = 0, r3, 48, 33
// EXPECT r72 = 0xffffffffffffffff   shrp R = r2, r3, 22
// EXPECT r73 = 0xffffffffffc00000   shl R = r2, 22
// EXPECT r74 = 0x3ffffffffff   shr.u R = r3, 22
// EXPECT r75 = 0xffffffffffffffff   shr R = r3, 22
// EXPECT r76 = 0x1ffffff   extr.u R = r3, 39, 25
// EXPECT r77 = 0xffffffffffffffff   extr R = r3, 39, 25
// EXPECT r78 = 0xffffff8000000000   dep.z R = r2, 39, 25
// EXPECT r79 = 0x2b8000000000   dep.z R = 87, 39, 25
// EXPECT r80 = 0xffffffffffffffff   dep R = r2, r3, 39, 1
// EXPECT r81 = 0x7fffffffff   dep R = 0, r3, 39, 25
// EXPECT r82 = 0xffffffffffffffff   shrp R = r2, r3, 25
// EXPECT r83 = 0xfffffffffe000000   shl R = r2, 25
// EXPECT r84 = 0x7fffffffff   shr.u R = r3, 25
// EXPECT r85 = 0xffffffffffffffff   shr R = r3, 25
// EXPECT r86 = 0x3ffffffffff   extr.u R = r3, 7, 42
// EXPECT r87 = 0xffffffffffffffff   extr R = r3, 7, 42
// EXPECT r88 = 0x1ffffffffff80   dep.z R = r2, 7, 42
// EXPECT r89 = 0x1b80   dep.z R = 55, 7, 42
// EXPECT r90 = 0xffffffffffffffff   dep R = r2, r3, 7, 13
// EXPECT r91 = 0xfffe00000000007f   dep R = 0, r3, 7, 42
// EXPECT r92 = 0xffffffffffffffff   shrp R = r2, r3, 63
// EXPECT r93 = 0x8000000000000000   shl R = r2, 63
// EXPECT r94 = 0x1   shr.u R = r3, 63
// EXPECT r95 = 0xffffffffffffffff   shr R = r3, 63
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0xffffffffffffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0xffffffff   zxt4 R = r3
// EXPECT r102 = 0x40   popcnt R = r3
// EXPECT p6 = 1   cmp.lt P1, P2 = -23, r3
// EXPECT p7 = 0
// EXPECT p8 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p9 = 1
// EXPECT p10 = 1   cmp4.lt P1, P2 = -8, r3
// EXPECT p11 = 0
// EXPECT p12 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p13 = 1
// EXPECT p14 = 0   cmp4.gtu P1, P2 = 1, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p17 = 1
// EXPECT p18 = 0   cmp4.geu P1, P2 = -58, r3
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p21 = 0
// EXPECT p22 = 1   cmp.ltu P1, P2 = 101, r3
// EXPECT p23 = 0
// EXPECT p24 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p31 = 1
// EXPECT p32 = 0   cmp.le P1, P2 = 90, r3
// EXPECT p33 = 1
// EXPECT p34 = 1   tbit.nz P1, P2 = r3, 46
// EXPECT p35 = 0
// EXPECT p36 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p37 = 0
// EXPECT p38 = 0   tbit.z P1, P2 = r3, 46
// EXPECT p39 = 1
// EXPECT p40 = 1   tbit.nz P1, P2 = r3, 46
// EXPECT p41 = 0
// EXPECT p42 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p43 = 0
// EXPECT p44 = 1   cmp4.ge P1, P2 = 97, r3
// EXPECT p45 = 0
// EXPECT p46 = 1   cmp4.le P1, P2 = -121, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp.gtu P1, P2 = -4, r3
// EXPECT p49 = 1
// EXPECT p50 = 1   cmp4.ne P1, P2 = -10, r3
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp4.ne P1, P2 = r2, r3
// EXPECT p53 = 1
// EXPECT p54 = 1   cmp4.gt P1, P2 = 80, r3
// EXPECT p55 = 0
// EXPECT p56 = 0   tbit.z P1, P2 = r3, 46
// EXPECT p57 = 1
// EXPECT p58 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   cmp.ne P1, P2 = r2, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp4.ge P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0xffffffffffffffff
  movl r3 = 0xffffffffffffffff
  movl r4 = 0x1f
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 10, r3
  and r13 = 10, r3
  or r14 = 10, r3
  xor r15 = 10, r3
  andcm r16 = 10, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -572, r3
  addl r22 = 1668891, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -572, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 16, 26
  extr r37 = r3, 16, 26
  dep.z r38 = r2, 16, 26
  dep.z r39 = -122, 16, 26
  dep r40 = r2, r3, 16, 10
  dep r41 = -1, r3, 16, 26
  shrp r42 = r2, r3, 10
  shl r43 = r2, 10
  shr.u r44 = r3, 10
  shr r45 = r3, 10
  extr.u r46 = r3, 35, 16
  extr r47 = r3, 35, 16
  dep.z r48 = r2, 35, 16
  dep.z r49 = 85, 35, 16
  dep r50 = r2, r3, 35, 6
  dep r51 = 0, r3, 35, 16
  shrp r52 = r2, r3, 47
  shl r53 = r2, 47
  shr.u r54 = r3, 47
  shr r55 = r3, 47
  extr.u r56 = r3, 8, 5
  extr r57 = r3, 8, 5
  dep.z r58 = r2, 8, 5
  dep.z r59 = 91, 8, 5
  dep r60 = r2, r3, 8, 16
  dep r61 = -1, r3, 8, 5
  shrp r62 = r2, r3, 20
  shl r63 = r2, 20
  shr.u r64 = r3, 20
  shr r65 = r3, 20
  extr.u r66 = r3, 48, 33
  extr r67 = r3, 48, 33
  dep.z r68 = r2, 48, 33
  dep.z r69 = 69, 48, 33
  dep r70 = r2, r3, 48, 6
  dep r71 = 0, r3, 48, 33
  shrp r72 = r2, r3, 22
  shl r73 = r2, 22
  shr.u r74 = r3, 22
  shr r75 = r3, 22
  extr.u r76 = r3, 39, 25
  extr r77 = r3, 39, 25
  dep.z r78 = r2, 39, 25
  dep.z r79 = 87, 39, 25
  dep r80 = r2, r3, 39, 1
  dep r81 = 0, r3, 39, 25
  shrp r82 = r2, r3, 25
  shl r83 = r2, 25
  shr.u r84 = r3, 25
  shr r85 = r3, 25
  extr.u r86 = r3, 7, 42
  extr r87 = r3, 7, 42
  dep.z r88 = r2, 7, 42
  dep.z r89 = 55, 7, 42
  dep r90 = r2, r3, 7, 13
  dep r91 = 0, r3, 7, 42
  shrp r92 = r2, r3, 63
  shl r93 = r2, 63
  shr.u r94 = r3, 63
  shr r95 = r3, 63
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.lt p6, p7 = -23, r3
  cmp.gtu p8, p9 = r2, r3
  cmp4.lt p10, p11 = -8, r3
  cmp.gt p12, p13 = r2, r3
  cmp4.gtu p14, p15 = 1, r3
  cmp.ltu p16, p17 = r2, r3
  cmp4.geu p18, p19 = -58, r3
  cmp.le p20, p21 = r2, r3
  cmp.ltu p22, p23 = 101, r3
  cmp.geu p24, p25 = r2, r3
  cmp4.gt p26, p27 = r2, r3
  cmp4.le p28, p29 = r2, r3
  cmp.lt p30, p31 = r2, r3
  cmp.le p32, p33 = 90, r3
  tbit.nz p34, p35 = r3, 46
  cmp.leu p36, p37 = r2, r3
  tbit.z p38, p39 = r3, 46
  tbit.nz p40, p41 = r3, 46
  cmp.ge p42, p43 = r2, r3
  cmp4.ge p44, p45 = 97, r3
  cmp4.le p46, p47 = -121, r3
  cmp.gtu p48, p49 = -4, r3
  cmp4.ne p50, p51 = -10, r3
  cmp4.ne p52, p53 = r2, r3
  cmp4.gt p54, p55 = 80, r3
  tbit.z p56, p57 = r3, 46
  cmp4.lt p58, p59 = r2, r3
  cmp.ne p60, p61 = r2, r3
  cmp4.ge p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
