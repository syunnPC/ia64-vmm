/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x2fa91425cb008852   add R = r2, r3
// EXPECT r9 = 0x2fa91425cb008853   add R = r2, r3, 1
// EXPECT r10 = 0x2fa91425cb008854   sub R = r2, r3
// EXPECT r11 = 0x2fa91425cb008853   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffffffffffa4   sub R = -93, r3
// EXPECT r13 = 0xffffffffffffffa3   and R = -93, r3
// EXPECT r14 = 0xffffffffffffffff   or R = -93, r3
// EXPECT r15 = 0x5c   xor R = -93, r3
// EXPECT r16 = 0x0   andcm R = -93, r3
// EXPECT r17 = 0x2fa91425cb008853   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0xd056ebda34ff77ac   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0xffffffffffffedcf   adds R = -4656, r3
// EXPECT r22 = 0x13404d   addl R = 1261645, r1
// EXPECT r23 = 0x5f52284b960110a5   shladd R = r2, 1, r3
// EXPECT r24 = 0x60000000960110a5   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0xbea450972c02214b   shladd R = r2, 2, r3
// EXPECT r26 = 0x600000002c02214b   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x7d48a12e58044297   shladd R = r2, 3, r3
// EXPECT r28 = 0x6000000058044297   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xfa91425cb008852f   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000b008852f   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x60000000cb008852   addp4 R = r2, r3
// EXPECT r32 = 0x60000000ffffedcf   addp4 R = -4656, r3
// EXPECT r33 = 0x2fa91425cb008853   shl R = r2, r4
// EXPECT r34 = 0xffffffffffffffff   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0x3fffffffff   extr.u R = r3, 13, 38
// EXPECT r37 = 0xffffffffffffffff   extr R = r3, 13, 38
// EXPECT r38 = 0x4b960110a6000   dep.z R = r2, 13, 38
// EXPECT r39 = 0x8a000   dep.z R = 69, 13, 38
// EXPECT r40 = 0xffffffffffff7fff   dep R = r2, r3, 13, 3
// EXPECT r41 = 0xfff8000000001fff   dep R = 0, r3, 13, 38
// EXPECT r42 = 0xffffffffffffffff   shrp R = r2, r3, 0
// EXPECT r43 = 0x2fa91425cb008853   shl R = r2, 0
// EXPECT r44 = 0xffffffffffffffff   shr.u R = r3, 0
// EXPECT r45 = 0xffffffffffffffff   shr R = r3, 0
// EXPECT r46 = 0x7ffffff   extr.u R = r3, 27, 27
// EXPECT r47 = 0xffffffffffffffff   extr R = r3, 27, 27
// EXPECT r48 = 0x18044298000000   dep.z R = r2, 27, 27
// EXPECT r49 = 0x3ffffcd0000000   dep.z R = -102, 27, 27
// EXPECT r50 = 0xfffffc429fffffff   dep R = r2, r3, 27, 16
// EXPECT r51 = 0xffffffffffffffff   dep R = -1, r3, 27, 27
// EXPECT r52 = 0x450972c02214ffff   shrp R = r2, r3, 50
// EXPECT r53 = 0x214c000000000000   shl R = r2, 50
// EXPECT r54 = 0x3fff   shr.u R = r3, 50
// EXPECT r55 = 0xffffffffffffffff   shr R = r3, 50
// EXPECT r56 = 0x3ff   extr.u R = r3, 53, 10
// EXPECT r57 = 0xffffffffffffffff   extr R = r3, 53, 10
// EXPECT r58 = 0xa60000000000000   dep.z R = r2, 53, 10
// EXPECT r59 = 0x7ca0000000000000   dep.z R = -27, 53, 10
// EXPECT r60 = 0xca7fffffffffffff   dep R = r2, r3, 53, 9
// EXPECT r61 = 0xffffffffffffffff   dep R = -1, r3, 53, 10
// EXPECT r62 = 0xa7fffffffffffff   shrp R = r2, r3, 11
// EXPECT r63 = 0x48a12e5804429800   shl R = r2, 11
// EXPECT r64 = 0x1fffffffffffff   shr.u R = r3, 11
// EXPECT r65 = 0xffffffffffffffff   shr R = r3, 11
// EXPECT r66 = 0x1ffffff   extr.u R = r3, 39, 43
// EXPECT r67 = 0xffffffffffffffff   extr R = r3, 39, 43
// EXPECT r68 = 0x8044298000000000   dep.z R = r2, 39, 43
// EXPECT r69 = 0xffffc38000000000   dep.z R = -121, 39, 43
// EXPECT r70 = 0xffe429ffffffffff   dep R = r2, r3, 39, 14
// EXPECT r71 = 0x7fffffffff   dep R = 0, r3, 39, 43
// EXPECT r72 = 0x4429ffffffffffff   shrp R = r2, r3, 17
// EXPECT r73 = 0x284b960110a60000   shl R = r2, 17
// EXPECT r74 = 0x7fffffffffff   shr.u R = r3, 17
// EXPECT r75 = 0xffffffffffffffff   shr R = r3, 17
// EXPECT r76 = 0x1fff   extr.u R = r3, 31, 13
// EXPECT r77 = 0xffffffffffffffff   extr R = r3, 31, 13
// EXPECT r78 = 0x42980000000   dep.z R = r2, 31, 13
// EXPECT r79 = 0xfc280000000   dep.z R = -123, 31, 13
// EXPECT r80 = 0xffffffffffffffff   dep R = r2, r3, 31, 2
// EXPECT r81 = 0xffffffffffffffff   dep R = -1, r3, 31, 13
// EXPECT r82 = 0xbea450972c02214f   shrp R = r2, r3, 62
// EXPECT r83 = 0xc000000000000000   shl R = r2, 62
// EXPECT r84 = 0x3   shr.u R = r3, 62
// EXPECT r85 = 0xffffffffffffffff   shr R = r3, 62
// EXPECT r86 = 0x1ffffff   extr.u R = r3, 22, 25
// EXPECT r87 = 0xffffffffffffffff   extr R = r3, 22, 25
// EXPECT r88 = 0x402214c00000   dep.z R = r2, 22, 25
// EXPECT r89 = 0x19400000   dep.z R = 101, 22, 25
// EXPECT r90 = 0xfffffffff4ffffff   dep R = r2, r3, 22, 7
// EXPECT r91 = 0xffff8000003fffff   dep R = 0, r3, 22, 25
// EXPECT r92 = 0x48a12e5804429fff   shrp R = r2, r3, 53
// EXPECT r93 = 0xa60000000000000   shl R = r2, 53
// EXPECT r94 = 0x7ff   shr.u R = r3, 53
// EXPECT r95 = 0xffffffffffffffff   shr R = r3, 53
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0xffffffffffffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0xffffffff   zxt4 R = r3
// EXPECT r102 = 0x40   popcnt R = r3
// EXPECT p6 = 0   cmp.lt P1, P2 = 74, r3
// EXPECT p7 = 1
// EXPECT p8 = 1   cmp.gt P1, P2 = 10, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp4.ge P1, P2 = -124, r3
// EXPECT p11 = 1
// EXPECT p12 = 1   tbit.nz P1, P2 = r3, 37
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp.gtu P1, P2 = 27, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p17 = 1
// EXPECT p18 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp4.ne P1, P2 = 73, r3
// EXPECT p21 = 0
// EXPECT p22 = 1   tbit.nz P1, P2 = r3, 9
// EXPECT p23 = 0
// EXPECT p24 = 1   cmp4.le P1, P2 = -54, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   tbit.z P1, P2 = r3, 9
// EXPECT p31 = 1
// EXPECT p32 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p33 = 0
// EXPECT p34 = 1   cmp.leu P1, P2 = -128, r3
// EXPECT p35 = 0
// EXPECT p36 = 1   cmp.le P1, P2 = -20, r3
// EXPECT p37 = 0
// EXPECT p38 = 0   cmp.geu P1, P2 = -21, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   tbit.z P1, P2 = r3, 37
// EXPECT p41 = 1
// EXPECT p42 = 1   cmp.ltu P1, P2 = 87, r3
// EXPECT p43 = 0
// EXPECT p44 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 1   cmp4.leu P1, P2 = -19, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p49 = 1
// EXPECT p50 = 0   cmp4.geu P1, P2 = 40, r3
// EXPECT p51 = 1
// EXPECT p52 = 1   cmp.ltu P1, P2 = r2, r3
// EXPECT p53 = 0
// EXPECT p54 = 0   tbit.z P1, P2 = r3, 9
// EXPECT p55 = 1
// EXPECT p56 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 0   cmp.ge P1, P2 = -118, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x2fa91425cb008853
  movl r3 = 0xffffffffffffffff
  movl r4 = 0x0
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -93, r3
  and r13 = -93, r3
  or r14 = -93, r3
  xor r15 = -93, r3
  andcm r16 = -93, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -4656, r3
  addl r22 = 1261645, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -4656, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 13, 38
  extr r37 = r3, 13, 38
  dep.z r38 = r2, 13, 38
  dep.z r39 = 69, 13, 38
  dep r40 = r2, r3, 13, 3
  dep r41 = 0, r3, 13, 38
  shrp r42 = r2, r3, 0
  shl r43 = r2, 0
  shr.u r44 = r3, 0
  shr r45 = r3, 0
  extr.u r46 = r3, 27, 27
  extr r47 = r3, 27, 27
  dep.z r48 = r2, 27, 27
  dep.z r49 = -102, 27, 27
  dep r50 = r2, r3, 27, 16
  dep r51 = -1, r3, 27, 27
  shrp r52 = r2, r3, 50
  shl r53 = r2, 50
  shr.u r54 = r3, 50
  shr r55 = r3, 50
  extr.u r56 = r3, 53, 10
  extr r57 = r3, 53, 10
  dep.z r58 = r2, 53, 10
  dep.z r59 = -27, 53, 10
  dep r60 = r2, r3, 53, 9
  dep r61 = -1, r3, 53, 10
  shrp r62 = r2, r3, 11
  shl r63 = r2, 11
  shr.u r64 = r3, 11
  shr r65 = r3, 11
  extr.u r66 = r3, 39, 43
  extr r67 = r3, 39, 43
  dep.z r68 = r2, 39, 43
  dep.z r69 = -121, 39, 43
  dep r70 = r2, r3, 39, 14
  dep r71 = 0, r3, 39, 43
  shrp r72 = r2, r3, 17
  shl r73 = r2, 17
  shr.u r74 = r3, 17
  shr r75 = r3, 17
  extr.u r76 = r3, 31, 13
  extr r77 = r3, 31, 13
  dep.z r78 = r2, 31, 13
  dep.z r79 = -123, 31, 13
  dep r80 = r2, r3, 31, 2
  dep r81 = -1, r3, 31, 13
  shrp r82 = r2, r3, 62
  shl r83 = r2, 62
  shr.u r84 = r3, 62
  shr r85 = r3, 62
  extr.u r86 = r3, 22, 25
  extr r87 = r3, 22, 25
  dep.z r88 = r2, 22, 25
  dep.z r89 = 101, 22, 25
  dep r90 = r2, r3, 22, 7
  dep r91 = 0, r3, 22, 25
  shrp r92 = r2, r3, 53
  shl r93 = r2, 53
  shr.u r94 = r3, 53
  shr r95 = r3, 53
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.lt p6, p7 = 74, r3
  cmp.gt p8, p9 = 10, r3
  cmp4.ge p10, p11 = -124, r3
  tbit.nz p12, p13 = r3, 37
  cmp.gtu p14, p15 = 27, r3
  cmp.eq p16, p17 = r2, r3
  cmp4.geu p18, p19 = r2, r3
  cmp4.ne p20, p21 = 73, r3
  tbit.nz p22, p23 = r3, 9
  cmp4.le p24, p25 = -54, r3
  cmp.le p26, p27 = r2, r3
  cmp4.leu p28, p29 = r2, r3
  tbit.z p30, p31 = r3, 9
  cmp4.lt p32, p33 = r2, r3
  cmp.leu p34, p35 = -128, r3
  cmp.le p36, p37 = -20, r3
  cmp.geu p38, p39 = -21, r3
  tbit.z p40, p41 = r3, 37
  cmp.ltu p42, p43 = 87, r3
  cmp4.ltu p44, p45 = r2, r3
  cmp4.leu p46, p47 = -19, r3
  cmp4.ge p48, p49 = r2, r3
  cmp4.geu p50, p51 = 40, r3
  cmp.ltu p52, p53 = r2, r3
  tbit.z p54, p55 = r3, 9
  cmp4.eq p56, p57 = r2, r3
  cmp.ge p58, p59 = -118, r3
  cmp4.gtu p60, p61 = r2, r3
  cmp.gt p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
