/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xb5c318e9fb34ccc4   add R = r2, r3
// EXPECT r9 = 0xb5c318e9fb34ccc5   add R = r2, r3, 1
// EXPECT r10 = 0x4a3ce71604cb333a   sub R = r2, r3
// EXPECT r11 = 0x4a3ce71604cb3339   sub R = r2, r3, 1
// EXPECT r12 = 0x4a3ce71604cb3377   sub R = 60, r3
// EXPECT r13 = 0x4   and R = 60, r3
// EXPECT r14 = 0xb5c318e9fb34ccfd   or R = 60, r3
// EXPECT r15 = 0xb5c318e9fb34ccf9   xor R = 60, r3
// EXPECT r16 = 0x38   andcm R = 60, r3
// EXPECT r17 = 0xb5c318e9fb34ccc5   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0x4a3ce71604cb333a   xor R = r2, r3
// EXPECT r20 = 0x4a3ce71604cb333a   andcm R = r2, r3
// EXPECT r21 = 0xb5c318e9fb34c2c6   adds R = -2559, r3
// EXPECT r22 = 0xfffffffffffba75f   addl R = -284833, r1
// EXPECT r23 = 0xb5c318e9fb34ccc3   shladd R = r2, 1, r3
// EXPECT r24 = 0x60000000fb34ccc3   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0xb5c318e9fb34ccc1   shladd R = r2, 2, r3
// EXPECT r26 = 0x60000000fb34ccc1   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xb5c318e9fb34ccbd   shladd R = r2, 3, r3
// EXPECT r28 = 0x60000000fb34ccbd   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xb5c318e9fb34ccb5   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000fb34ccb5   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x60000000fb34ccc4   addp4 R = r2, r3
// EXPECT r32 = 0x60000000fb34c2c6   addp4 R = -2559, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0x2d70c   extr.u R = r3, 46, 48
// EXPECT r37 = 0xfffffffffffed70c   extr R = r3, 46, 48
// EXPECT r38 = 0xffffc00000000000   dep.z R = r2, 46, 48
// EXPECT r39 = 0x4000000000000   dep.z R = 16, 46, 48
// EXPECT r40 = 0xb5ffd8e9fb34ccc5   dep R = r2, r3, 46, 11
// EXPECT r41 = 0x18e9fb34ccc5   dep R = 0, r3, 46, 48
// EXPECT r42 = 0xfffffffffffffffb   shrp R = r2, r3, 60
// EXPECT r43 = 0xf000000000000000   shl R = r2, 60
// EXPECT r44 = 0xb   shr.u R = r3, 60
// EXPECT r45 = 0xfffffffffffffffb   shr R = r3, 60
// EXPECT r46 = 0x2d70c63a7ec   extr.u R = r3, 22, 62
// EXPECT r47 = 0xfffffed70c63a7ec   extr R = r3, 22, 62
// EXPECT r48 = 0xffffffffffc00000   dep.z R = r2, 22, 62
// EXPECT r49 = 0x19400000   dep.z R = 101, 22, 62
// EXPECT r50 = 0xb5c318e9fff4ccc5   dep R = r2, r3, 22, 6
// EXPECT r51 = 0x34ccc5   dep R = 0, r3, 22, 62
// EXPECT r52 = 0xfffffed70c63a7ec   shrp R = r2, r3, 22
// EXPECT r53 = 0xffffffffffc00000   shl R = r2, 22
// EXPECT r54 = 0x2d70c63a7ec   shr.u R = r3, 22
// EXPECT r55 = 0xfffffed70c63a7ec   shr R = r3, 22
// EXPECT r56 = 0x5ae18c   extr.u R = r3, 41, 26
// EXPECT r57 = 0xffffffffffdae18c   extr R = r3, 41, 26
// EXPECT r58 = 0xfffffe0000000000   dep.z R = r2, 41, 26
// EXPECT r59 = 0xffff740000000000   dep.z R = -70, 41, 26
// EXPECT r60 = 0xb5c31ee9fb34ccc5   dep R = r2, r3, 41, 3
// EXPECT r61 = 0xe9fb34ccc5   dep R = 0, r3, 41, 26
// EXPECT r62 = 0xfffffffffffffed7   shrp R = r2, r3, 54
// EXPECT r63 = 0xffc0000000000000   shl R = r2, 54
// EXPECT r64 = 0x2d7   shr.u R = r3, 54
// EXPECT r65 = 0xfffffffffffffed7   shr R = r3, 54
// EXPECT r66 = 0xe9fb34ccc   extr.u R = r3, 4, 36
// EXPECT r67 = 0xfffffffe9fb34ccc   extr R = r3, 4, 36
// EXPECT r68 = 0xfffffffff0   dep.z R = r2, 4, 36
// EXPECT r69 = 0xf0   dep.z R = 15, 4, 36
// EXPECT r70 = 0xb5c318e9fb34fff5   dep R = r2, r3, 4, 11
// EXPECT r71 = 0xb5c3180000000005   dep R = 0, r3, 4, 36
// EXPECT r72 = 0xffffffffffff6b86   shrp R = r2, r3, 47
// EXPECT r73 = 0xffff800000000000   shl R = r2, 47
// EXPECT r74 = 0x16b86   shr.u R = r3, 47
// EXPECT r75 = 0xffffffffffff6b86   shr R = r3, 47
// EXPECT r76 = 0xb5c318e   extr.u R = r3, 36, 40
// EXPECT r77 = 0xfffffffffb5c318e   extr R = r3, 36, 40
// EXPECT r78 = 0xfffffff000000000   dep.z R = r2, 36, 40
// EXPECT r79 = 0x68000000000   dep.z R = 104, 36, 40
// EXPECT r80 = 0xb5c31ff9fb34ccc5   dep R = r2, r3, 36, 7
// EXPECT r81 = 0xfffffff9fb34ccc5   dep R = -1, r3, 36, 40
// EXPECT r82 = 0xfffff6b8631d3f66   shrp R = r2, r3, 19
// EXPECT r83 = 0xfffffffffff80000   shl R = r2, 19
// EXPECT r84 = 0x16b8631d3f66   shr.u R = r3, 19
// EXPECT r85 = 0xfffff6b8631d3f66   shr R = r3, 19
// EXPECT r86 = 0xd9a6   extr.u R = r3, 13, 16
// EXPECT r87 = 0xffffffffffffd9a6   extr R = r3, 13, 16
// EXPECT r88 = 0x1fffe000   dep.z R = r2, 13, 16
// EXPECT r89 = 0xf8000   dep.z R = 124, 13, 16
// EXPECT r90 = 0xb5c318e9fb3fecc5   dep R = r2, r3, 13, 8
// EXPECT r91 = 0xb5c318e9ffffecc5   dep R = -1, r3, 13, 16
// EXPECT r92 = 0xffffed70c63a7ecd   shrp R = r2, r3, 18
// EXPECT r93 = 0xfffffffffffc0000   shl R = r2, 18
// EXPECT r94 = 0x2d70c63a7ecd   shr.u R = r3, 18
// EXPECT r95 = 0xffffed70c63a7ecd   shr R = r3, 18
// EXPECT r96 = 0xffffffffffffffc5   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffccc5   sxt2 R = r3
// EXPECT r98 = 0xfffffffffb34ccc5   sxt4 R = r3
// EXPECT r99 = 0xc5   zxt1 R = r3
// EXPECT r100 = 0xccc5   zxt2 R = r3
// EXPECT r101 = 0xfb34ccc5   zxt4 R = r3
// EXPECT r102 = 0x22   popcnt R = r3
// EXPECT p6 = 0   cmp4.le P1, P2 = 99, r3
// EXPECT p7 = 1
// EXPECT p8 = 1   cmp4.gtu P1, P2 = -120, r3
// EXPECT p9 = 0
// EXPECT p10 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p11 = 0
// EXPECT p12 = 1   tbit.z P1, P2 = r3, 33
// EXPECT p13 = 0
// EXPECT p14 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp.gt P1, P2 = 85, r3
// EXPECT p17 = 0
// EXPECT p18 = 0   cmp.lt P1, P2 = -43, r3
// EXPECT p19 = 1
// EXPECT p20 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   tbit.nz P1, P2 = r3, 33
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp4.gt P1, P2 = 49, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp4.geu P1, P2 = 123, r3
// EXPECT p31 = 1
// EXPECT p32 = 0   cmp.gtu P1, P2 = 73, r3
// EXPECT p33 = 1
// EXPECT p34 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p35 = 0
// EXPECT p36 = 1   tbit.nz P1, P2 = r3, 24
// EXPECT p37 = 0
// EXPECT p38 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   tbit.z P1, P2 = r3, 24
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp4.le P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p49 = 1
// EXPECT p50 = 1   cmp4.ge P1, P2 = 65, r3
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp4.eq P1, P2 = -13, r3
// EXPECT p53 = 1
// EXPECT p54 = 0   tbit.nz P1, P2 = r3, 4
// EXPECT p55 = 1
// EXPECT p56 = 0   cmp4.leu P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp.geu P1, P2 = -45, r3
// EXPECT p59 = 0
// EXPECT p60 = 1   cmp4.ne P1, P2 = -122, r3
// EXPECT p61 = 0
// EXPECT p62 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0xffffffffffffffff
  movl r3 = 0xb5c318e9fb34ccc5
  movl r4 = 0xffffffffffffffff
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 60, r3
  and r13 = 60, r3
  or r14 = 60, r3
  xor r15 = 60, r3
  andcm r16 = 60, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -2559, r3
  addl r22 = -284833, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -2559, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 46, 48
  extr r37 = r3, 46, 48
  dep.z r38 = r2, 46, 48
  dep.z r39 = 16, 46, 48
  dep r40 = r2, r3, 46, 11
  dep r41 = 0, r3, 46, 48
  shrp r42 = r2, r3, 60
  shl r43 = r2, 60
  shr.u r44 = r3, 60
  shr r45 = r3, 60
  extr.u r46 = r3, 22, 62
  extr r47 = r3, 22, 62
  dep.z r48 = r2, 22, 62
  dep.z r49 = 101, 22, 62
  dep r50 = r2, r3, 22, 6
  dep r51 = 0, r3, 22, 62
  shrp r52 = r2, r3, 22
  shl r53 = r2, 22
  shr.u r54 = r3, 22
  shr r55 = r3, 22
  extr.u r56 = r3, 41, 26
  extr r57 = r3, 41, 26
  dep.z r58 = r2, 41, 26
  dep.z r59 = -70, 41, 26
  dep r60 = r2, r3, 41, 3
  dep r61 = 0, r3, 41, 26
  shrp r62 = r2, r3, 54
  shl r63 = r2, 54
  shr.u r64 = r3, 54
  shr r65 = r3, 54
  extr.u r66 = r3, 4, 36
  extr r67 = r3, 4, 36
  dep.z r68 = r2, 4, 36
  dep.z r69 = 15, 4, 36
  dep r70 = r2, r3, 4, 11
  dep r71 = 0, r3, 4, 36
  shrp r72 = r2, r3, 47
  shl r73 = r2, 47
  shr.u r74 = r3, 47
  shr r75 = r3, 47
  extr.u r76 = r3, 36, 40
  extr r77 = r3, 36, 40
  dep.z r78 = r2, 36, 40
  dep.z r79 = 104, 36, 40
  dep r80 = r2, r3, 36, 7
  dep r81 = -1, r3, 36, 40
  shrp r82 = r2, r3, 19
  shl r83 = r2, 19
  shr.u r84 = r3, 19
  shr r85 = r3, 19
  extr.u r86 = r3, 13, 16
  extr r87 = r3, 13, 16
  dep.z r88 = r2, 13, 16
  dep.z r89 = 124, 13, 16
  dep r90 = r2, r3, 13, 8
  dep r91 = -1, r3, 13, 16
  shrp r92 = r2, r3, 18
  shl r93 = r2, 18
  shr.u r94 = r3, 18
  shr r95 = r3, 18
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.le p6, p7 = 99, r3
  cmp4.gtu p8, p9 = -120, r3
  cmp.gt p10, p11 = r2, r3
  tbit.z p12, p13 = r3, 33
  cmp.geu p14, p15 = r2, r3
  cmp.gt p16, p17 = 85, r3
  cmp.lt p18, p19 = -43, r3
  cmp.ltu p20, p21 = r2, r3
  tbit.nz p22, p23 = r3, 33
  cmp4.gt p24, p25 = 49, r3
  cmp.leu p26, p27 = r2, r3
  cmp4.gtu p28, p29 = r2, r3
  cmp4.geu p30, p31 = 123, r3
  cmp.gtu p32, p33 = 73, r3
  cmp.ge p34, p35 = r2, r3
  tbit.nz p36, p37 = r3, 24
  cmp4.ltu p38, p39 = r2, r3
  tbit.z p40, p41 = r3, 24
  cmp4.le p42, p43 = r2, r3
  cmp.eq p44, p45 = r2, r3
  cmp.gtu p46, p47 = r2, r3
  cmp4.lt p48, p49 = r2, r3
  cmp4.ge p50, p51 = 65, r3
  cmp4.eq p52, p53 = -13, r3
  tbit.nz p54, p55 = r3, 4
  cmp4.leu p56, p57 = r2, r3
  cmp.geu p58, p59 = -45, r3
  cmp4.ne p60, p61 = -122, r3
  cmp.ne p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
