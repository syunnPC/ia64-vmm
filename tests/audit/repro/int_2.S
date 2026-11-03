/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x68624fabfffffffe   add R = r2, r3
// EXPECT r9 = 0x68624fabffffffff   add R = r2, r3, 1
// EXPECT r10 = 0x979db05400000000   sub R = r2, r3
// EXPECT r11 = 0x979db053ffffffff   sub R = r2, r3, 1
// EXPECT r12 = 0x179db053ffffff8d   sub R = -116, r3
// EXPECT r13 = 0xe8624fabffffff8c   and R = -116, r3
// EXPECT r14 = 0xffffffffffffffff   or R = -116, r3
// EXPECT r15 = 0x179db05400000073   xor R = -116, r3
// EXPECT r16 = 0x179db05400000000   andcm R = -116, r3
// EXPECT r17 = 0x68624fabffffffff   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0x979db05400000000   xor R = r2, r3
// EXPECT r20 = 0x179db05400000000   andcm R = r2, r3
// EXPECT r21 = 0xe8624fabfffff69d   adds R = -2402, r3
// EXPECT r22 = 0x99e48   addl R = 630344, r1
// EXPECT r23 = 0xe8624fabfffffffd   shladd R = r2, 1, r3
// EXPECT r24 = 0x60000000fffffffd   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0xe8624fabfffffffb   shladd R = r2, 2, r3
// EXPECT r26 = 0x60000000fffffffb   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xe8624fabfffffff7   shladd R = r2, 3, r3
// EXPECT r28 = 0x60000000fffffff7   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xe8624fabffffffef   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000ffffffef   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x60000000fffffffe   addp4 R = r2, r3
// EXPECT r32 = 0x60000000fffff69d   addp4 R = -2402, r3
// EXPECT r33 = 0xffffffff00000000   shl R = r2, r4
// EXPECT r34 = 0xe8624fab   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffe8624fab   shr R = r3, r4
// EXPECT r36 = 0x2afff   extr.u R = r3, 22, 18
// EXPECT r37 = 0xfffffffffffeafff   extr R = r3, 22, 18
// EXPECT r38 = 0xffffc00000   dep.z R = r2, 22, 18
// EXPECT r39 = 0xe000000   dep.z R = 56, 22, 18
// EXPECT r40 = 0xe8624fabffffffff   dep R = r2, r3, 22, 6
// EXPECT r41 = 0xe8624fffffffffff   dep R = -1, r3, 22, 18
// EXPECT r42 = 0xffffffffffffff43   shrp R = r2, r3, 53
// EXPECT r43 = 0xffe0000000000000   shl R = r2, 53
// EXPECT r44 = 0x743   shr.u R = r3, 53
// EXPECT r45 = 0xffffffffffffff43   shr R = r3, 53
// EXPECT r46 = 0x3a189   extr.u R = r3, 46, 46
// EXPECT r47 = 0xffffffffffffa189   extr R = r3, 46, 46
// EXPECT r48 = 0xffffc00000000000   dep.z R = r2, 46, 46
// EXPECT r49 = 0xe400000000000   dep.z R = 57, 46, 46
// EXPECT r50 = 0xffffcfabffffffff   dep R = r2, r3, 46, 15
// EXPECT r51 = 0xfabffffffff   dep R = 0, r3, 46, 46
// EXPECT r52 = 0xfffffffffffffd0c   shrp R = r2, r3, 51
// EXPECT r53 = 0xfff8000000000000   shl R = r2, 51
// EXPECT r54 = 0x1d0c   shr.u R = r3, 51
// EXPECT r55 = 0xfffffffffffffd0c   shr R = r3, 51
// EXPECT r56 = 0x1d   extr.u R = r3, 59, 32
// EXPECT r57 = 0xfffffffffffffffd   extr R = r3, 59, 32
// EXPECT r58 = 0xf800000000000000   dep.z R = r2, 59, 32
// EXPECT r59 = 0xd000000000000000   dep.z R = 122, 59, 32
// EXPECT r60 = 0xf8624fabffffffff   dep R = r2, r3, 59, 9
// EXPECT r61 = 0xf8624fabffffffff   dep R = -1, r3, 59, 32
// EXPECT r62 = 0xffffffffffff4312   shrp R = r2, r3, 45
// EXPECT r63 = 0xffffe00000000000   shl R = r2, 45
// EXPECT r64 = 0x74312   shr.u R = r3, 45
// EXPECT r65 = 0xffffffffffff4312   shr R = r3, 45
// EXPECT r66 = 0x3a   extr.u R = r3, 58, 60
// EXPECT r67 = 0xfffffffffffffffa   extr R = r3, 58, 60
// EXPECT r68 = 0xfc00000000000000   dep.z R = r2, 58, 60
// EXPECT r69 = 0xcc00000000000000   dep.z R = 51, 58, 60
// EXPECT r70 = 0xfc624fabffffffff   dep R = r2, r3, 58, 15
// EXPECT r71 = 0xfc624fabffffffff   dep R = -1, r3, 58, 60
// EXPECT r72 = 0xfffffffe8624fabf   shrp R = r2, r3, 28
// EXPECT r73 = 0xfffffffff0000000   shl R = r2, 28
// EXPECT r74 = 0xe8624fabf   shr.u R = r3, 28
// EXPECT r75 = 0xfffffffe8624fabf   shr R = r3, 28
// EXPECT r76 = 0x343127   extr.u R = r3, 41, 22
// EXPECT r77 = 0xfffffffffff43127   extr R = r3, 41, 22
// EXPECT r78 = 0x7ffffe0000000000   dep.z R = r2, 41, 22
// EXPECT r79 = 0x120000000000   dep.z R = 9, 41, 22
// EXPECT r80 = 0xe9ffffabffffffff   dep R = r2, r3, 41, 16
// EXPECT r81 = 0xffffffabffffffff   dep R = -1, r3, 41, 22
// EXPECT r82 = 0xffffffffffa1893e   shrp R = r2, r3, 38
// EXPECT r83 = 0xffffffc000000000   shl R = r2, 38
// EXPECT r84 = 0x3a1893e   shr.u R = r3, 38
// EXPECT r85 = 0xffffffffffa1893e   shr R = r3, 38
// EXPECT r86 = 0xe86   extr.u R = r3, 52, 40
// EXPECT r87 = 0xfffffffffffffe86   extr R = r3, 52, 40
// EXPECT r88 = 0xfff0000000000000   dep.z R = r2, 52, 40
// EXPECT r89 = 0xfea0000000000000   dep.z R = -22, 52, 40
// EXPECT r90 = 0xfff24fabffffffff   dep R = r2, r3, 52, 16
// EXPECT r91 = 0xfff24fabffffffff   dep R = -1, r3, 52, 40
// EXPECT r92 = 0xfff43127d5ffffff   shrp R = r2, r3, 9
// EXPECT r93 = 0xfffffffffffffe00   shl R = r2, 9
// EXPECT r94 = 0x743127d5ffffff   shr.u R = r3, 9
// EXPECT r95 = 0xfff43127d5ffffff   shr R = r3, 9
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0xffffffffffffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0xffffffff   zxt4 R = r3
// EXPECT r102 = 0x31   popcnt R = r3
// EXPECT p6 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p7 = 1
// EXPECT p8 = 0   cmp.eq P1, P2 = 46, r3
// EXPECT p9 = 1
// EXPECT p10 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p11 = 0
// EXPECT p12 = 1   tbit.nz P1, P2 = r3, 14
// EXPECT p13 = 0
// EXPECT p14 = 1   cmp4.ne P1, P2 = -3, r3
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p17 = 0
// EXPECT p18 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p21 = 0
// EXPECT p22 = 0   cmp.leu P1, P2 = -103, r3
// EXPECT p23 = 1
// EXPECT p24 = 0   cmp4.gt P1, P2 = -99, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p29 = 1
// EXPECT p30 = 0   tbit.z P1, P2 = r3, 10
// EXPECT p31 = 1
// EXPECT p32 = 1   cmp.gtu P1, P2 = -12, r3
// EXPECT p33 = 0
// EXPECT p34 = 0   cmp.le P1, P2 = -98, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   cmp4.gtu P1, P2 = 57, r3
// EXPECT p37 = 1
// EXPECT p38 = 1   cmp4.ge P1, P2 = 56, r3
// EXPECT p39 = 0
// EXPECT p40 = 0   cmp4.geu P1, P2 = -40, r3
// EXPECT p41 = 1
// EXPECT p42 = 1   cmp.geu P1, P2 = -59, r3
// EXPECT p43 = 0
// EXPECT p44 = 1   cmp4.eq P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   tbit.z P1, P2 = r3, 3
// EXPECT p47 = 1
// EXPECT p48 = 1   cmp4.ltu P1, P2 = -98, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   cmp.ne P1, P2 = -124, r3
// EXPECT p51 = 0
// EXPECT p52 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p53 = 0
// EXPECT p54 = 1   tbit.nz P1, P2 = r3, 31
// EXPECT p55 = 0
// EXPECT p56 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp4.leu P1, P2 = -112, r3
// EXPECT p59 = 0
// EXPECT p60 = 1   cmp4.ge P1, P2 = r2, r3
// EXPECT p61 = 0
// EXPECT p62 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0x7fffffffffffffff
  movl r3 = 0xe8624fabffffffff
  movl r4 = 0x20
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -116, r3
  and r13 = -116, r3
  or r14 = -116, r3
  xor r15 = -116, r3
  andcm r16 = -116, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -2402, r3
  addl r22 = 630344, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -2402, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 22, 18
  extr r37 = r3, 22, 18
  dep.z r38 = r2, 22, 18
  dep.z r39 = 56, 22, 18
  dep r40 = r2, r3, 22, 6
  dep r41 = -1, r3, 22, 18
  shrp r42 = r2, r3, 53
  shl r43 = r2, 53
  shr.u r44 = r3, 53
  shr r45 = r3, 53
  extr.u r46 = r3, 46, 46
  extr r47 = r3, 46, 46
  dep.z r48 = r2, 46, 46
  dep.z r49 = 57, 46, 46
  dep r50 = r2, r3, 46, 15
  dep r51 = 0, r3, 46, 46
  shrp r52 = r2, r3, 51
  shl r53 = r2, 51
  shr.u r54 = r3, 51
  shr r55 = r3, 51
  extr.u r56 = r3, 59, 32
  extr r57 = r3, 59, 32
  dep.z r58 = r2, 59, 32
  dep.z r59 = 122, 59, 32
  dep r60 = r2, r3, 59, 9
  dep r61 = -1, r3, 59, 32
  shrp r62 = r2, r3, 45
  shl r63 = r2, 45
  shr.u r64 = r3, 45
  shr r65 = r3, 45
  extr.u r66 = r3, 58, 60
  extr r67 = r3, 58, 60
  dep.z r68 = r2, 58, 60
  dep.z r69 = 51, 58, 60
  dep r70 = r2, r3, 58, 15
  dep r71 = -1, r3, 58, 60
  shrp r72 = r2, r3, 28
  shl r73 = r2, 28
  shr.u r74 = r3, 28
  shr r75 = r3, 28
  extr.u r76 = r3, 41, 22
  extr r77 = r3, 41, 22
  dep.z r78 = r2, 41, 22
  dep.z r79 = 9, 41, 22
  dep r80 = r2, r3, 41, 16
  dep r81 = -1, r3, 41, 22
  shrp r82 = r2, r3, 38
  shl r83 = r2, 38
  shr.u r84 = r3, 38
  shr r85 = r3, 38
  extr.u r86 = r3, 52, 40
  extr r87 = r3, 52, 40
  dep.z r88 = r2, 52, 40
  dep.z r89 = -22, 52, 40
  dep r90 = r2, r3, 52, 16
  dep r91 = -1, r3, 52, 40
  shrp r92 = r2, r3, 9
  shl r93 = r2, 9
  shr.u r94 = r3, 9
  shr r95 = r3, 9
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.lt p6, p7 = r2, r3
  cmp.eq p8, p9 = 46, r3
  cmp.ne p10, p11 = r2, r3
  tbit.nz p12, p13 = r3, 14
  cmp4.ne p14, p15 = -3, r3
  cmp.ge p16, p17 = r2, r3
  cmp.le p18, p19 = r2, r3
  cmp.leu p20, p21 = r2, r3
  cmp.leu p22, p23 = -103, r3
  cmp4.gt p24, p25 = -99, r3
  cmp.eq p26, p27 = r2, r3
  cmp4.lt p28, p29 = r2, r3
  tbit.z p30, p31 = r3, 10
  cmp.gtu p32, p33 = -12, r3
  cmp.le p34, p35 = -98, r3
  cmp4.gtu p36, p37 = 57, r3
  cmp4.ge p38, p39 = 56, r3
  cmp4.geu p40, p41 = -40, r3
  cmp.geu p42, p43 = -59, r3
  cmp4.eq p44, p45 = r2, r3
  tbit.z p46, p47 = r3, 3
  cmp4.ltu p48, p49 = -98, r3
  cmp.ne p50, p51 = -124, r3
  cmp4.geu p52, p53 = r2, r3
  tbit.nz p54, p55 = r3, 31
  cmp4.gtu p56, p57 = r2, r3
  cmp4.leu p58, p59 = -112, r3
  cmp4.ge p60, p61 = r2, r3
  cmp.gtu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
