/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x3d833a9a269d131   add R = r2, r3
// EXPECT r9 = 0x3d833a9a269d132   add R = r2, r3, 1
// EXPECT r10 = 0x3d833a9a269d133   sub R = r2, r3
// EXPECT r11 = 0x3d833a9a269d132   sub R = r2, r3, 1
// EXPECT r12 = 0x8000000000000039   sub R = 56, r3
// EXPECT r13 = 0x38   and R = 56, r3
// EXPECT r14 = 0x7fffffffffffffff   or R = 56, r3
// EXPECT r15 = 0x7fffffffffffffc7   xor R = 56, r3
// EXPECT r16 = 0x0   andcm R = 56, r3
// EXPECT r17 = 0x3d833a9a269d132   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0xfc27cc565d962ecd   xor R = r2, r3
// EXPECT r20 = 0x8000000000000000   andcm R = r2, r3
// EXPECT r21 = 0x7fffffffffffe8c7   adds R = -5944, r3
// EXPECT r22 = 0xfffffffffffcc835   addl R = -210891, r1
// EXPECT r23 = 0x87b0675344d3a263   shladd R = r2, 1, r3
// EXPECT r24 = 0x6000000044d3a263   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x8f60cea689a744c7   shladd R = r2, 2, r3
// EXPECT r26 = 0x6000000089a744c7   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x9ec19d4d134e898f   shladd R = r2, 3, r3
// EXPECT r28 = 0x60000000134e898f   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xbd833a9a269d131f   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000269d131f   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x60000000a269d131   addp4 R = r2, r3
// EXPECT r32 = 0x60000000ffffe8c7   addp4 R = -5944, r3
// EXPECT r33 = 0x83d833a9a269d132   shl R = r2, r4
// EXPECT r34 = 0x7fffffffffffffff   shr.u R = r3, r4
// EXPECT r35 = 0x7fffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0x3ffffffffff   extr.u R = r3, 3, 42
// EXPECT r37 = 0xffffffffffffffff   extr R = r3, 3, 42
// EXPECT r38 = 0x1d4d134e8990   dep.z R = r2, 3, 42
// EXPECT r39 = 0x1a0   dep.z R = 52, 3, 42
// EXPECT r40 = 0x7fffffffffffff97   dep R = r2, r3, 3, 6
// EXPECT r41 = 0x7fffe00000000007   dep R = 0, r3, 3, 42
// EXPECT r42 = 0x3fffffffffffffff   shrp R = r2, r3, 1
// EXPECT r43 = 0x7b0675344d3a264   shl R = r2, 1
// EXPECT r44 = 0x3fffffffffffffff   shr.u R = r3, 1
// EXPECT r45 = 0x3fffffffffffffff   shr R = r3, 1
// EXPECT r46 = 0xfffffffff   extr.u R = r3, 27, 40
// EXPECT r47 = 0xfffffffff   extr R = r3, 27, 40
// EXPECT r48 = 0x4d134e8990000000   dep.z R = r2, 27, 40
// EXPECT r49 = 0xffffffffa0000000   dep.z R = -12, 27, 40
// EXPECT r50 = 0x7fffffe997ffffff   dep R = r2, r3, 27, 10
// EXPECT r51 = 0x7ffffff   dep R = 0, r3, 27, 40
// EXPECT r52 = 0x9a269d1327ffffff   shrp R = r2, r3, 36
// EXPECT r53 = 0x269d132000000000   shl R = r2, 36
// EXPECT r54 = 0x7ffffff   shr.u R = r3, 36
// EXPECT r55 = 0x7ffffff   shr R = r3, 36
// EXPECT r56 = 0x7ffffff   extr.u R = r3, 34, 27
// EXPECT r57 = 0xffffffffffffffff   extr R = r3, 34, 27
// EXPECT r58 = 0x9a744c800000000   dep.z R = r2, 34, 27
// EXPECT r59 = 0x1d800000000   dep.z R = 118, 34, 27
// EXPECT r60 = 0x7fffffcbffffffff   dep R = r2, r3, 34, 5
// EXPECT r61 = 0x60000003ffffffff   dep R = 0, r3, 34, 27
// EXPECT r62 = 0x689a744c9fffffff   shrp R = r2, r3, 34
// EXPECT r63 = 0x89a744c800000000   shl R = r2, 34
// EXPECT r64 = 0x1fffffff   shr.u R = r3, 34
// EXPECT r65 = 0x1fffffff   shr R = r3, 34
// EXPECT r66 = 0x1ffffffffffffff   extr.u R = r3, 6, 57
// EXPECT r67 = 0xffffffffffffffff   extr R = r3, 6, 57
// EXPECT r68 = 0x760cea689a744c80   dep.z R = r2, 6, 57
// EXPECT r69 = 0x1940   dep.z R = 101, 6, 57
// EXPECT r70 = 0x7ffffffffffffcbf   dep R = r2, r3, 6, 4
// EXPECT r71 = 0x3f   dep R = 0, r3, 6, 57
// EXPECT r72 = 0x689a744c9fffffff   shrp R = r2, r3, 34
// EXPECT r73 = 0x89a744c800000000   shl R = r2, 34
// EXPECT r74 = 0x1fffffff   shr.u R = r3, 34
// EXPECT r75 = 0x1fffffff   shr R = r3, 34
// EXPECT r76 = 0xfff   extr.u R = r3, 30, 12
// EXPECT r77 = 0xffffffffffffffff   extr R = r3, 30, 12
// EXPECT r78 = 0x4c80000000   dep.z R = r2, 30, 12
// EXPECT r79 = 0x1880000000   dep.z R = 98, 30, 12
// EXPECT r80 = 0x7ffffffcbfffffff   dep R = r2, r3, 30, 5
// EXPECT r81 = 0x7fffffffffffffff   dep R = -1, r3, 30, 12
// EXPECT r82 = 0x3a9a269d1327ffff   shrp R = r2, r3, 44
// EXPECT r83 = 0x9d13200000000000   shl R = r2, 44
// EXPECT r84 = 0x7ffff   shr.u R = r3, 44
// EXPECT r85 = 0x7ffff   shr R = r3, 44
// EXPECT r86 = 0x1fff   extr.u R = r3, 3, 13
// EXPECT r87 = 0xffffffffffffffff   extr R = r3, 3, 13
// EXPECT r88 = 0x8990   dep.z R = r2, 3, 13
// EXPECT r89 = 0xfe60   dep.z R = -52, 3, 13
// EXPECT r90 = 0x7fffffffffffffd7   dep R = r2, r3, 3, 3
// EXPECT r91 = 0x7fffffffffff0007   dep R = 0, r3, 3, 13
// EXPECT r92 = 0x44d3a264ffffffff   shrp R = r2, r3, 31
// EXPECT r93 = 0xd134e89900000000   shl R = r2, 31
// EXPECT r94 = 0xffffffff   shr.u R = r3, 31
// EXPECT r95 = 0xffffffff   shr R = r3, 31
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0xffffffffffffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0xffffffff   zxt4 R = r3
// EXPECT r102 = 0x3f   popcnt R = r3
// EXPECT p6 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p7 = 0
// EXPECT p8 = 0   cmp.gt P1, P2 = -20, r3
// EXPECT p9 = 1
// EXPECT p10 = 1   cmp.gtu P1, P2 = -71, r3
// EXPECT p11 = 0
// EXPECT p12 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 1   cmp4.leu P1, P2 = -14, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp4.gtu P1, P2 = 77, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   cmp.ge P1, P2 = -44, r3
// EXPECT p23 = 1
// EXPECT p24 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   tbit.z P1, P2 = r3, 39
// EXPECT p27 = 1
// EXPECT p28 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p29 = 1
// EXPECT p30 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p31 = 1
// EXPECT p32 = 0   cmp.geu P1, P2 = 91, r3
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   cmp4.geu P1, P2 = 33, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.eq P1, P2 = 19, r3
// EXPECT p39 = 1
// EXPECT p40 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p41 = 0
// EXPECT p42 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p43 = 0
// EXPECT p44 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   cmp.ltu P1, P2 = -36, r3
// EXPECT p47 = 1
// EXPECT p48 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p49 = 1
// EXPECT p50 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p51 = 0
// EXPECT p52 = 0   tbit.z P1, P2 = r3, 24
// EXPECT p53 = 1
// EXPECT p54 = 0   cmp.leu P1, P2 = -59, r3
// EXPECT p55 = 1
// EXPECT p56 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p59 = 1
// EXPECT p60 = 1   cmp4.ne P1, P2 = 54, r3
// EXPECT p61 = 0
// EXPECT p62 = 1   cmp.ne P1, P2 = 114, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x83d833a9a269d132
  movl r3 = 0x7fffffffffffffff
  movl r4 = 0x0
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 56, r3
  and r13 = 56, r3
  or r14 = 56, r3
  xor r15 = 56, r3
  andcm r16 = 56, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -5944, r3
  addl r22 = -210891, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -5944, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 3, 42
  extr r37 = r3, 3, 42
  dep.z r38 = r2, 3, 42
  dep.z r39 = 52, 3, 42
  dep r40 = r2, r3, 3, 6
  dep r41 = 0, r3, 3, 42
  shrp r42 = r2, r3, 1
  shl r43 = r2, 1
  shr.u r44 = r3, 1
  shr r45 = r3, 1
  extr.u r46 = r3, 27, 40
  extr r47 = r3, 27, 40
  dep.z r48 = r2, 27, 40
  dep.z r49 = -12, 27, 40
  dep r50 = r2, r3, 27, 10
  dep r51 = 0, r3, 27, 40
  shrp r52 = r2, r3, 36
  shl r53 = r2, 36
  shr.u r54 = r3, 36
  shr r55 = r3, 36
  extr.u r56 = r3, 34, 27
  extr r57 = r3, 34, 27
  dep.z r58 = r2, 34, 27
  dep.z r59 = 118, 34, 27
  dep r60 = r2, r3, 34, 5
  dep r61 = 0, r3, 34, 27
  shrp r62 = r2, r3, 34
  shl r63 = r2, 34
  shr.u r64 = r3, 34
  shr r65 = r3, 34
  extr.u r66 = r3, 6, 57
  extr r67 = r3, 6, 57
  dep.z r68 = r2, 6, 57
  dep.z r69 = 101, 6, 57
  dep r70 = r2, r3, 6, 4
  dep r71 = 0, r3, 6, 57
  shrp r72 = r2, r3, 34
  shl r73 = r2, 34
  shr.u r74 = r3, 34
  shr r75 = r3, 34
  extr.u r76 = r3, 30, 12
  extr r77 = r3, 30, 12
  dep.z r78 = r2, 30, 12
  dep.z r79 = 98, 30, 12
  dep r80 = r2, r3, 30, 5
  dep r81 = -1, r3, 30, 12
  shrp r82 = r2, r3, 44
  shl r83 = r2, 44
  shr.u r84 = r3, 44
  shr r85 = r3, 44
  extr.u r86 = r3, 3, 13
  extr r87 = r3, 3, 13
  dep.z r88 = r2, 3, 13
  dep.z r89 = -52, 3, 13
  dep r90 = r2, r3, 3, 3
  dep r91 = 0, r3, 3, 13
  shrp r92 = r2, r3, 31
  shl r93 = r2, 31
  shr.u r94 = r3, 31
  shr r95 = r3, 31
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.ne p6, p7 = r2, r3
  cmp.gt p8, p9 = -20, r3
  cmp.gtu p10, p11 = -71, r3
  cmp4.le p12, p13 = r2, r3
  cmp4.ge p14, p15 = r2, r3
  cmp4.leu p16, p17 = -14, r3
  cmp4.ne p18, p19 = r2, r3
  cmp4.gtu p20, p21 = 77, r3
  cmp.ge p22, p23 = -44, r3
  cmp4.geu p24, p25 = r2, r3
  tbit.z p26, p27 = r3, 39
  cmp4.gtu p28, p29 = r2, r3
  cmp.gt p30, p31 = r2, r3
  cmp.geu p32, p33 = 91, r3
  cmp.eq p34, p35 = r2, r3
  cmp4.geu p36, p37 = 33, r3
  cmp.eq p38, p39 = 19, r3
  cmp.le p40, p41 = r2, r3
  cmp.geu p42, p43 = r2, r3
  cmp.gtu p44, p45 = r2, r3
  cmp.ltu p46, p47 = -36, r3
  cmp.leu p48, p49 = r2, r3
  cmp4.lt p50, p51 = r2, r3
  tbit.z p52, p53 = r3, 24
  cmp.leu p54, p55 = -59, r3
  cmp.ltu p56, p57 = r2, r3
  cmp4.eq p58, p59 = r2, r3
  cmp4.ne p60, p61 = 54, r3
  cmp.ne p62, p63 = 114, r3
  ;;
  break.i 0x1ffffe
