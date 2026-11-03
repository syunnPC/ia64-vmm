/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xffd79efe0180abff   padd1 R = r2, r3
// EXPECT r9 = 0xff276000ff805303   psub1 R = r2, r3
// EXPECT r10 = 0xffd79efe01807fff   padd1.sss R = r2, r3
// EXPECT r11 = 0x7f276000ff7f5303   psub1.sss R = r2, r3
// EXPECT r12 = 0xd79efe0100ab00   padd1.uus R = r2, r3
// EXPECT r13 = 0xffffffff00805303   psub1.uus R = r2, r3
// EXPECT r14 = 0xffffffff0180abff   padd1.uuu R = r2, r3
// EXPECT r15 = 0x27600000005300   psub1.uuu R = r2, r3
// EXPECT r16 = 0x7febcfff0140557f   pavg1 R = r2, r3
// EXPECT r17 = 0x80eccfff01405680   pavg1.raz R = r2, r3
// EXPECT r18 = 0xff133000ffc02981   pavgsub1 R = r2, r3
// EXPECT r19 = 0xd79ffe0180abff   padd2 R = r2, r3
// EXPECT r20 = 0xff276000fe805203   psub2 R = r2, r3
// EXPECT r21 = 0xd79ffe01807fff   padd2.sss R = r2, r3
// EXPECT r22 = 0x7fff6000fe805203   psub2.sss R = r2, r3
// EXPECT r23 = 0xd79ffe0180abff   padd2.uus R = r2, r3
// EXPECT r24 = 0xff27ffff00005203   psub2.uus R = r2, r3
// EXPECT r25 = 0xffffffff0180abff   padd2.uuu R = r2, r3
// EXPECT r26 = 0x600000005203   psub2.uuu R = r2, r3
// EXPECT r27 = 0x806bcfff00c055ff   pavg2 R = r2, r3
// EXPECT r28 = 0x806ccfff00c05600   pavg2.raz R = r2, r3
// EXPECT r29 = 0xff933000ff402901   pavgsub2 R = r2, r3
// EXPECT r30 = 0xd89ffe0180abff   padd4 R = r2, r3
// EXPECT r31 = 0xff276000fe805203   psub4 R = r2, r3
// EXPECT r32 = 0xff00000000   pcmp1.eq R = r2, r3
// EXPECT r33 = 0xffffff0000ffffff   pcmp1.gt R = r2, r3
// EXPECT r34 = 0x0   pcmp2.eq R = r2, r3
// EXPECT r35 = 0xffffffff0000ffff   pcmp2.gt R = r2, r3
// EXPECT r36 = 0x0   pcmp4.eq R = r2, r3
// EXPECT r37 = 0xffffffff00000000   pcmp4.gt R = r2, r3
// EXPECT r38 = 0x80ffffff01807ffe   pmax1.u R = r2, r3
// EXPECT r39 = 0x7fd89fff00002c01   pmin1.u R = r2, r3
// EXPECT r40 = 0x7fffffff01807f01   pmax2 R = r2, r3
// EXPECT r41 = 0x80d89fff00002cfe   pmin2 R = r2, r3
// EXPECT r42 = 0x600116522efe   pmpy2.r R = r2, r3
// EXPECT r43 = 0xc06c7f2800000000   pmpy2.l R = r2, r3
// EXPECT r44 = 0x7f28600100002efe   pmpyshr2 R = r2, r3, 0
// EXPECT r45 = 0x7f28600100002efe   pmpyshr2.u R = r2, r3, 0
// EXPECT r46 = 0xd8fe00c00000a45d   pmpyshr2 R = r2, r3, 7
// EXPECT r47 = 0xd6fefcc00000a45d   pmpyshr2.u R = r2, r3, 7
// EXPECT r48 = 0x80d8000000002ca4   pmpyshr2 R = r2, r3, 15
// EXPECT r49 = 0x80d63ffc00002ca4   pmpyshr2.u R = r2, r3, 15
// EXPECT r50 = 0xc06c000000001652   pmpyshr2 R = r2, r3, 16
// EXPECT r51 = 0x406b9ffe00001652   pmpyshr2.u R = r2, r3, 16
// EXPECT r52 = 0x7fffffff00007f01   pshl2 R = r2, 0
// EXPECT r53 = 0x7fffffff00007f01   pshl4 R = r2, 0
// EXPECT r54 = 0x80d89fff01802cfe   pshr2 R = r3, 0
// EXPECT r55 = 0x80d89fff01802cfe   pshr2.u R = r3, 0
// EXPECT r56 = 0x80d89fff01802cfe   pshr4 R = r3, 0
// EXPECT r57 = 0x80d89fff01802cfe   pshr4.u R = r3, 0
// EXPECT r58 = 0xfffefffe0000fe02   pshl2 R = r2, 1
// EXPECT r59 = 0xfffffffe0000fe02   pshl4 R = r2, 1
// EXPECT r60 = 0xc06ccfff00c0167f   pshr2 R = r3, 1
// EXPECT r61 = 0x406c4fff00c0167f   pshr2.u R = r3, 1
// EXPECT r62 = 0xc06c4fff00c0167f   pshr4 R = r3, 1
// EXPECT r63 = 0x406c4fff00c0167f   pshr4.u R = r3, 1
// EXPECT r64 = 0x8000800000008000   pshl2 R = r2, 15
// EXPECT r65 = 0xffff80003f808000   pshl4 R = r2, 15
// EXPECT r66 = 0xffffffff00000000   pshr2 R = r3, 15
// EXPECT r67 = 0x1000100000000   pshr2.u R = r3, 15
// EXPECT r68 = 0xffff01b100000300   pshr4 R = r3, 15
// EXPECT r69 = 0x101b100000300   pshr4.u R = r3, 15
// EXPECT r70 = 0x0   pshl2 R = r2, 16
// EXPECT r71 = 0xffff00007f010000   pshl4 R = r2, 16
// EXPECT r72 = 0xffffffff00000000   pshr2 R = r3, 16
// EXPECT r73 = 0x0   pshr2.u R = r3, 16
// EXPECT r74 = 0xffff80d800000180   pshr4 R = r3, 16
// EXPECT r75 = 0x80d800000180   pshr4.u R = r3, 16
// EXPECT r76 = 0x0   pshl2 R = r2, 31
// EXPECT r77 = 0x8000000080000000   pshl4 R = r2, 31
// EXPECT r78 = 0xffffffff00000000   pshr2 R = r3, 31
// EXPECT r79 = 0x0   pshr2.u R = r3, 31
// EXPECT r80 = 0xffffffff00000000   pshr4 R = r3, 31
// EXPECT r81 = 0x100000000   pshr4.u R = r3, 31
// EXPECT r82 = 0x7fffffff00007f01   pshl2 R = r2, r4
// EXPECT r83 = 0x7fffffff00007f01   pshl4 R = r2, r4
// EXPECT r84 = 0x80d89fff01802cfe   pshr2 R = r3, r4
// EXPECT r85 = 0x80d89fff01802cfe   pshr2.u R = r3, r4
// EXPECT r86 = 0x80d89fff01802cfe   pshr4 R = r3, r4
// EXPECT r87 = 0x80d89fff01802cfe   pshr4.u R = r3, r4
// EXPECT r88 = 0x7fff9ffd01807fff   pshladd2 R = r2, 1, r3
// EXPECT r89 = 0xc0d79ffe01806c7e   pshradd2 R = r2, 1, r3
// EXPECT r90 = 0x7fff9ffb01807fff   pshladd2 R = r2, 2, r3
// EXPECT r91 = 0xa0d79ffe01804cbe   pshradd2 R = r2, 2, r3
// EXPECT r92 = 0x7fff9ff701807fff   pshladd2 R = r2, 3, r3
// EXPECT r93 = 0x90d79ffe01803cde   pshradd2 R = r2, 3, r3
// EXPECT r94 = 0x259   psad1 R = r2, r3
// EXPECT r95 = 0x17f0000ffffff7f   mux1 R = r2, @rev
// EXPECT r96 = 0x7f00ff7fff00ff01   mux1 R = r2, @mix
// EXPECT r97 = 0x7f00ff00ff7fff01   mux1 R = r2, @shuf
// EXPECT r98 = 0x7fff007fffff0001   mux1 R = r2, @alt
// EXPECT r99 = 0x101010101010101   mux1 R = r2, @brcst
// EXPECT r100 = 0x7f010000ffff7fff   mux2 R = r2, 0x1b
// EXPECT r101 = 0xffff7f017fff0000   mux2 R = r2, 0x8d
// EXPECT r102 = 0x7fff0000ffff7f01   mux2 R = r2, 0xd8
// EXPECT r103 = 0xffffffffffffffff   mux2 R = r2, 0xaa
// EXPECT r104 = 0x7fffffff00007f01   mux2 R = r2, 0xe4
// EXPECT r105 = 0x7f80ff9f00017f2c   mix1.l R = r2, r3
// EXPECT r106 = 0xffd8ffff008001fe   mix1.r R = r2, r3
// EXPECT r107 = 0x7f80ffd8ff9fffff   unpack1.h R = r2, r3
// EXPECT r108 = 0x100807f2c01fe   unpack1.l R = r2, r3
// EXPECT r109 = 0x7fff80d800000180   mix2.l R = r2, r3
// EXPECT r110 = 0xffff9fff7f012cfe   mix2.r R = r2, r3
// EXPECT r111 = 0x7fff80d8ffff9fff   unpack2.h R = r2, r3
// EXPECT r112 = 0x1807f012cfe   unpack2.l R = r2, r3
// EXPECT r113 = 0x7fffffff80d89fff   mix4.l R = r2, r3
// EXPECT r114 = 0x7f0101802cfe   mix4.r R = r2, r3
// EXPECT r115 = 0x7fffffff80d89fff   unpack4.h R = r2, r3
// EXPECT r116 = 0x7f0101802cfe   unpack4.l R = r2, r3
// EXPECT r117 = 0x80807f7f7fff007f   pack2.sss R = r2, r3
.text
.global _start
_start:
  movl r2 = 0x7fffffff00007f01
  movl r3 = 0x80d89fff01802cfe
  movl r4 = 0x0
  movl r5 = 0x7f00ff0000007f00
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
