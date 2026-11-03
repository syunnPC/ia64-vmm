/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x61b339ffa48174e4   add R = r2, r3
// EXPECT r9 = 0x61b339ffa48174e5   add R = r2, r3, 1
// EXPECT r10 = 0x61b339fea48174e6   sub R = r2, r3
// EXPECT r11 = 0x61b339fea48174e5   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffff7ffffff3   sub R = -14, r3
// EXPECT r13 = 0x7ffffff2   and R = -14, r3
// EXPECT r14 = 0xffffffffffffffff   or R = -14, r3
// EXPECT r15 = 0xffffffff8000000d   xor R = -14, r3
// EXPECT r16 = 0xffffffff80000000   andcm R = -14, r3
// EXPECT r17 = 0x248174e5   and R = r2, r3
// EXPECT r18 = 0x61b339ff7fffffff   or R = r2, r3
// EXPECT r19 = 0x61b339ff5b7e8b1a   xor R = r2, r3
// EXPECT r20 = 0x61b339ff00000000   andcm R = r2, r3
// EXPECT r21 = 0x7fffeba4   adds R = -5211, r3
// EXPECT r22 = 0x163f62   addl R = 1458018, r1
// EXPECT r23 = 0xc36673fec902e9c9   shladd R = r2, 1, r3
// EXPECT r24 = 0x20000000c902e9c9   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x86cce7fd1205d393   shladd R = r2, 2, r3
// EXPECT r26 = 0x200000001205d393   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xd99cff9a40ba727   shladd R = r2, 3, r3
// EXPECT r28 = 0x20000000a40ba727   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x1b339ff2c8174e4f   shladd R = r2, 4, r3
// EXPECT r30 = 0x20000000c8174e4f   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x20000000a48174e4   addp4 R = r2, r3
// EXPECT r32 = 0x200000007fffeba4   addp4 R = -5211, r3
// EXPECT r33 = 0x61b339ff248174e5   shl R = r2, r4
// EXPECT r34 = 0x7fffffff   shr.u R = r3, r4
// EXPECT r35 = 0x7fffffff   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 56, 15
// EXPECT r37 = 0x0   extr R = r3, 56, 15
// EXPECT r38 = 0xe500000000000000   dep.z R = r2, 56, 15
// EXPECT r39 = 0x5800000000000000   dep.z R = 88, 56, 15
// EXPECT r40 = 0x50000007fffffff   dep R = r2, r3, 56, 5
// EXPECT r41 = 0xff0000007fffffff   dep R = -1, r3, 56, 15
// EXPECT r42 = 0x174e5000000007ff   shrp R = r2, r3, 20
// EXPECT r43 = 0x9ff248174e500000   shl R = r2, 20
// EXPECT r44 = 0x7ff   shr.u R = r3, 20
// EXPECT r45 = 0x7ff   shr R = r3, 20
// EXPECT r46 = 0x3fffff   extr.u R = r3, 6, 22
// EXPECT r47 = 0xffffffffffffffff   extr R = r3, 6, 22
// EXPECT r48 = 0x5d3940   dep.z R = r2, 6, 22
// EXPECT r49 = 0xfffea80   dep.z R = -86, 6, 22
// EXPECT r50 = 0x7ffd397f   dep R = r2, r3, 6, 13
// EXPECT r51 = 0x7fffffff   dep R = -1, r3, 6, 22
// EXPECT r52 = 0x1b339ff248174e50   shrp R = r2, r3, 60
// EXPECT r53 = 0x5000000000000000   shl R = r2, 60
// EXPECT r54 = 0x0   shr.u R = r3, 60
// EXPECT r55 = 0x0   shr R = r3, 60
// EXPECT r56 = 0x0   extr.u R = r3, 61, 50
// EXPECT r57 = 0x0   extr R = r3, 61, 50
// EXPECT r58 = 0xa000000000000000   dep.z R = r2, 61, 50
// EXPECT r59 = 0xe000000000000000   dep.z R = -113, 61, 50
// EXPECT r60 = 0xa00000007fffffff   dep R = r2, r3, 61, 3
// EXPECT r61 = 0x7fffffff   dep R = 0, r3, 61, 50
// EXPECT r62 = 0x9240ba7280000000   shrp R = r2, r3, 33
// EXPECT r63 = 0x4902e9ca00000000   shl R = r2, 33
// EXPECT r64 = 0x0   shr.u R = r3, 33
// EXPECT r65 = 0x0   shr R = r3, 33
// EXPECT r66 = 0x0   extr.u R = r3, 45, 47
// EXPECT r67 = 0x0   extr R = r3, 45, 47
// EXPECT r68 = 0x2e9ca00000000000   dep.z R = r2, 45, 47
// EXPECT r69 = 0x8a00000000000   dep.z R = 69, 45, 47
// EXPECT r70 = 0x1ca0007fffffff   dep R = r2, r3, 45, 10
// EXPECT r71 = 0x7fffffff   dep R = 0, r3, 45, 47
// EXPECT r72 = 0x248174e500000000   shrp R = r2, r3, 32
// EXPECT r73 = 0x248174e500000000   shl R = r2, 32
// EXPECT r74 = 0x0   shr.u R = r3, 32
// EXPECT r75 = 0x0   shr R = r3, 32
// EXPECT r76 = 0x1   extr.u R = r3, 30, 43
// EXPECT r77 = 0x1   extr R = r3, 30, 43
// EXPECT r78 = 0xc9205d3940000000   dep.z R = r2, 30, 43
// EXPECT r79 = 0xe80000000   dep.z R = 58, 30, 43
// EXPECT r80 = 0x1397fffffff   dep R = r2, r3, 30, 12
// EXPECT r81 = 0x3fffffff   dep R = 0, r3, 30, 43
// EXPECT r82 = 0xa00000000fffffff   shrp R = r2, r3, 3
// EXPECT r83 = 0xd99cff9240ba728   shl R = r2, 3
// EXPECT r84 = 0xfffffff   shr.u R = r3, 3
// EXPECT r85 = 0xfffffff   shr R = r3, 3
// EXPECT r86 = 0x0   extr.u R = r3, 48, 56
// EXPECT r87 = 0x0   extr R = r3, 48, 56
// EXPECT r88 = 0x74e5000000000000   dep.z R = r2, 48, 56
// EXPECT r89 = 0xff90000000000000   dep.z R = -112, 48, 56
// EXPECT r90 = 0x100007fffffff   dep R = r2, r3, 48, 1
// EXPECT r91 = 0x7fffffff   dep R = 0, r3, 48, 56
// EXPECT r92 = 0xcce7fc9205d39400   shrp R = r2, r3, 54
// EXPECT r93 = 0x3940000000000000   shl R = r2, 54
// EXPECT r94 = 0x0   shr.u R = r3, 54
// EXPECT r95 = 0x0   shr R = r3, 54
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0x7fffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0x7fffffff   zxt4 R = r3
// EXPECT r102 = 0x1f   popcnt R = r3
// EXPECT p6 = 1   cmp4.le P1, P2 = 107, r3
// EXPECT p7 = 0
// EXPECT p8 = 0   cmp4.gtu P1, P2 = 45, r3
// EXPECT p9 = 1
// EXPECT p10 = 1   cmp4.ne P1, P2 = 127, r3
// EXPECT p11 = 0
// EXPECT p12 = 0   cmp.leu P1, P2 = -37, r3
// EXPECT p13 = 1
// EXPECT p14 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p17 = 0
// EXPECT p18 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p19 = 1
// EXPECT p20 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   cmp.ge P1, P2 = -110, r3
// EXPECT p23 = 1
// EXPECT p24 = 0   cmp4.eq P1, P2 = 13, r3
// EXPECT p25 = 1
// EXPECT p26 = 1   tbit.nz P1, P2 = r3, 30
// EXPECT p27 = 0
// EXPECT p28 = 0   cmp.gtu P1, P2 = 12, r3
// EXPECT p29 = 1
// EXPECT p30 = 0   tbit.nz P1, P2 = r3, 60
// EXPECT p31 = 1
// EXPECT p32 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p33 = 1
// EXPECT p34 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p35 = 0
// EXPECT p36 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.ltu P1, P2 = -76, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp4.ge P1, P2 = 16, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp4.geu P1, P2 = 105, r3
// EXPECT p43 = 1
// EXPECT p44 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p47 = 1
// EXPECT p48 = 1   tbit.nz P1, P2 = r3, 21
// EXPECT p49 = 0
// EXPECT p50 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p51 = 1
// EXPECT p52 = 1   tbit.z P1, P2 = r3, 60
// EXPECT p53 = 0
// EXPECT p54 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p55 = 1
// EXPECT p56 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp.ne P1, P2 = 70, r3
// EXPECT p59 = 0
// EXPECT p60 = 1   cmp4.leu P1, P2 = 60, r3
// EXPECT p61 = 0
// EXPECT p62 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x61b339ff248174e5
  movl r3 = 0x7fffffff
  movl r4 = 0x0
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -14, r3
  and r13 = -14, r3
  or r14 = -14, r3
  xor r15 = -14, r3
  andcm r16 = -14, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -5211, r3
  addl r22 = 1458018, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -5211, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 56, 15
  extr r37 = r3, 56, 15
  dep.z r38 = r2, 56, 15
  dep.z r39 = 88, 56, 15
  dep r40 = r2, r3, 56, 5
  dep r41 = -1, r3, 56, 15
  shrp r42 = r2, r3, 20
  shl r43 = r2, 20
  shr.u r44 = r3, 20
  shr r45 = r3, 20
  extr.u r46 = r3, 6, 22
  extr r47 = r3, 6, 22
  dep.z r48 = r2, 6, 22
  dep.z r49 = -86, 6, 22
  dep r50 = r2, r3, 6, 13
  dep r51 = -1, r3, 6, 22
  shrp r52 = r2, r3, 60
  shl r53 = r2, 60
  shr.u r54 = r3, 60
  shr r55 = r3, 60
  extr.u r56 = r3, 61, 50
  extr r57 = r3, 61, 50
  dep.z r58 = r2, 61, 50
  dep.z r59 = -113, 61, 50
  dep r60 = r2, r3, 61, 3
  dep r61 = 0, r3, 61, 50
  shrp r62 = r2, r3, 33
  shl r63 = r2, 33
  shr.u r64 = r3, 33
  shr r65 = r3, 33
  extr.u r66 = r3, 45, 47
  extr r67 = r3, 45, 47
  dep.z r68 = r2, 45, 47
  dep.z r69 = 69, 45, 47
  dep r70 = r2, r3, 45, 10
  dep r71 = 0, r3, 45, 47
  shrp r72 = r2, r3, 32
  shl r73 = r2, 32
  shr.u r74 = r3, 32
  shr r75 = r3, 32
  extr.u r76 = r3, 30, 43
  extr r77 = r3, 30, 43
  dep.z r78 = r2, 30, 43
  dep.z r79 = 58, 30, 43
  dep r80 = r2, r3, 30, 12
  dep r81 = 0, r3, 30, 43
  shrp r82 = r2, r3, 3
  shl r83 = r2, 3
  shr.u r84 = r3, 3
  shr r85 = r3, 3
  extr.u r86 = r3, 48, 56
  extr r87 = r3, 48, 56
  dep.z r88 = r2, 48, 56
  dep.z r89 = -112, 48, 56
  dep r90 = r2, r3, 48, 1
  dep r91 = 0, r3, 48, 56
  shrp r92 = r2, r3, 54
  shl r93 = r2, 54
  shr.u r94 = r3, 54
  shr r95 = r3, 54
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.le p6, p7 = 107, r3
  cmp4.gtu p8, p9 = 45, r3
  cmp4.ne p10, p11 = 127, r3
  cmp.leu p12, p13 = -37, r3
  cmp.gt p14, p15 = r2, r3
  cmp.gtu p16, p17 = r2, r3
  cmp4.geu p18, p19 = r2, r3
  cmp.eq p20, p21 = r2, r3
  cmp.ge p22, p23 = -110, r3
  cmp4.eq p24, p25 = 13, r3
  tbit.nz p26, p27 = r3, 30
  cmp.gtu p28, p29 = 12, r3
  tbit.nz p30, p31 = r3, 60
  cmp4.gt p32, p33 = r2, r3
  cmp.ne p34, p35 = r2, r3
  cmp4.ge p36, p37 = r2, r3
  cmp.ltu p38, p39 = -76, r3
  cmp4.ge p40, p41 = 16, r3
  cmp4.geu p42, p43 = 105, r3
  cmp.ge p44, p45 = r2, r3
  cmp.lt p46, p47 = r2, r3
  tbit.nz p48, p49 = r3, 21
  cmp.leu p50, p51 = r2, r3
  tbit.z p52, p53 = r3, 60
  cmp.le p54, p55 = r2, r3
  cmp4.eq p56, p57 = r2, r3
  cmp.ne p58, p59 = 70, r3
  cmp4.leu p60, p61 = 60, r3
  cmp4.le p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
