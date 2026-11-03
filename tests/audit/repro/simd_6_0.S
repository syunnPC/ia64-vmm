/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x9e00020089b67f7f   padd1 R = r2, r3
// EXPECT r9 = 0x600200028d467f7f   psub1 R = r2, r3
// EXPECT r10 = 0x9e00020089b67f80   padd1.sss R = r2, r3
// EXPECT r11 = 0x600200028d467f7f   psub1.sss R = r2, r3
// EXPECT r12 = 0x9e00020089b67f7f   padd1.uus R = r2, r3
// EXPECT r13 = 0xff0200028dff7fff   psub1.uus R = r2, r3
// EXPECT r14 = 0xffff02ffffff7fff   padd1.uuu R = r2, r3
// EXPECT r15 = 0x6000000000467f7f   psub1.uuu R = r2, r3
// EXPECT r16 = 0xcf800180c5db3fbf   pavg1 R = r2, r3
// EXPECT r17 = 0xcf800180c5db40c0   pavg1.raz R = r2, r3
// EXPECT r18 = 0x30810081c7233f3f   pavgsub1 R = r2, r3
// EXPECT r19 = 0x9f0003008ab6807f   padd2 R = r2, r3
// EXPECT r20 = 0x5f02ff028d467f7f   psub2 R = r2, r3
// EXPECT r21 = 0x9f0003008ab67fff   padd2.sss R = r2, r3
// EXPECT r22 = 0x5f02ff028d467f7f   psub2.sss R = r2, r3
// EXPECT r23 = 0x9f0003008ab6807f   padd2.uus R = r2, r3
// EXPECT r24 = 0xffff00008d467f7f   psub2.uus R = r2, r3
// EXPECT r25 = 0xffff0300ffff807f   padd2.uuu R = r2, r3
// EXPECT r26 = 0x5f02000000007f7f   psub2.uuu R = r2, r3
// EXPECT r27 = 0xcf800180c55b403f   pavg2 R = r2, r3
// EXPECT r28 = 0xcf800180c55b4040   pavg2.raz R = r2, r3
// EXPECT r29 = 0x2f81ff81c6a33fbf   pavgsub2 R = r2, r3
// EXPECT r30 = 0x9f0003008ab6807f   padd4 R = r2, r3
// EXPECT r31 = 0x5f01ff028d467f7f   psub4 R = r2, r3
// EXPECT r32 = 0xff0000000000   pcmp1.eq R = r2, r3
// EXPECT r33 = 0xffff00ff00ffffff   pcmp1.gt R = r2, r3
// EXPECT r34 = 0x0   pcmp2.eq R = r2, r3
// EXPECT r35 = 0xffff00000000ffff   pcmp2.gt R = r2, r3
// EXPECT r36 = 0x0   pcmp4.eq R = r2, r3
// EXPECT r37 = 0xffffffff00000000   pcmp4.gt R = r2, r3
// EXPECT r38 = 0xffff01fffefe7fff   pmax1.u R = r2, r3
// EXPECT r39 = 0x9f0101018bb80080   pmin1.u R = r2, r3
// EXPECT r40 = 0xff0101fffeb87fff   pmax2 R = r2, r3
// EXPECT r41 = 0x9fff01018bfe0080   pmin2 R = r2, r3
// EXPECT r42 = 0x200ff003fff80   pmpy2.r R = r2, r3
// EXPECT r43 = 0x5fa0ff0094a290   pmpy2.l R = r2, r3
// EXPECT r44 = 0xa0ff00ffa290ff80   pmpyshr2 R = r2, r3, 0
// EXPECT r45 = 0xa0ff00ffa290ff80   pmpyshr2.u R = r2, r3, 0
// EXPECT r46 = 0xbf41040129457fff   pmpyshr2 R = r2, r3, 7
// EXPECT r47 = 0xbf41040195457fff   pmpyshr2.u R = r2, r3, 7
// EXPECT r48 = 0xbf00040129007f   pmpyshr2 R = r2, r3, 15
// EXPECT r49 = 0x3ebf00041695007f   pmpyshr2.u R = r2, r3, 15
// EXPECT r50 = 0x5f00020094003f   pmpyshr2 R = r2, r3, 16
// EXPECT r51 = 0x9f5f00028b4a003f   pmpyshr2.u R = r2, r3, 16
// EXPECT r52 = 0xff0101018bfe7fff   pshl2 R = r2, 0
// EXPECT r53 = 0xff0101018bfe7fff   pshl4 R = r2, 0
// EXPECT r54 = 0x9fff01fffeb80080   pshr2 R = r3, 0
// EXPECT r55 = 0x9fff01fffeb80080   pshr2.u R = r3, 0
// EXPECT r56 = 0x9fff01fffeb80080   pshr4 R = r3, 0
// EXPECT r57 = 0x9fff01fffeb80080   pshr4.u R = r3, 0
// EXPECT r58 = 0xfe02020217fcfffe   pshl2 R = r2, 1
// EXPECT r59 = 0xfe02020217fcfffe   pshl4 R = r2, 1
// EXPECT r60 = 0xcfff00ffff5c0040   pshr2 R = r3, 1
// EXPECT r61 = 0x4fff00ff7f5c0040   pshr2.u R = r3, 1
// EXPECT r62 = 0xcfff80ffff5c0040   pshr4 R = r3, 1
// EXPECT r63 = 0x4fff80ff7f5c0040   pshr4.u R = r3, 1
// EXPECT r64 = 0x8000800000008000   pshl2 R = r2, 15
// EXPECT r65 = 0x808080003fff8000   pshl4 R = r2, 15
// EXPECT r66 = 0xffff0000ffff0000   pshr2 R = r3, 15
// EXPECT r67 = 0x1000000010000   pshr2.u R = r3, 15
// EXPECT r68 = 0xffff3ffefffffd70   pshr4 R = r3, 15
// EXPECT r69 = 0x13ffe0001fd70   pshr4.u R = r3, 15
// EXPECT r70 = 0x0   pshl2 R = r2, 16
// EXPECT r71 = 0x10100007fff0000   pshl4 R = r2, 16
// EXPECT r72 = 0xffff0000ffff0000   pshr2 R = r3, 16
// EXPECT r73 = 0x0   pshr2.u R = r3, 16
// EXPECT r74 = 0xffff9ffffffffeb8   pshr4 R = r3, 16
// EXPECT r75 = 0x9fff0000feb8   pshr4.u R = r3, 16
// EXPECT r76 = 0x0   pshl2 R = r2, 31
// EXPECT r77 = 0x8000000080000000   pshl4 R = r2, 31
// EXPECT r78 = 0xffff0000ffff0000   pshr2 R = r3, 31
// EXPECT r79 = 0x0   pshr2.u R = r3, 31
// EXPECT r80 = 0xffffffffffffffff   pshr4 R = r3, 31
// EXPECT r81 = 0x100000001   pshr4.u R = r3, 31
// EXPECT r82 = 0x0   pshl2 R = r2, r4
// EXPECT r83 = 0x0   pshl4 R = r2, r4
// EXPECT r84 = 0xffff0000ffff0000   pshr2 R = r3, r4
// EXPECT r85 = 0x0   pshr2.u R = r3, r4
// EXPECT r86 = 0xffffffffffffffff   pshr4 R = r3, r4
// EXPECT r87 = 0x0   pshr4.u R = r3, r4
// EXPECT r88 = 0x9e01040180007fff   pshladd2 R = r2, 1, r3
// EXPECT r89 = 0x9f7f027fc4b7407f   pshradd2 R = r2, 1, r3
// EXPECT r90 = 0x9c03060380007fff   pshladd2 R = r2, 2, r3
// EXPECT r91 = 0x9fbf023fe1b7207f   pshradd2 R = r2, 2, r3
// EXPECT r92 = 0x98070a0780007fff   pshladd2 R = r2, 3, r3
// EXPECT r93 = 0x9fdf021ff037107f   pshradd2 R = r2, 3, r3
// EXPECT r94 = 0x413   psad1 R = r2, r3
// EXPECT r95 = 0xff7ffe8b010101ff   mux1 R = r2, @rev
// EXPECT r96 = 0xff8b017f01fe01ff   mux1 R = r2, @mix
// EXPECT r97 = 0xff8b01fe017f01ff   mux1 R = r2, @shuf
// EXPECT r98 = 0xff018b7f0101feff   mux1 R = r2, @alt
// EXPECT r99 = 0xffffffffffffffff   mux1 R = r2, @brcst
// EXPECT r100 = 0x7fff8bfe0101ff01   mux2 R = r2, 0x1b
// EXPECT r101 = 0x1017fffff018bfe   mux2 R = r2, 0x8d
// EXPECT r102 = 0xff018bfe01017fff   mux2 R = r2, 0xd8
// EXPECT r103 = 0x101010101010101   mux2 R = r2, 0xaa
// EXPECT r104 = 0xff0101018bfe7fff   mux2 R = r2, 0xe4
// EXPECT r105 = 0xff9f01018bfe7f00   mix1.l R = r2, r3
// EXPECT r106 = 0x1ff01fffeb8ff80   mix1.r R = r2, r3
// EXPECT r107 = 0xff9f01ff010101ff   unpack1.h R = r2, r3
// EXPECT r108 = 0x8bfefeb87f00ff80   unpack1.l R = r2, r3
// EXPECT r109 = 0xff019fff8bfefeb8   mix2.l R = r2, r3
// EXPECT r110 = 0x10101ff7fff0080   mix2.r R = r2, r3
// EXPECT r111 = 0xff019fff010101ff   unpack2.h R = r2, r3
// EXPECT r112 = 0x8bfefeb87fff0080   unpack2.l R = r2, r3
// EXPECT r113 = 0xff0101019fff01ff   mix4.l R = r2, r3
// EXPECT r114 = 0x8bfe7ffffeb80080   mix4.r R = r2, r3
// EXPECT r115 = 0xff0101019fff01ff   unpack4.h R = r2, r3
// EXPECT r116 = 0x8bfe7ffffeb80080   unpack4.l R = r2, r3
// EXPECT r117 = 0x807f807f807f807f   pack2.sss R = r2, r3
.text
.global _start
_start:
  movl r2 = 0xff0101018bfe7fff
  movl r3 = 0x9fff01fffeb80080
  movl r4 = 0x3f
  movl r5 = 0xff00010000fe7f00
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
