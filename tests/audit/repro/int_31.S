/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xf83646bf0324aac2   add R = r2, r3
// EXPECT r9 = 0xf83646bf0324aac3   add R = r2, r3, 1
// EXPECT r10 = 0xf83646bf0324aac4   sub R = r2, r3
// EXPECT r11 = 0xf83646bf0324aac3   sub R = r2, r3, 1
// EXPECT r12 = 0x7ffffffffffffff0   sub R = -17, r3
// EXPECT r13 = 0x7fffffffffffffef   and R = -17, r3
// EXPECT r14 = 0xffffffffffffffff   or R = -17, r3
// EXPECT r15 = 0x8000000000000010   xor R = -17, r3
// EXPECT r16 = 0x8000000000000000   andcm R = -17, r3
// EXPECT r17 = 0x783646bf0324aac3   and R = r2, r3
// EXPECT r18 = 0x7fffffffffffffff   or R = r2, r3
// EXPECT r19 = 0x7c9b940fcdb553c   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0x8000000000001549   adds R = 5450, r3
// EXPECT r22 = 0xffffffffffe7afd0   addl R = -1593392, r1
// EXPECT r23 = 0x706c8d7e06495585   shladd R = r2, 1, r3
// EXPECT r24 = 0x6000000006495585   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x60d91afc0c92ab0b   shladd R = r2, 2, r3
// EXPECT r26 = 0x600000000c92ab0b   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x41b235f819255617   shladd R = r2, 3, r3
// EXPECT r28 = 0x6000000019255617   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x3646bf0324aac2f   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000324aac2f   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x600000000324aac2   addp4 R = r2, r3
// EXPECT r32 = 0x6000000000001549   addp4 R = 5450, r3
// EXPECT r33 = 0x324aac300000000   shl R = r2, r4
// EXPECT r34 = 0x7fffffff   shr.u R = r3, r4
// EXPECT r35 = 0x7fffffff   shr R = r3, r4
// EXPECT r36 = 0xffffff   extr.u R = r3, 6, 24
// EXPECT r37 = 0xffffffffffffffff   extr R = r3, 6, 24
// EXPECT r38 = 0x92ab0c0   dep.z R = r2, 6, 24
// EXPECT r39 = 0xd80   dep.z R = 54, 6, 24
// EXPECT r40 = 0x7ffffffffffff8ff   dep R = r2, r3, 6, 5
// EXPECT r41 = 0x7fffffffc000003f   dep R = 0, r3, 6, 24
// EXPECT r42 = 0x2ab0dfffffffffff   shrp R = r2, r3, 18
// EXPECT r43 = 0x1afc0c92ab0c0000   shl R = r2, 18
// EXPECT r44 = 0x1fffffffffff   shr.u R = r3, 18
// EXPECT r45 = 0x1fffffffffff   shr R = r3, 18
// EXPECT r46 = 0xf   extr.u R = r3, 48, 4
// EXPECT r47 = 0xffffffffffffffff   extr R = r3, 48, 4
// EXPECT r48 = 0x3000000000000   dep.z R = r2, 48, 4
// EXPECT r49 = 0xc000000000000   dep.z R = -84, 48, 4
// EXPECT r50 = 0x2ac3ffffffffffff   dep R = r2, r3, 48, 15
// EXPECT r51 = 0x7ff0ffffffffffff   dep R = 0, r3, 48, 4
// EXPECT r52 = 0xc92ab0dfffffffff   shrp R = r2, r3, 26
// EXPECT r53 = 0xfc0c92ab0c000000   shl R = r2, 26
// EXPECT r54 = 0x1fffffffff   shr.u R = r3, 26
// EXPECT r55 = 0x1fffffffff   shr R = r3, 26
// EXPECT r56 = 0xff   extr.u R = r3, 55, 48
// EXPECT r57 = 0xff   extr R = r3, 55, 48
// EXPECT r58 = 0x6180000000000000   dep.z R = r2, 55, 48
// EXPECT r59 = 0xf180000000000000   dep.z R = -29, 55, 48
// EXPECT r60 = 0x61ffffffffffffff   dep R = r2, r3, 55, 11
// EXPECT r61 = 0x7fffffffffffff   dep R = 0, r3, 55, 48
// EXPECT r62 = 0xc1b235f81925561b   shrp R = r2, r3, 61
// EXPECT r63 = 0x6000000000000000   shl R = r2, 61
// EXPECT r64 = 0x3   shr.u R = r3, 61
// EXPECT r65 = 0x3   shr R = r3, 61
// EXPECT r66 = 0x3ffffffff   extr.u R = r3, 5, 34
// EXPECT r67 = 0xffffffffffffffff   extr R = r3, 5, 34
// EXPECT r68 = 0x6064955860   dep.z R = r2, 5, 34
// EXPECT r69 = 0x580   dep.z R = 44, 5, 34
// EXPECT r70 = 0x7fffffffffffd87f   dep R = r2, r3, 5, 10
// EXPECT r71 = 0x7fffffffffffffff   dep R = -1, r3, 5, 34
// EXPECT r72 = 0x91afc0c92ab0dfff   shrp R = r2, r3, 50
// EXPECT r73 = 0xab0c000000000000   shl R = r2, 50
// EXPECT r74 = 0x1fff   shr.u R = r3, 50
// EXPECT r75 = 0x1fff   shr R = r3, 50
// EXPECT r76 = 0x3   extr.u R = r3, 61, 41
// EXPECT r77 = 0x3   extr R = r3, 61, 41
// EXPECT r78 = 0x6000000000000000   dep.z R = r2, 61, 41
// EXPECT r79 = 0xa000000000000000   dep.z R = -19, 61, 41
// EXPECT r80 = 0x7fffffffffffffff   dep R = r2, r3, 61, 4
// EXPECT r81 = 0xffffffffffffffff   dep R = -1, r3, 61, 41
// EXPECT r82 = 0x92ab0dffffffffff   shrp R = r2, r3, 22
// EXPECT r83 = 0xafc0c92ab0c00000   shl R = r2, 22
// EXPECT r84 = 0x1ffffffffff   shr.u R = r3, 22
// EXPECT r85 = 0x1ffffffffff   shr R = r3, 22
// EXPECT r86 = 0xfff   extr.u R = r3, 37, 12
// EXPECT r87 = 0xffffffffffffffff   extr R = r3, 37, 12
// EXPECT r88 = 0x1586000000000   dep.z R = r2, 37, 12
// EXPECT r89 = 0x1fb0000000000   dep.z R = -40, 37, 12
// EXPECT r90 = 0x7ffff87fffffffff   dep R = r2, r3, 37, 7
// EXPECT r91 = 0x7ffe001fffffffff   dep R = 0, r3, 37, 12
// EXPECT r92 = 0x8d7e06495586ffff   shrp R = r2, r3, 47
// EXPECT r93 = 0x5561800000000000   shl R = r2, 47
// EXPECT r94 = 0xffff   shr.u R = r3, 47
// EXPECT r95 = 0xffff   shr R = r3, 47
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0xffffffffffffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0xffffffff   zxt4 R = r3
// EXPECT r102 = 0x3f   popcnt R = r3
// EXPECT p6 = 0   cmp4.geu P1, P2 = -45, r3
// EXPECT p7 = 1
// EXPECT p8 = 1   cmp4.gt P1, P2 = r2, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp.ge P1, P2 = 42, r3
// EXPECT p11 = 1
// EXPECT p12 = 1   cmp4.ne P1, P2 = -104, r3
// EXPECT p13 = 0
// EXPECT p14 = 1   tbit.z P1, P2 = r3, 63
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp4.leu P1, P2 = -67, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   tbit.nz P1, P2 = r3, 19
// EXPECT p19 = 0
// EXPECT p20 = 1   cmp4.ge P1, P2 = 43, r3
// EXPECT p21 = 0
// EXPECT p22 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p23 = 0
// EXPECT p24 = 1   cmp.ltu P1, P2 = 28, r3
// EXPECT p25 = 0
// EXPECT p26 = 1   tbit.nz P1, P2 = r3, 60
// EXPECT p27 = 0
// EXPECT p28 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   tbit.z P1, P2 = r3, 19
// EXPECT p31 = 1
// EXPECT p32 = 0   cmp.eq P1, P2 = -80, r3
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   cmp4.gtu P1, P2 = -26, r3
// EXPECT p37 = 1
// EXPECT p38 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p39 = 0
// EXPECT p40 = 0   cmp.leu P1, P2 = -70, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp.ne P1, P2 = 76, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p49 = 1
// EXPECT p50 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p51 = 1
// EXPECT p52 = 1   cmp.le P1, P2 = -28, r3
// EXPECT p53 = 0
// EXPECT p54 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p55 = 1
// EXPECT p56 = 1   cmp.lt P1, P2 = 17, r3
// EXPECT p57 = 0
// EXPECT p58 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p59 = 1
// EXPECT p60 = 1   cmp.lt P1, P2 = r2, r3
// EXPECT p61 = 0
// EXPECT p62 = 1   cmp4.ltu P1, P2 = 9, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x783646bf0324aac3
  movl r3 = 0x7fffffffffffffff
  movl r4 = 0x20
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -17, r3
  and r13 = -17, r3
  or r14 = -17, r3
  xor r15 = -17, r3
  andcm r16 = -17, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 5450, r3
  addl r22 = -1593392, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 5450, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 6, 24
  extr r37 = r3, 6, 24
  dep.z r38 = r2, 6, 24
  dep.z r39 = 54, 6, 24
  dep r40 = r2, r3, 6, 5
  dep r41 = 0, r3, 6, 24
  shrp r42 = r2, r3, 18
  shl r43 = r2, 18
  shr.u r44 = r3, 18
  shr r45 = r3, 18
  extr.u r46 = r3, 48, 4
  extr r47 = r3, 48, 4
  dep.z r48 = r2, 48, 4
  dep.z r49 = -84, 48, 4
  dep r50 = r2, r3, 48, 15
  dep r51 = 0, r3, 48, 4
  shrp r52 = r2, r3, 26
  shl r53 = r2, 26
  shr.u r54 = r3, 26
  shr r55 = r3, 26
  extr.u r56 = r3, 55, 48
  extr r57 = r3, 55, 48
  dep.z r58 = r2, 55, 48
  dep.z r59 = -29, 55, 48
  dep r60 = r2, r3, 55, 11
  dep r61 = 0, r3, 55, 48
  shrp r62 = r2, r3, 61
  shl r63 = r2, 61
  shr.u r64 = r3, 61
  shr r65 = r3, 61
  extr.u r66 = r3, 5, 34
  extr r67 = r3, 5, 34
  dep.z r68 = r2, 5, 34
  dep.z r69 = 44, 5, 34
  dep r70 = r2, r3, 5, 10
  dep r71 = -1, r3, 5, 34
  shrp r72 = r2, r3, 50
  shl r73 = r2, 50
  shr.u r74 = r3, 50
  shr r75 = r3, 50
  extr.u r76 = r3, 61, 41
  extr r77 = r3, 61, 41
  dep.z r78 = r2, 61, 41
  dep.z r79 = -19, 61, 41
  dep r80 = r2, r3, 61, 4
  dep r81 = -1, r3, 61, 41
  shrp r82 = r2, r3, 22
  shl r83 = r2, 22
  shr.u r84 = r3, 22
  shr r85 = r3, 22
  extr.u r86 = r3, 37, 12
  extr r87 = r3, 37, 12
  dep.z r88 = r2, 37, 12
  dep.z r89 = -40, 37, 12
  dep r90 = r2, r3, 37, 7
  dep r91 = 0, r3, 37, 12
  shrp r92 = r2, r3, 47
  shl r93 = r2, 47
  shr.u r94 = r3, 47
  shr r95 = r3, 47
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.geu p6, p7 = -45, r3
  cmp4.gt p8, p9 = r2, r3
  cmp.ge p10, p11 = 42, r3
  cmp4.ne p12, p13 = -104, r3
  tbit.z p14, p15 = r3, 63
  cmp4.leu p16, p17 = -67, r3
  tbit.nz p18, p19 = r3, 19
  cmp4.ge p20, p21 = 43, r3
  cmp.ne p22, p23 = r2, r3
  cmp.ltu p24, p25 = 28, r3
  tbit.nz p26, p27 = r3, 60
  cmp.le p28, p29 = r2, r3
  tbit.z p30, p31 = r3, 19
  cmp.eq p32, p33 = -80, r3
  cmp.ge p34, p35 = r2, r3
  cmp4.gtu p36, p37 = -26, r3
  cmp4.leu p38, p39 = r2, r3
  cmp.leu p40, p41 = -70, r3
  cmp4.geu p42, p43 = r2, r3
  cmp.eq p44, p45 = r2, r3
  cmp.ne p46, p47 = 76, r3
  cmp.gt p48, p49 = r2, r3
  cmp.geu p50, p51 = r2, r3
  cmp.le p52, p53 = -28, r3
  cmp.gtu p54, p55 = r2, r3
  cmp.lt p56, p57 = 17, r3
  cmp4.gtu p58, p59 = r2, r3
  cmp.lt p60, p61 = r2, r3
  cmp4.ltu p62, p63 = 9, r3
  ;;
  break.i 0x1ffffe
