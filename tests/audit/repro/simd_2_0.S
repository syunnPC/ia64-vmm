/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x34edcab84f7f7e01   padd1 R = r2, r3
// EXPECT r9 = 0xcaef324653818001   psub1 R = r2, r3
// EXPECT r10 = 0x34edcab84f7f7e01   padd1.sss R = r2, r3
// EXPECT r11 = 0x7fef324653818001   psub1.sss R = r2, r3
// EXPECT r12 = 0x34edcab84f7fff01   padd1.uus R = r2, r3
// EXPECT r13 = 0xcaefffff53008001   psub1.uus R = r2, r3
// EXPECT r14 = 0xffffffffff7fff01   padd1.uuu R = r2, r3
// EXPECT r15 = 0x324600008001   psub1.uuu R = r2, r3
// EXPECT r16 = 0x9af7e5dca73fbf01   pavg1 R = r2, r3
// EXPECT r17 = 0x9af7e5dca840bf01   pavg1.raz R = r2, r3
// EXPECT r18 = 0xe5f71923a9c14001   pavgsub1 R = r2, r3
// EXPECT r19 = 0x35edcbb84f7f7e01   padd2 R = r2, r3
// EXPECT r20 = 0xc9ef324652818001   psub2 R = r2, r3
// EXPECT r21 = 0x35edcbb84f7f7e01   padd2.sss R = r2, r3
// EXPECT r22 = 0x7fff324652818001   psub2.sss R = r2, r3
// EXPECT r23 = 0x35edcbb84f7fffff   padd2.uus R = r2, r3
// EXPECT r24 = 0xc9efffff52818001   psub2.uus R = r2, r3
// EXPECT r25 = 0xffffffffffffffff   padd2.uuu R = r2, r3
// EXPECT r26 = 0x324600008001   psub2.uuu R = r2, r3
// EXPECT r27 = 0x9af7e5dca7bfbf01   pavg2 R = r2, r3
// EXPECT r28 = 0x9af7e5dca7c0bf01   pavg2.raz R = r2, r3
// EXPECT r29 = 0xe4f71923a9414001   pavgsub2 R = r2, r3
// EXPECT r30 = 0x35eecbb84f807e01   padd4 R = r2, r3
// EXPECT r31 = 0xc9ef324652818001   psub4 R = r2, r3
// EXPECT r32 = 0x0   pcmp1.eq R = r2, r3
// EXPECT r33 = 0xff00ffffff0000ff   pcmp1.gt R = r2, r3
// EXPECT r34 = 0x0   pcmp2.eq R = r2, r3
// EXPECT r35 = 0xffffffffffff0000   pcmp2.gt R = r2, r3
// EXPECT r36 = 0x0   pcmp4.eq R = r2, r3
// EXPECT r37 = 0xffffffffffffffff   pcmp4.gt R = r2, r3
// EXPECT r38 = 0xb5fffefffe7fff01   pmax1.u R = r2, r3
// EXPECT r39 = 0x7feeccb951007f00   pmin1.u R = r2, r3
// EXPECT r40 = 0x7feefeff51007f00   pmax2 R = r2, r3
// EXPECT r41 = 0xb5ffccb9fe7fff01   pmin2 R = r2, r3
// EXPECT r42 = 0x337a47ff817f00   pmpy2.r R = r2, r3
// EXPECT r43 = 0xdb04b412ff862f00   pmpy2.l R = r2, r3
// EXPECT r44 = 0xb4127a472f007f00   pmpyshr2 R = r2, r3, 0
// EXPECT r45 = 0xb4127a472f007f00   pmpyshr2.u R = r2, r3, 0
// EXPECT r46 = 0x96866f40c5e02fe   pmpyshr2 R = r2, r3, 7
// EXPECT r47 = 0xe568d6f40c5e02fe   pmpyshr2.u R = r2, r3, 7
// EXPECT r48 = 0xb6090066ff0cff02   pmpyshr2 R = r2, r3, 15
// EXPECT r49 = 0xb5e597d6a10cfd02   pmpyshr2.u R = r2, r3, 15
// EXPECT r50 = 0xdb040033ff86ff81   pmpyshr2 R = r2, r3, 16
// EXPECT r51 = 0x5af2cbeb50867e81   pmpyshr2.u R = r2, r3, 16
// EXPECT r52 = 0x7feefeff5100ff01   pshl2 R = r2, 0
// EXPECT r53 = 0x7feefeff5100ff01   pshl4 R = r2, 0
// EXPECT r54 = 0xb5ffccb9fe7f7f00   pshr2 R = r3, 0
// EXPECT r55 = 0xb5ffccb9fe7f7f00   pshr2.u R = r3, 0
// EXPECT r56 = 0xb5ffccb9fe7f7f00   pshr4 R = r3, 0
// EXPECT r57 = 0xb5ffccb9fe7f7f00   pshr4.u R = r3, 0
// EXPECT r58 = 0xffdcfdfea200fe02   pshl2 R = r2, 1
// EXPECT r59 = 0xffddfdfea201fe02   pshl4 R = r2, 1
// EXPECT r60 = 0xdaffe65cff3f3f80   pshr2 R = r3, 1
// EXPECT r61 = 0x5aff665c7f3f3f80   pshr2.u R = r3, 1
// EXPECT r62 = 0xdaffe65cff3fbf80   pshr4 R = r3, 1
// EXPECT r63 = 0x5affe65c7f3fbf80   pshr4.u R = r3, 1
// EXPECT r64 = 0x800000008000   pshl2 R = r2, 15
// EXPECT r65 = 0x7f7f80007f808000   pshl4 R = r2, 15
// EXPECT r66 = 0xffffffffffff0000   pshr2 R = r3, 15
// EXPECT r67 = 0x1000100010000   pshr2.u R = r3, 15
// EXPECT r68 = 0xffff6bfffffffcfe   pshr4 R = r3, 15
// EXPECT r69 = 0x16bff0001fcfe   pshr4.u R = r3, 15
// EXPECT r70 = 0x0   pshl2 R = r2, 16
// EXPECT r71 = 0xfeff0000ff010000   pshl4 R = r2, 16
// EXPECT r72 = 0xffffffffffff0000   pshr2 R = r3, 16
// EXPECT r73 = 0x0   pshr2.u R = r3, 16
// EXPECT r74 = 0xffffb5fffffffe7f   pshr4 R = r3, 16
// EXPECT r75 = 0xb5ff0000fe7f   pshr4.u R = r3, 16
// EXPECT r76 = 0x0   pshl2 R = r2, 31
// EXPECT r77 = 0x8000000080000000   pshl4 R = r2, 31
// EXPECT r78 = 0xffffffffffff0000   pshr2 R = r3, 31
// EXPECT r79 = 0x0   pshr2.u R = r3, 31
// EXPECT r80 = 0xffffffffffffffff   pshr4 R = r3, 31
// EXPECT r81 = 0x100000001   pshr4.u R = r3, 31
// EXPECT r82 = 0x0   pshl2 R = r2, r4
// EXPECT r83 = 0xfdfe0000fe020000   pshl4 R = r2, r4
// EXPECT r84 = 0xffffffffffff0000   pshr2 R = r3, r4
// EXPECT r85 = 0x0   pshr2.u R = r3, r4
// EXPECT r86 = 0xffffdaffffffff3f   pshr4 R = r3, r4
// EXPECT r87 = 0x5aff00007f3f   pshr4.u R = r3, r4
// EXPECT r88 = 0x7fffcab77fff7d02   pshladd2 R = r2, 1, r3
// EXPECT r89 = 0xf5f6cc3826ff7e80   pshradd2 R = r2, 1, r3
// EXPECT r90 = 0x7fffc8b57fff7b04   pshladd2 R = r2, 2, r3
// EXPECT r91 = 0xd5facc7812bf7ec0   pshradd2 R = r2, 2, r3
// EXPECT r92 = 0x7fffc4b17fff7708   pshladd2 R = r2, 3, r3
// EXPECT r93 = 0xc5fccc98089f7ee0   pshradd2 R = r2, 3, r3
// EXPECT r94 = 0x26c   psad1 R = r2, r3
// EXPECT r95 = 0x1ff0051fffeee7f   mux1 R = r2, @rev
// EXPECT r96 = 0x7f51feffee00ff01   mux1 R = r2, @mix
// EXPECT r97 = 0x7f51ee00feffff01   mux1 R = r2, @shuf
// EXPECT r98 = 0x7ffe51ffeeff0001   mux1 R = r2, @alt
// EXPECT r99 = 0x101010101010101   mux1 R = r2, @brcst
// EXPECT r100 = 0xff015100feff7fee   mux2 R = r2, 0x1b
// EXPECT r101 = 0xfeffff017fee5100   mux2 R = r2, 0x8d
// EXPECT r102 = 0x7fee5100feffff01   mux2 R = r2, 0xd8
// EXPECT r103 = 0xfefffefffefffeff   mux2 R = r2, 0xaa
// EXPECT r104 = 0x7feefeff5100ff01   mux2 R = r2, 0xe4
// EXPECT r105 = 0x7fb5fecc51feff7f   mix1.l R = r2, r3
// EXPECT r106 = 0xeeffffb9007f0100   mix1.r R = r2, r3
// EXPECT r107 = 0x7fb5eefffeccffb9   unpack1.h R = r2, r3
// EXPECT r108 = 0x51fe007fff7f0100   unpack1.l R = r2, r3
// EXPECT r109 = 0x7feeb5ff5100fe7f   mix2.l R = r2, r3
// EXPECT r110 = 0xfeffccb9ff017f00   mix2.r R = r2, r3
// EXPECT r111 = 0x7feeb5fffeffccb9   unpack2.h R = r2, r3
// EXPECT r112 = 0x5100fe7fff017f00   unpack2.l R = r2, r3
// EXPECT r113 = 0x7feefeffb5ffccb9   mix4.l R = r2, r3
// EXPECT r114 = 0x5100ff01fe7f7f00   mix4.r R = r2, r3
// EXPECT r115 = 0x7feefeffb5ffccb9   unpack4.h R = r2, r3
// EXPECT r116 = 0x5100ff01fe7f7f00   unpack4.l R = r2, r3
// EXPECT r117 = 0x8080807f7f807f80   pack2.sss R = r2, r3
.text
.global _start
_start:
  movl r2 = 0x7feefeff5100ff01
  movl r3 = 0xb5ffccb9fe7f7f00
  movl r4 = 0x11
  movl r5 = 0x7f00fe000000ff00
  ;;
  padd1 r8 = r2, r3
  psub1 r9 = r2, r3
  padd1.sss r10 = r2, r3
  psub1.sss r11 = r2, r3
  padd1.uus r12 = r2, r3
  psub1.uus r13 = r2, r3
  padd1.uuu r14 = r2, r3
  psub1.uuu r15 = r2, r3
  pavg1 r16 = r2, r3
  pavg1.raz r17 = r2, r3
  pavgsub1 r18 = r2, r3
  padd2 r19 = r2, r3
  psub2 r20 = r2, r3
  padd2.sss r21 = r2, r3
  psub2.sss r22 = r2, r3
  padd2.uus r23 = r2, r3
  psub2.uus r24 = r2, r3
  padd2.uuu r25 = r2, r3
  psub2.uuu r26 = r2, r3
  pavg2 r27 = r2, r3
  pavg2.raz r28 = r2, r3
  pavgsub2 r29 = r2, r3
  padd4 r30 = r2, r3
  psub4 r31 = r2, r3
  pcmp1.eq r32 = r2, r3
  pcmp1.gt r33 = r2, r3
  pcmp2.eq r34 = r2, r3
  pcmp2.gt r35 = r2, r3
  pcmp4.eq r36 = r2, r3
  pcmp4.gt r37 = r2, r3
  pmax1.u r38 = r2, r3
  pmin1.u r39 = r2, r3
  pmax2 r40 = r2, r3
  pmin2 r41 = r2, r3
  pmpy2.r r42 = r2, r3
  pmpy2.l r43 = r2, r3
  pmpyshr2 r44 = r2, r3, 0
  pmpyshr2.u r45 = r2, r3, 0
  pmpyshr2 r46 = r2, r3, 7
  pmpyshr2.u r47 = r2, r3, 7
  pmpyshr2 r48 = r2, r3, 15
  pmpyshr2.u r49 = r2, r3, 15
  pmpyshr2 r50 = r2, r3, 16
  pmpyshr2.u r51 = r2, r3, 16
  pshl2 r52 = r2, 0
  pshl4 r53 = r2, 0
  pshr2 r54 = r3, 0
  pshr2.u r55 = r3, 0
  pshr4 r56 = r3, 0
  pshr4.u r57 = r3, 0
  pshl2 r58 = r2, 1
  pshl4 r59 = r2, 1
  pshr2 r60 = r3, 1
  pshr2.u r61 = r3, 1
  pshr4 r62 = r3, 1
  pshr4.u r63 = r3, 1
  pshl2 r64 = r2, 15
  pshl4 r65 = r2, 15
  pshr2 r66 = r3, 15
  pshr2.u r67 = r3, 15
  pshr4 r68 = r3, 15
  pshr4.u r69 = r3, 15
  pshl2 r70 = r2, 16
  pshl4 r71 = r2, 16
  pshr2 r72 = r3, 16
  pshr2.u r73 = r3, 16
  pshr4 r74 = r3, 16
  pshr4.u r75 = r3, 16
  pshl2 r76 = r2, 31
  pshl4 r77 = r2, 31
  pshr2 r78 = r3, 31
  pshr2.u r79 = r3, 31
  pshr4 r80 = r3, 31
  pshr4.u r81 = r3, 31
  pshl2 r82 = r2, r4
  pshl4 r83 = r2, r4
  pshr2 r84 = r3, r4
  pshr2.u r85 = r3, r4
  pshr4 r86 = r3, r4
  pshr4.u r87 = r3, r4
  pshladd2 r88 = r2, 1, r3
  pshradd2 r89 = r2, 1, r3
  pshladd2 r90 = r2, 2, r3
  pshradd2 r91 = r2, 2, r3
  pshladd2 r92 = r2, 3, r3
  pshradd2 r93 = r2, 3, r3
  psad1 r94 = r2, r3
  mux1 r95 = r2, @rev
  mux1 r96 = r2, @mix
  mux1 r97 = r2, @shuf
  mux1 r98 = r2, @alt
  mux1 r99 = r2, @brcst
  mux2 r100 = r2, 0x1b
  mux2 r101 = r2, 0x8d
  mux2 r102 = r2, 0xd8
  mux2 r103 = r2, 0xaa
  mux2 r104 = r2, 0xe4
  mix1.l r105 = r2, r3
  mix1.r r106 = r2, r3
  unpack1.h r107 = r2, r3
  unpack1.l r108 = r2, r3
  mix2.l r109 = r2, r3
  mix2.r r110 = r2, r3
  unpack2.h r111 = r2, r3
  unpack2.l r112 = r2, r3
  mix4.l r113 = r2, r3
  mix4.r r114 = r2, r3
  unpack4.h r115 = r2, r3
  unpack4.l r116 = r2, r3
  pack2.sss r117 = r2, r3
  ;;
  break.i 0x1ffffe
