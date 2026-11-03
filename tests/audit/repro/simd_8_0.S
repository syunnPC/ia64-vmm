/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xe8ad8125fea00144   padd1 R = r2, r3
// EXPECT r9 = 0x16ad7fcd0260ffa4   psub1 R = r2, r3
// EXPECT r10 = 0xe8ad8125fea00144   padd1.sss R = r2, r3
// EXPECT r11 = 0x167f80cd0280ff7f   psub1.sss R = r2, r3
// EXPECT r12 = 0xe80081ff00a00144   padd1.uus R = r2, r3
// EXPECT r13 = 0xffad7fcd026000a4   psub1.uus R = r2, r3
// EXPECT r14 = 0xffad81fffea001ff   padd1.uuu R = r2, r3
// EXPECT r15 = 0x16007fcd00600000   psub1.uuu R = r2, r3
// EXPECT r16 = 0xf45741937f5001a2   pavg1 R = r2, r3
// EXPECT r17 = 0xf45741937f5001a2   pavg1.raz R = r2, r3
// EXPECT r18 = 0xbd73f678130ffd2   pavgsub1 R = r2, r3
// EXPECT r19 = 0xe8ad8225fea00244   padd2 R = r2, r3
// EXPECT r20 = 0x15ad7fcd0260fea4   psub2 R = r2, r3
// EXPECT r21 = 0xe8ad8225fea00244   padd2.sss R = r2, r3
// EXPECT r22 = 0x15ad80000260fea4   psub2.sss R = r2, r3
// EXPECT r23 = 0xe8ad822500000244   padd2.uus R = r2, r3
// EXPECT r24 = 0xffff7fcd02600000   psub2.uus R = r2, r3
// EXPECT r25 = 0xffff8225fea00244   padd2.uuu R = r2, r3
// EXPECT r26 = 0x15ad7fcd00000000   psub2.uuu R = r2, r3
// EXPECT r27 = 0xf45741137f500122   pavg2 R = r2, r3
// EXPECT r28 = 0xf45741137f500122   pavg2.raz R = r2, r3
// EXPECT r29 = 0xad73fe78130ff52   pavgsub2 R = r2, r3
// EXPECT r30 = 0xe8ad8225fea00244   padd4 R = r2, r3
// EXPECT r31 = 0x15ad7fcd025ffea4   psub4 R = r2, r3
// EXPECT r32 = 0x0   pcmp1.eq R = r2, r3
// EXPECT r33 = 0xffff0000ff0000ff   pcmp1.gt R = r2, r3
// EXPECT r34 = 0x0   pcmp2.eq R = r2, r3
// EXPECT r35 = 0xffff0000ffff0000   pcmp2.gt R = r2, r3
// EXPECT r36 = 0x0   pcmp4.eq R = r2, r3
// EXPECT r37 = 0xffffffffffffffff   pcmp4.gt R = r2, r3
// EXPECT r38 = 0xff8080f9fe8001d0   pmax1.u R = r2, r3
// EXPECT r39 = 0xe92d012c00200074   pmin1.u R = r2, r3
// EXPECT r40 = 0xff2d012c008001d0   pmax2 R = r2, r3
// EXPECT r41 = 0xe98080f9fe200074   pmin2 R = r2, r3
// EXPECT r42 = 0xff6b23cc0000d240   pmpy2.r R = r2, r3
// EXPECT r43 = 0x128b80ffff1000   pmpy2.l R = r2, r3
// EXPECT r44 = 0x8b8023cc1000d240   pmpyshr2 R = r2, r3, 0
// EXPECT r45 = 0x8b8023cc1000d240   pmpyshr2.u R = r2, r3, 0
// EXPECT r46 = 0x2517d647fe2001a4   pmpyshr2 R = r2, r3, 7
// EXPECT r47 = 0x7f172e47fe2001a4   pmpyshr2.u R = r2, r3, 7
// EXPECT r48 = 0x25fed6fffe0001   pmpyshr2 R = r2, r3, 15
// EXPECT r49 = 0xd17f012e00fe0001   pmpyshr2.u R = r2, r3, 15
// EXPECT r50 = 0x12ff6bffff0000   pmpyshr2 R = r2, r3, 16
// EXPECT r51 = 0xe8bf0097007f0000   pmpyshr2.u R = r2, r3, 16
// EXPECT r52 = 0xff2d80f900800074   pshl2 R = r2, 0
// EXPECT r53 = 0xff2d80f900800074   pshl4 R = r2, 0
// EXPECT r54 = 0xe980012cfe2001d0   pshr2 R = r3, 0
// EXPECT r55 = 0xe980012cfe2001d0   pshr2.u R = r3, 0
// EXPECT r56 = 0xe980012cfe2001d0   pshr4 R = r3, 0
// EXPECT r57 = 0xe980012cfe2001d0   pshr4.u R = r3, 0
// EXPECT r58 = 0xfe5a01f2010000e8   pshl2 R = r2, 1
// EXPECT r59 = 0xfe5b01f2010000e8   pshl4 R = r2, 1
// EXPECT r60 = 0xf4c00096ff1000e8   pshr2 R = r3, 1
// EXPECT r61 = 0x74c000967f1000e8   pshr2.u R = r3, 1
// EXPECT r62 = 0xf4c00096ff1000e8   pshr4 R = r3, 1
// EXPECT r63 = 0x74c000967f1000e8   pshr4.u R = r3, 1
// EXPECT r64 = 0x8000800000000000   pshl2 R = r2, 15
// EXPECT r65 = 0xc07c8000003a0000   pshl4 R = r2, 15
// EXPECT r66 = 0xffff0000ffff0000   pshr2 R = r3, 15
// EXPECT r67 = 0x1000000010000   pshr2.u R = r3, 15
// EXPECT r68 = 0xffffd300fffffc40   pshr4 R = r3, 15
// EXPECT r69 = 0x1d3000001fc40   pshr4.u R = r3, 15
// EXPECT r70 = 0x0   pshl2 R = r2, 16
// EXPECT r71 = 0x80f9000000740000   pshl4 R = r2, 16
// EXPECT r72 = 0xffff0000ffff0000   pshr2 R = r3, 16
// EXPECT r73 = 0x0   pshr2.u R = r3, 16
// EXPECT r74 = 0xffffe980fffffe20   pshr4 R = r3, 16
// EXPECT r75 = 0xe9800000fe20   pshr4.u R = r3, 16
// EXPECT r76 = 0x0   pshl2 R = r2, 31
// EXPECT r77 = 0x8000000000000000   pshl4 R = r2, 31
// EXPECT r78 = 0xffff0000ffff0000   pshr2 R = r3, 31
// EXPECT r79 = 0x0   pshr2.u R = r3, 31
// EXPECT r80 = 0xffffffffffffffff   pshr4 R = r3, 31
// EXPECT r81 = 0x100000001   pshr4.u R = r3, 31
// EXPECT r82 = 0x0   pshl2 R = r2, r4
// EXPECT r83 = 0x8000000000000000   pshl4 R = r2, r4
// EXPECT r84 = 0xffff0000ffff0000   pshr2 R = r3, r4
// EXPECT r85 = 0x0   pshr2.u R = r3, r4
// EXPECT r86 = 0xffffffffffffffff   pshr4 R = r3, r4
// EXPECT r87 = 0x100000001   pshr4.u R = r3, r4
// EXPECT r88 = 0xe7da8000ff2002b8   pshladd2 R = r2, 1, r3
// EXPECT r89 = 0xe916c1a8fe60020a   pshradd2 R = r2, 1, r3
// EXPECT r90 = 0xe6348000002003a0   pshladd2 R = r2, 2, r3
// EXPECT r91 = 0xe94be16afe4001ed   pshradd2 R = r2, 2, r3
// EXPECT r92 = 0xe2e8800002200570   pshladd2 R = r2, 3, r3
// EXPECT r93 = 0xe965f14bfe3001de   pshradd2 R = r2, 3, r3
// EXPECT r94 = 0x370   psad1 R = r2, r3
// EXPECT r95 = 0x74008000f9802dff   mux1 R = r2, @rev
// EXPECT r96 = 0xff0080002d80f974   mux1 R = r2, @mix
// EXPECT r97 = 0xff002d808000f974   mux1 R = r2, @shuf
// EXPECT r98 = 0xff8000002df98074   mux1 R = r2, @alt
// EXPECT r99 = 0x7474747474747474   mux1 R = r2, @brcst
// EXPECT r100 = 0x74008080f9ff2d   mux2 R = r2, 0x1b
// EXPECT r101 = 0x80f90074ff2d0080   mux2 R = r2, 0x8d
// EXPECT r102 = 0xff2d008080f90074   mux2 R = r2, 0xd8
// EXPECT r103 = 0x80f980f980f980f9   mux2 R = r2, 0xaa
// EXPECT r104 = 0xff2d80f900800074   mux2 R = r2, 0xe4
// EXPECT r105 = 0xffe9800100fe0001   mix1.l R = r2, r3
// EXPECT r106 = 0x2d80f92c802074d0   mix1.r R = r2, r3
// EXPECT r107 = 0xffe92d808001f92c   unpack1.h R = r2, r3
// EXPECT r108 = 0xfe8020000174d0   unpack1.l R = r2, r3
// EXPECT r109 = 0xff2de9800080fe20   mix2.l R = r2, r3
// EXPECT r110 = 0x80f9012c007401d0   mix2.r R = r2, r3
// EXPECT r111 = 0xff2de98080f9012c   unpack2.h R = r2, r3
// EXPECT r112 = 0x80fe20007401d0   unpack2.l R = r2, r3
// EXPECT r113 = 0xff2d80f9e980012c   mix4.l R = r2, r3
// EXPECT r114 = 0x800074fe2001d0   mix4.r R = r2, r3
// EXPECT r115 = 0xff2d80f9e980012c   unpack4.h R = r2, r3
// EXPECT r116 = 0x800074fe2001d0   unpack4.l R = r2, r3
// EXPECT r117 = 0x807f807f80807f74   pack2.sss R = r2, r3
.text
.global _start
_start:
  movl r2 = 0xff2d80f900800074
  movl r3 = 0xe980012cfe2001d0
  movl r4 = 0x1f
  movl r5 = 0xff00800000800000
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
