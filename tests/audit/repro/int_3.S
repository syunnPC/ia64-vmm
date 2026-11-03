/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x97524d6af51e8720   add R = r2, r3
// EXPECT r9 = 0x97524d6af51e8721   add R = r2, r3, 1
// EXPECT r10 = 0x97524d6af51e8724   sub R = r2, r3
// EXPECT r11 = 0x97524d6af51e8723   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffffffffff90   sub R = -114, r3
// EXPECT r13 = 0xffffffffffffff8e   and R = -114, r3
// EXPECT r14 = 0xfffffffffffffffe   or R = -114, r3
// EXPECT r15 = 0x70   xor R = -114, r3
// EXPECT r16 = 0x0   andcm R = -114, r3
// EXPECT r17 = 0x97524d6af51e8722   and R = r2, r3
// EXPECT r18 = 0xfffffffffffffffe   or R = r2, r3
// EXPECT r19 = 0x68adb2950ae178dc   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0x3ca   adds R = 972, r3
// EXPECT r22 = 0xfffffffffff4e072   addl R = -728974, r1
// EXPECT r23 = 0x2ea49ad5ea3d0e42   shladd R = r2, 1, r3
// EXPECT r24 = 0x60000000ea3d0e42   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x5d4935abd47a1c86   shladd R = r2, 2, r3
// EXPECT r26 = 0x60000000d47a1c86   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xba926b57a8f4390e   shladd R = r2, 3, r3
// EXPECT r28 = 0x60000000a8f4390e   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x7524d6af51e8721e   shladd R = r2, 4, r3
// EXPECT r30 = 0x6000000051e8721e   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x60000000f51e8720   addp4 R = r2, r3
// EXPECT r32 = 0x60000000000003ca   addp4 R = 972, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0x3fff   extr.u R = r3, 41, 14
// EXPECT r37 = 0xffffffffffffffff   extr R = r3, 41, 14
// EXPECT r38 = 0xe440000000000   dep.z R = r2, 41, 14
// EXPECT r39 = 0x7fd80000000000   dep.z R = -20, 41, 14
// EXPECT r40 = 0xfffe45fffffffffe   dep R = r2, r3, 41, 9
// EXPECT r41 = 0xfffffffffffffffe   dep R = -1, r3, 41, 14
// EXPECT r42 = 0xe45ffffffffffff   shrp R = r2, r3, 15
// EXPECT r43 = 0x26b57a8f43910000   shl R = r2, 15
// EXPECT r44 = 0x1ffffffffffff   shr.u R = r3, 15
// EXPECT r45 = 0xffffffffffffffff   shr R = r3, 15
// EXPECT r46 = 0xffffffffffffff   extr.u R = r3, 8, 62
// EXPECT r47 = 0xffffffffffffffff   extr R = r3, 8, 62
// EXPECT r48 = 0x524d6af51e872200   dep.z R = r2, 8, 62
// EXPECT r49 = 0x7700   dep.z R = 119, 8, 62
// EXPECT r50 = 0xfffffffffffffafe   dep R = r2, r3, 8, 3
// EXPECT r51 = 0xfffffffffffffffe   dep R = -1, r3, 8, 62
// EXPECT r52 = 0x22ffffffffffffff   shrp R = r2, r3, 8
// EXPECT r53 = 0x524d6af51e872200   shl R = r2, 8
// EXPECT r54 = 0xffffffffffffff   shr.u R = r3, 8
// EXPECT r55 = 0xffffffffffffffff   shr R = r3, 8
// EXPECT r56 = 0xfff   extr.u R = r3, 52, 20
// EXPECT r57 = 0xffffffffffffffff   extr R = r3, 52, 20
// EXPECT r58 = 0x7220000000000000   dep.z R = r2, 52, 20
// EXPECT r59 = 0xf8a0000000000000   dep.z R = -118, 52, 20
// EXPECT r60 = 0xf22ffffffffffffe   dep R = r2, r3, 52, 10
// EXPECT r61 = 0xfffffffffffffffe   dep R = -1, r3, 52, 20
// EXPECT r62 = 0x926b57a8f43917ff   shrp R = r2, r3, 53
// EXPECT r63 = 0xe440000000000000   shl R = r2, 53
// EXPECT r64 = 0x7ff   shr.u R = r3, 53
// EXPECT r65 = 0xffffffffffffffff   shr R = r3, 53
// EXPECT r66 = 0x3f   extr.u R = r3, 15, 6
// EXPECT r67 = 0xffffffffffffffff   extr R = r3, 15, 6
// EXPECT r68 = 0x110000   dep.z R = r2, 15, 6
// EXPECT r69 = 0xb8000   dep.z R = -105, 15, 6
// EXPECT r70 = 0xfffffffff3917ffe   dep R = r2, r3, 15, 13
// EXPECT r71 = 0xfffffffffffffffe   dep R = -1, r3, 15, 6
// EXPECT r72 = 0x5ea3d0e45fffffff   shrp R = r2, r3, 35
// EXPECT r73 = 0xa8f4391000000000   shl R = r2, 35
// EXPECT r74 = 0x1fffffff   shr.u R = r3, 35
// EXPECT r75 = 0xffffffffffffffff   shr R = r3, 35
// EXPECT r76 = 0x1f   extr.u R = r3, 30, 5
// EXPECT r77 = 0xffffffffffffffff   extr R = r3, 30, 5
// EXPECT r78 = 0x80000000   dep.z R = r2, 30, 5
// EXPECT r79 = 0x780000000   dep.z R = 30, 30, 5
// EXPECT r80 = 0xffffffffbffffffe   dep R = r2, r3, 30, 1
// EXPECT r81 = 0xfffffff83ffffffe   dep R = 0, r3, 30, 5
// EXPECT r82 = 0x3917ffffffffffff   shrp R = r2, r3, 13
// EXPECT r83 = 0x49ad5ea3d0e44000   shl R = r2, 13
// EXPECT r84 = 0x7ffffffffffff   shr.u R = r3, 13
// EXPECT r85 = 0xffffffffffffffff   shr R = r3, 13
// EXPECT r86 = 0x3ffffff   extr.u R = r3, 4, 26
// EXPECT r87 = 0xffffffffffffffff   extr R = r3, 4, 26
// EXPECT r88 = 0x11e87220   dep.z R = r2, 4, 26
// EXPECT r89 = 0x500   dep.z R = 80, 4, 26
// EXPECT r90 = 0xfffffffffffff22e   dep R = r2, r3, 4, 10
// EXPECT r91 = 0xfffffffffffffffe   dep R = -1, r3, 4, 26
// EXPECT r92 = 0xd0e45fffffffffff   shrp R = r2, r3, 19
// EXPECT r93 = 0x6b57a8f439100000   shl R = r2, 19
// EXPECT r94 = 0x1fffffffffff   shr.u R = r3, 19
// EXPECT r95 = 0xffffffffffffffff   shr R = r3, 19
// EXPECT r96 = 0xfffffffffffffffe   sxt1 R = r3
// EXPECT r97 = 0xfffffffffffffffe   sxt2 R = r3
// EXPECT r98 = 0xfffffffffffffffe   sxt4 R = r3
// EXPECT r99 = 0xfe   zxt1 R = r3
// EXPECT r100 = 0xfffe   zxt2 R = r3
// EXPECT r101 = 0xfffffffe   zxt4 R = r3
// EXPECT r102 = 0x3f   popcnt R = r3
// EXPECT p6 = 1   cmp.ne P1, P2 = 45, r3
// EXPECT p7 = 0
// EXPECT p8 = 1   tbit.nz P1, P2 = r3, 40
// EXPECT p9 = 0
// EXPECT p10 = 1   tbit.nz P1, P2 = r3, 17
// EXPECT p11 = 0
// EXPECT p12 = 0   cmp4.eq P1, P2 = 10, r3
// EXPECT p13 = 1
// EXPECT p14 = 0   cmp4.le P1, P2 = 95, r3
// EXPECT p15 = 1
// EXPECT p16 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   tbit.nz P1, P2 = r3, 2
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 1   cmp.ge P1, P2 = 69, r3
// EXPECT p23 = 0
// EXPECT p24 = 1   cmp4.leu P1, P2 = 4, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp.eq P1, P2 = -107, r3
// EXPECT p27 = 1
// EXPECT p28 = 0   tbit.z P1, P2 = r3, 17
// EXPECT p29 = 1
// EXPECT p30 = 0   cmp4.ge P1, P2 = -123, r3
// EXPECT p31 = 1
// EXPECT p32 = 1   cmp4.ne P1, P2 = 92, r3
// EXPECT p33 = 0
// EXPECT p34 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   tbit.z P1, P2 = r3, 2
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.geu P1, P2 = -76, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   tbit.z P1, P2 = r3, 48
// EXPECT p43 = 1
// EXPECT p44 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   cmp.lt P1, P2 = 32, r3
// EXPECT p47 = 1
// EXPECT p48 = 1   cmp.leu P1, P2 = 65, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   cmp4.ltu P1, P2 = 26, r3
// EXPECT p51 = 0
// EXPECT p52 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p53 = 0
// EXPECT p54 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p59 = 0
// EXPECT p60 = 1   cmp.le P1, P2 = -58, r3
// EXPECT p61 = 0
// EXPECT p62 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0x97524d6af51e8722
  movl r3 = 0xfffffffffffffffe
  movl r4 = 0x40
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -114, r3
  and r13 = -114, r3
  or r14 = -114, r3
  xor r15 = -114, r3
  andcm r16 = -114, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 972, r3
  addl r22 = -728974, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 972, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 41, 14
  extr r37 = r3, 41, 14
  dep.z r38 = r2, 41, 14
  dep.z r39 = -20, 41, 14
  dep r40 = r2, r3, 41, 9
  dep r41 = -1, r3, 41, 14
  shrp r42 = r2, r3, 15
  shl r43 = r2, 15
  shr.u r44 = r3, 15
  shr r45 = r3, 15
  extr.u r46 = r3, 8, 62
  extr r47 = r3, 8, 62
  dep.z r48 = r2, 8, 62
  dep.z r49 = 119, 8, 62
  dep r50 = r2, r3, 8, 3
  dep r51 = -1, r3, 8, 62
  shrp r52 = r2, r3, 8
  shl r53 = r2, 8
  shr.u r54 = r3, 8
  shr r55 = r3, 8
  extr.u r56 = r3, 52, 20
  extr r57 = r3, 52, 20
  dep.z r58 = r2, 52, 20
  dep.z r59 = -118, 52, 20
  dep r60 = r2, r3, 52, 10
  dep r61 = -1, r3, 52, 20
  shrp r62 = r2, r3, 53
  shl r63 = r2, 53
  shr.u r64 = r3, 53
  shr r65 = r3, 53
  extr.u r66 = r3, 15, 6
  extr r67 = r3, 15, 6
  dep.z r68 = r2, 15, 6
  dep.z r69 = -105, 15, 6
  dep r70 = r2, r3, 15, 13
  dep r71 = -1, r3, 15, 6
  shrp r72 = r2, r3, 35
  shl r73 = r2, 35
  shr.u r74 = r3, 35
  shr r75 = r3, 35
  extr.u r76 = r3, 30, 5
  extr r77 = r3, 30, 5
  dep.z r78 = r2, 30, 5
  dep.z r79 = 30, 30, 5
  dep r80 = r2, r3, 30, 1
  dep r81 = 0, r3, 30, 5
  shrp r82 = r2, r3, 13
  shl r83 = r2, 13
  shr.u r84 = r3, 13
  shr r85 = r3, 13
  extr.u r86 = r3, 4, 26
  extr r87 = r3, 4, 26
  dep.z r88 = r2, 4, 26
  dep.z r89 = 80, 4, 26
  dep r90 = r2, r3, 4, 10
  dep r91 = -1, r3, 4, 26
  shrp r92 = r2, r3, 19
  shl r93 = r2, 19
  shr.u r94 = r3, 19
  shr r95 = r3, 19
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.ne p6, p7 = 45, r3
  tbit.nz p8, p9 = r3, 40
  tbit.nz p10, p11 = r3, 17
  cmp4.eq p12, p13 = 10, r3
  cmp4.le p14, p15 = 95, r3
  cmp4.ne p16, p17 = r2, r3
  tbit.nz p18, p19 = r3, 2
  cmp4.geu p20, p21 = r2, r3
  cmp.ge p22, p23 = 69, r3
  cmp4.leu p24, p25 = 4, r3
  cmp.eq p26, p27 = -107, r3
  tbit.z p28, p29 = r3, 17
  cmp4.ge p30, p31 = -123, r3
  cmp4.ne p32, p33 = 92, r3
  cmp.ge p34, p35 = r2, r3
  tbit.z p36, p37 = r3, 2
  cmp.geu p38, p39 = -76, r3
  cmp.geu p40, p41 = r2, r3
  tbit.z p42, p43 = r3, 48
  cmp.leu p44, p45 = r2, r3
  cmp.lt p46, p47 = 32, r3
  cmp.leu p48, p49 = 65, r3
  cmp4.ltu p50, p51 = 26, r3
  cmp4.le p52, p53 = r2, r3
  cmp4.ltu p54, p55 = r2, r3
  cmp4.gt p56, p57 = r2, r3
  cmp.le p58, p59 = r2, r3
  cmp.le p60, p61 = -58, r3
  cmp.gt p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
