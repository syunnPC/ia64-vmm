/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x47d433985b11bb39   add R = r2, r3
// EXPECT r9 = 0x47d433985b11bb3a   add R = r2, r3, 1
// EXPECT r10 = 0xb82bcc67a4ee44cb   sub R = r2, r3
// EXPECT r11 = 0xb82bcc67a4ee44ca   sub R = r2, r3, 1
// EXPECT r12 = 0xb82bcc67a4ee4532   sub R = 105, r3
// EXPECT r13 = 0x21   and R = 105, r3
// EXPECT r14 = 0x47d433985b11bb7f   or R = 105, r3
// EXPECT r15 = 0x47d433985b11bb5e   xor R = 105, r3
// EXPECT r16 = 0x48   andcm R = 105, r3
// EXPECT r17 = 0x2   and R = r2, r3
// EXPECT r18 = 0x47d433985b11bb37   or R = r2, r3
// EXPECT r19 = 0x47d433985b11bb35   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0x47d433985b11d0f5   adds R = 5566, r3
// EXPECT r22 = 0x1eabe9   addl R = 2010089, r1
// EXPECT r23 = 0x47d433985b11bb3b   shladd R = r2, 1, r3
// EXPECT r24 = 0x200000005b11bb3b   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x47d433985b11bb3f   shladd R = r2, 2, r3
// EXPECT r26 = 0x200000005b11bb3f   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x47d433985b11bb47   shladd R = r2, 3, r3
// EXPECT r28 = 0x200000005b11bb47   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x47d433985b11bb57   shladd R = r2, 4, r3
// EXPECT r30 = 0x200000005b11bb57   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x200000005b11bb39   addp4 R = r2, r3
// EXPECT r32 = 0x200000005b11d0f5   addp4 R = 5566, r3
// EXPECT r33 = 0x200000000   shl R = r2, r4
// EXPECT r34 = 0x47d43398   shr.u R = r3, r4
// EXPECT r35 = 0x47d43398   shr R = r3, r4
// EXPECT r36 = 0x11f50ce616c46e   extr.u R = r3, 10, 60
// EXPECT r37 = 0x11f50ce616c46e   extr R = r3, 10, 60
// EXPECT r38 = 0x800   dep.z R = r2, 10, 60
// EXPECT r39 = 0xe000   dep.z R = 56, 10, 60
// EXPECT r40 = 0x47d433985a000b37   dep R = r2, r3, 10, 15
// EXPECT r41 = 0xffffffffffffff37   dep R = -1, r3, 10, 60
// EXPECT r42 = 0x123   shrp R = r2, r3, 57
// EXPECT r43 = 0x400000000000000   shl R = r2, 57
// EXPECT r44 = 0x23   shr.u R = r3, 57
// EXPECT r45 = 0x23   shr R = r3, 57
// EXPECT r46 = 0xfa   extr.u R = r3, 51, 9
// EXPECT r47 = 0xfa   extr R = r3, 51, 9
// EXPECT r48 = 0x10000000000000   dep.z R = r2, 51, 9
// EXPECT r49 = 0x3d8000000000000   dep.z R = 123, 51, 9
// EXPECT r50 = 0x47d433985b11bb37   dep R = r2, r3, 51, 1
// EXPECT r51 = 0x400433985b11bb37   dep R = 0, r3, 51, 9
// EXPECT r52 = 0x123ea19cc2d88   shrp R = r2, r3, 17
// EXPECT r53 = 0x40000   shl R = r2, 17
// EXPECT r54 = 0x23ea19cc2d88   shr.u R = r3, 17
// EXPECT r55 = 0x23ea19cc2d88   shr R = r3, 17
// EXPECT r56 = 0x4   extr.u R = r3, 22, 3
// EXPECT r57 = 0xfffffffffffffffc   extr R = r3, 22, 3
// EXPECT r58 = 0x800000   dep.z R = r2, 22, 3
// EXPECT r59 = 0x800000   dep.z R = 18, 22, 3
// EXPECT r60 = 0x47d433800091bb37   dep R = r2, r3, 22, 16
// EXPECT r61 = 0x47d433985a11bb37   dep R = 0, r3, 22, 3
// EXPECT r62 = 0x247d433985b11b   shrp R = r2, r3, 12
// EXPECT r63 = 0x2000   shl R = r2, 12
// EXPECT r64 = 0x47d433985b11b   shr.u R = r3, 12
// EXPECT r65 = 0x47d433985b11b   shr R = r3, 12
// EXPECT r66 = 0x11f50ce616c46   extr.u R = r3, 14, 51
// EXPECT r67 = 0x11f50ce616c46   extr R = r3, 14, 51
// EXPECT r68 = 0x8000   dep.z R = r2, 14, 51
// EXPECT r69 = 0xffffffffffe60000   dep.z R = -104, 14, 51
// EXPECT r70 = 0x47d433985b10bb37   dep R = r2, r3, 14, 3
// EXPECT r71 = 0xfffffffffffffb37   dep R = -1, r3, 14, 51
// EXPECT r72 = 0x123ea19c   shrp R = r2, r3, 37
// EXPECT r73 = 0x4000000000   shl R = r2, 37
// EXPECT r74 = 0x23ea19c   shr.u R = r3, 37
// EXPECT r75 = 0x23ea19c   shr R = r3, 37
// EXPECT r76 = 0x8fa   extr.u R = r3, 51, 30
// EXPECT r77 = 0x8fa   extr R = r3, 51, 30
// EXPECT r78 = 0x10000000000000   dep.z R = r2, 51, 30
// EXPECT r79 = 0xfed8000000000000   dep.z R = -37, 51, 30
// EXPECT r80 = 0x1433985b11bb37   dep R = r2, r3, 51, 12
// EXPECT r81 = 0x433985b11bb37   dep R = 0, r3, 51, 30
// EXPECT r82 = 0x91f50ce61   shrp R = r2, r3, 30
// EXPECT r83 = 0x80000000   shl R = r2, 30
// EXPECT r84 = 0x11f50ce61   shr.u R = r3, 30
// EXPECT r85 = 0x11f50ce61   shr R = r3, 30
// EXPECT r86 = 0x8fa867   extr.u R = r3, 39, 40
// EXPECT r87 = 0x8fa867   extr R = r3, 39, 40
// EXPECT r88 = 0x10000000000   dep.z R = r2, 39, 40
// EXPECT r89 = 0xffffea0000000000   dep.z R = -44, 39, 40
// EXPECT r90 = 0x47d401185b11bb37   dep R = r2, r3, 39, 11
// EXPECT r91 = 0xffffff985b11bb37   dep R = -1, r3, 39, 40
// EXPECT r92 = 0x24   shrp R = r2, r3, 60
// EXPECT r93 = 0x2000000000000000   shl R = r2, 60
// EXPECT r94 = 0x4   shr.u R = r3, 60
// EXPECT r95 = 0x4   shr R = r3, 60
// EXPECT r96 = 0x37   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffbb37   sxt2 R = r3
// EXPECT r98 = 0x5b11bb37   sxt4 R = r3
// EXPECT r99 = 0x37   zxt1 R = r3
// EXPECT r100 = 0xbb37   zxt2 R = r3
// EXPECT r101 = 0x5b11bb37   zxt4 R = r3
// EXPECT r102 = 0x21   popcnt R = r3
// EXPECT p6 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p7 = 1
// EXPECT p8 = 1   cmp.ne P1, P2 = 98, r3
// EXPECT p9 = 0
// EXPECT p10 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p11 = 0
// EXPECT p12 = 1   cmp.lt P1, P2 = -91, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 1   cmp4.le P1, P2 = -96, r3
// EXPECT p17 = 0
// EXPECT p18 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p19 = 1
// EXPECT p20 = 0   cmp4.ge P1, P2 = 2, r3
// EXPECT p21 = 1
// EXPECT p22 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p23 = 0
// EXPECT p24 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   tbit.nz P1, P2 = r3, 63
// EXPECT p27 = 1
// EXPECT p28 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p29 = 1
// EXPECT p30 = 1   tbit.nz P1, P2 = r3, 5
// EXPECT p31 = 0
// EXPECT p32 = 0   cmp4.eq P1, P2 = -121, r3
// EXPECT p33 = 1
// EXPECT p34 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p35 = 0
// EXPECT p36 = 1   cmp4.ltu P1, P2 = 19, r3
// EXPECT p37 = 0
// EXPECT p38 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p39 = 0
// EXPECT p40 = 0   cmp.eq P1, P2 = 44, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp.geu P1, P2 = 64, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp4.leu P1, P2 = -107, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp.le P1, P2 = 114, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   tbit.nz P1, P2 = r3, 61
// EXPECT p49 = 1
// EXPECT p50 = 1   tbit.z P1, P2 = r3, 63
// EXPECT p51 = 0
// EXPECT p52 = 0   tbit.z P1, P2 = r3, 5
// EXPECT p53 = 1
// EXPECT p54 = 0   cmp.ge P1, P2 = 71, r3
// EXPECT p55 = 1
// EXPECT p56 = 1   tbit.z P1, P2 = r3, 61
// EXPECT p57 = 0
// EXPECT p58 = 0   cmp.gt P1, P2 = -69, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp4.geu P1, P2 = -47, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x2
  movl r3 = 0x47d433985b11bb37
  movl r4 = 0x20
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 105, r3
  and r13 = 105, r3
  or r14 = 105, r3
  xor r15 = 105, r3
  andcm r16 = 105, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 5566, r3
  addl r22 = 2010089, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 5566, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 10, 60
  extr r37 = r3, 10, 60
  dep.z r38 = r2, 10, 60
  dep.z r39 = 56, 10, 60
  dep r40 = r2, r3, 10, 15
  dep r41 = -1, r3, 10, 60
  shrp r42 = r2, r3, 57
  shl r43 = r2, 57
  shr.u r44 = r3, 57
  shr r45 = r3, 57
  extr.u r46 = r3, 51, 9
  extr r47 = r3, 51, 9
  dep.z r48 = r2, 51, 9
  dep.z r49 = 123, 51, 9
  dep r50 = r2, r3, 51, 1
  dep r51 = 0, r3, 51, 9
  shrp r52 = r2, r3, 17
  shl r53 = r2, 17
  shr.u r54 = r3, 17
  shr r55 = r3, 17
  extr.u r56 = r3, 22, 3
  extr r57 = r3, 22, 3
  dep.z r58 = r2, 22, 3
  dep.z r59 = 18, 22, 3
  dep r60 = r2, r3, 22, 16
  dep r61 = 0, r3, 22, 3
  shrp r62 = r2, r3, 12
  shl r63 = r2, 12
  shr.u r64 = r3, 12
  shr r65 = r3, 12
  extr.u r66 = r3, 14, 51
  extr r67 = r3, 14, 51
  dep.z r68 = r2, 14, 51
  dep.z r69 = -104, 14, 51
  dep r70 = r2, r3, 14, 3
  dep r71 = -1, r3, 14, 51
  shrp r72 = r2, r3, 37
  shl r73 = r2, 37
  shr.u r74 = r3, 37
  shr r75 = r3, 37
  extr.u r76 = r3, 51, 30
  extr r77 = r3, 51, 30
  dep.z r78 = r2, 51, 30
  dep.z r79 = -37, 51, 30
  dep r80 = r2, r3, 51, 12
  dep r81 = 0, r3, 51, 30
  shrp r82 = r2, r3, 30
  shl r83 = r2, 30
  shr.u r84 = r3, 30
  shr r85 = r3, 30
  extr.u r86 = r3, 39, 40
  extr r87 = r3, 39, 40
  dep.z r88 = r2, 39, 40
  dep.z r89 = -44, 39, 40
  dep r90 = r2, r3, 39, 11
  dep r91 = -1, r3, 39, 40
  shrp r92 = r2, r3, 60
  shl r93 = r2, 60
  shr.u r94 = r3, 60
  shr r95 = r3, 60
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.gt p6, p7 = r2, r3
  cmp.ne p8, p9 = 98, r3
  cmp4.leu p10, p11 = r2, r3
  cmp.lt p12, p13 = -91, r3
  cmp4.ge p14, p15 = r2, r3
  cmp4.le p16, p17 = -96, r3
  cmp.gtu p18, p19 = r2, r3
  cmp4.ge p20, p21 = 2, r3
  cmp4.le p22, p23 = r2, r3
  cmp.ge p24, p25 = r2, r3
  tbit.nz p26, p27 = r3, 63
  cmp.eq p28, p29 = r2, r3
  tbit.nz p30, p31 = r3, 5
  cmp4.eq p32, p33 = -121, r3
  cmp4.lt p34, p35 = r2, r3
  cmp4.ltu p36, p37 = 19, r3
  cmp4.ne p38, p39 = r2, r3
  cmp.eq p40, p41 = 44, r3
  cmp.geu p42, p43 = 64, r3
  cmp4.leu p44, p45 = -107, r3
  cmp.le p46, p47 = 114, r3
  tbit.nz p48, p49 = r3, 61
  tbit.z p50, p51 = r3, 63
  tbit.z p52, p53 = r3, 5
  cmp.ge p54, p55 = 71, r3
  tbit.z p56, p57 = r3, 61
  cmp.gt p58, p59 = -69, r3
  cmp.gt p60, p61 = r2, r3
  cmp4.geu p62, p63 = -47, r3
  ;;
  break.i 0x1ffffe
