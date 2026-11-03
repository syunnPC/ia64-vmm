/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x681f9b0cbf4e7af6   add R = r2, r3
// EXPECT r9 = 0x681f9b0cbf4e7af7   add R = r2, r3, 1
// EXPECT r10 = 0x681f9b0cbf4e7af6   sub R = r2, r3
// EXPECT r11 = 0x681f9b0cbf4e7af5   sub R = r2, r3, 1
// EXPECT r12 = 0x8000000000000016   sub R = 22, r3
// EXPECT r13 = 0x0   and R = 22, r3
// EXPECT r14 = 0x8000000000000016   or R = 22, r3
// EXPECT r15 = 0x8000000000000016   xor R = 22, r3
// EXPECT r16 = 0x16   andcm R = 22, r3
// EXPECT r17 = 0x8000000000000000   and R = r2, r3
// EXPECT r18 = 0xe81f9b0cbf4e7af6   or R = r2, r3
// EXPECT r19 = 0x681f9b0cbf4e7af6   xor R = r2, r3
// EXPECT r20 = 0x681f9b0cbf4e7af6   andcm R = r2, r3
// EXPECT r21 = 0x7fffffffffffefea   adds R = -4118, r3
// EXPECT r22 = 0xffffffffffe59c25   addl R = -1729499, r1
// EXPECT r23 = 0x503f36197e9cf5ec   shladd R = r2, 1, r3
// EXPECT r24 = 0x7e9cf5ec   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x207e6c32fd39ebd8   shladd R = r2, 2, r3
// EXPECT r26 = 0xfd39ebd8   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xc0fcd865fa73d7b0   shladd R = r2, 3, r3
// EXPECT r28 = 0xfa73d7b0   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x1f9b0cbf4e7af60   shladd R = r2, 4, r3
// EXPECT r30 = 0xf4e7af60   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0xbf4e7af6   addp4 R = r2, r3
// EXPECT r32 = 0xffffefea   addp4 R = -4118, r3
// EXPECT r33 = 0xe81f9b0cbf4e7af6   shl R = r2, r4
// EXPECT r34 = 0x8000000000000000   shr.u R = r3, r4
// EXPECT r35 = 0x8000000000000000   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 0, 60
// EXPECT r37 = 0x0   extr R = r3, 0, 60
// EXPECT r38 = 0x81f9b0cbf4e7af6   dep.z R = r2, 0, 60
// EXPECT r39 = 0x20   dep.z R = 32, 0, 60
// EXPECT r40 = 0x80000000000002f6   dep R = r2, r3, 0, 11
// EXPECT r41 = 0x8fffffffffffffff   dep R = -1, r3, 0, 60
// EXPECT r42 = 0x3f36197e9cf5ed00   shrp R = r2, r3, 55
// EXPECT r43 = 0x7b00000000000000   shl R = r2, 55
// EXPECT r44 = 0x100   shr.u R = r3, 55
// EXPECT r45 = 0xffffffffffffff00   shr R = r3, 55
// EXPECT r46 = 0x0   extr.u R = r3, 25, 8
// EXPECT r47 = 0x0   extr R = r3, 25, 8
// EXPECT r48 = 0x1ec000000   dep.z R = r2, 25, 8
// EXPECT r49 = 0x0   dep.z R = 0, 25, 8
// EXPECT r50 = 0x80000000ec000000   dep R = r2, r3, 25, 7
// EXPECT r51 = 0x80000001fe000000   dep R = -1, r3, 25, 8
// EXPECT r52 = 0x7e6c32fd39ebda00   shrp R = r2, r3, 54
// EXPECT r53 = 0xbd80000000000000   shl R = r2, 54
// EXPECT r54 = 0x200   shr.u R = r3, 54
// EXPECT r55 = 0xfffffffffffffe00   shr R = r3, 54
// EXPECT r56 = 0x0   extr.u R = r3, 19, 8
// EXPECT r57 = 0x0   extr R = r3, 19, 8
// EXPECT r58 = 0x7b00000   dep.z R = r2, 19, 8
// EXPECT r59 = 0x5900000   dep.z R = -78, 19, 8
// EXPECT r60 = 0x8000000000300000   dep R = r2, r3, 19, 3
// EXPECT r61 = 0x8000000000000000   dep R = 0, r3, 19, 8
// EXPECT r62 = 0xb400000000000000   shrp R = r2, r3, 5
// EXPECT r63 = 0x3f36197e9cf5ec0   shl R = r2, 5
// EXPECT r64 = 0x400000000000000   shr.u R = r3, 5
// EXPECT r65 = 0xfc00000000000000   shr R = r3, 5
// EXPECT r66 = 0x0   extr.u R = r3, 6, 53
// EXPECT r67 = 0x0   extr R = r3, 6, 53
// EXPECT r68 = 0x7e6c32fd39ebd80   dep.z R = r2, 6, 53
// EXPECT r69 = 0x7fffffffffffbc0   dep.z R = -17, 6, 53
// EXPECT r70 = 0x800000000002bd80   dep R = r2, r3, 6, 12
// EXPECT r71 = 0x8000000000000000   dep R = 0, r3, 6, 53
// EXPECT r72 = 0x865fa73d7b400000   shrp R = r2, r3, 41
// EXPECT r73 = 0x9cf5ec0000000000   shl R = r2, 41
// EXPECT r74 = 0x400000   shr.u R = r3, 41
// EXPECT r75 = 0xffffffffffc00000   shr R = r3, 41
// EXPECT r76 = 0x4000000000000   extr.u R = r3, 13, 53
// EXPECT r77 = 0xfffc000000000000   extr R = r3, 13, 53
// EXPECT r78 = 0xf36197e9cf5ec000   dep.z R = r2, 13, 53
// EXPECT r79 = 0xfffffffffff2e000   dep.z R = -105, 13, 53
// EXPECT r80 = 0x80000000075ec000   dep R = r2, r3, 13, 14
// EXPECT r81 = 0xffffffffffffe000   dep R = -1, r3, 13, 53
// EXPECT r82 = 0x7b40000000000000   shrp R = r2, r3, 9
// EXPECT r83 = 0x3f36197e9cf5ec00   shl R = r2, 9
// EXPECT r84 = 0x40000000000000   shr.u R = r3, 9
// EXPECT r85 = 0xffc0000000000000   shr R = r3, 9
// EXPECT r86 = 0x0   extr.u R = r3, 5, 46
// EXPECT r87 = 0x0   extr R = r3, 5, 46
// EXPECT r88 = 0x36197e9cf5ec0   dep.z R = r2, 5, 46
// EXPECT r89 = 0x7fffffffff620   dep.z R = -79, 5, 46
// EXPECT r90 = 0x8000000000001ec0   dep R = r2, r3, 5, 8
// EXPECT r91 = 0x8000000000000000   dep R = 0, r3, 5, 46
// EXPECT r92 = 0x3d7b400000000000   shrp R = r2, r3, 17
// EXPECT r93 = 0x36197e9cf5ec0000   shl R = r2, 17
// EXPECT r94 = 0x400000000000   shr.u R = r3, 17
// EXPECT r95 = 0xffffc00000000000   shr R = r3, 17
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0x0   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x0   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p7 = 1
// EXPECT p8 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p9 = 1
// EXPECT p10 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p11 = 0
// EXPECT p12 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp.eq P1, P2 = 13, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp4.ltu P1, P2 = -35, r3
// EXPECT p17 = 1
// EXPECT p18 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p19 = 0
// EXPECT p20 = 1   tbit.z P1, P2 = r3, 8
// EXPECT p21 = 0
// EXPECT p22 = 1   cmp.ge P1, P2 = -51, r3
// EXPECT p23 = 0
// EXPECT p24 = 0   cmp4.leu P1, P2 = -119, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp4.gtu P1, P2 = -104, r3
// EXPECT p29 = 0
// EXPECT p30 = 1   cmp4.lt P1, P2 = -111, r3
// EXPECT p31 = 0
// EXPECT p32 = 0   tbit.nz P1, P2 = r3, 20
// EXPECT p33 = 1
// EXPECT p34 = 1   tbit.z P1, P2 = r3, 21
// EXPECT p35 = 0
// EXPECT p36 = 1   cmp.ltu P1, P2 = 49, r3
// EXPECT p37 = 0
// EXPECT p38 = 1   cmp.gt P1, P2 = -112, r3
// EXPECT p39 = 0
// EXPECT p40 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p41 = 0
// EXPECT p42 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp.le P1, P2 = 112, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp4.le P1, P2 = -111, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   tbit.nz P1, P2 = r3, 21
// EXPECT p49 = 1
// EXPECT p50 = 1   cmp.geu P1, P2 = -124, r3
// EXPECT p51 = 0
// EXPECT p52 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p53 = 0
// EXPECT p54 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 1   cmp4.ne P1, P2 = 115, r3
// EXPECT p57 = 0
// EXPECT p58 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p61 = 1
// EXPECT p62 = 0   tbit.nz P1, P2 = r3, 37
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0xe81f9b0cbf4e7af6
  movl r3 = 0x8000000000000000
  movl r4 = 0x0
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 22, r3
  and r13 = 22, r3
  or r14 = 22, r3
  xor r15 = 22, r3
  andcm r16 = 22, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -4118, r3
  addl r22 = -1729499, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -4118, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 0, 60
  extr r37 = r3, 0, 60
  dep.z r38 = r2, 0, 60
  dep.z r39 = 32, 0, 60
  dep r40 = r2, r3, 0, 11
  dep r41 = -1, r3, 0, 60
  shrp r42 = r2, r3, 55
  shl r43 = r2, 55
  shr.u r44 = r3, 55
  shr r45 = r3, 55
  extr.u r46 = r3, 25, 8
  extr r47 = r3, 25, 8
  dep.z r48 = r2, 25, 8
  dep.z r49 = 0, 25, 8
  dep r50 = r2, r3, 25, 7
  dep r51 = -1, r3, 25, 8
  shrp r52 = r2, r3, 54
  shl r53 = r2, 54
  shr.u r54 = r3, 54
  shr r55 = r3, 54
  extr.u r56 = r3, 19, 8
  extr r57 = r3, 19, 8
  dep.z r58 = r2, 19, 8
  dep.z r59 = -78, 19, 8
  dep r60 = r2, r3, 19, 3
  dep r61 = 0, r3, 19, 8
  shrp r62 = r2, r3, 5
  shl r63 = r2, 5
  shr.u r64 = r3, 5
  shr r65 = r3, 5
  extr.u r66 = r3, 6, 53
  extr r67 = r3, 6, 53
  dep.z r68 = r2, 6, 53
  dep.z r69 = -17, 6, 53
  dep r70 = r2, r3, 6, 12
  dep r71 = 0, r3, 6, 53
  shrp r72 = r2, r3, 41
  shl r73 = r2, 41
  shr.u r74 = r3, 41
  shr r75 = r3, 41
  extr.u r76 = r3, 13, 53
  extr r77 = r3, 13, 53
  dep.z r78 = r2, 13, 53
  dep.z r79 = -105, 13, 53
  dep r80 = r2, r3, 13, 14
  dep r81 = -1, r3, 13, 53
  shrp r82 = r2, r3, 9
  shl r83 = r2, 9
  shr.u r84 = r3, 9
  shr r85 = r3, 9
  extr.u r86 = r3, 5, 46
  extr r87 = r3, 5, 46
  dep.z r88 = r2, 5, 46
  dep.z r89 = -79, 5, 46
  dep r90 = r2, r3, 5, 8
  dep r91 = 0, r3, 5, 46
  shrp r92 = r2, r3, 17
  shl r93 = r2, 17
  shr.u r94 = r3, 17
  shr r95 = r3, 17
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.ltu p6, p7 = r2, r3
  cmp.lt p8, p9 = r2, r3
  cmp4.geu p10, p11 = r2, r3
  cmp.ne p12, p13 = r2, r3
  cmp.eq p14, p15 = 13, r3
  cmp4.ltu p16, p17 = -35, r3
  cmp4.gtu p18, p19 = r2, r3
  tbit.z p20, p21 = r3, 8
  cmp.ge p22, p23 = -51, r3
  cmp4.leu p24, p25 = -119, r3
  cmp4.eq p26, p27 = r2, r3
  cmp4.gtu p28, p29 = -104, r3
  cmp4.lt p30, p31 = -111, r3
  tbit.nz p32, p33 = r3, 20
  tbit.z p34, p35 = r3, 21
  cmp.ltu p36, p37 = 49, r3
  cmp.gt p38, p39 = -112, r3
  cmp.gtu p40, p41 = r2, r3
  cmp4.gt p42, p43 = r2, r3
  cmp.le p44, p45 = 112, r3
  cmp4.le p46, p47 = -111, r3
  tbit.nz p48, p49 = r3, 21
  cmp.geu p50, p51 = -124, r3
  cmp.ge p52, p53 = r2, r3
  cmp.gt p54, p55 = r2, r3
  cmp4.ne p56, p57 = 115, r3
  cmp4.ge p58, p59 = r2, r3
  cmp.leu p60, p61 = r2, r3
  tbit.nz p62, p63 = r3, 37
  ;;
  break.i 0x1ffffe
