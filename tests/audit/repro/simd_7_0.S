/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xfffefe00148102a4   padd1 R = r2, r3
// EXPECT r9 = 0x1020200127f00a6   psub1 R = r2, r3
// EXPECT r10 = 0xfffefe80148102a4   padd1.sss R = r2, r3
// EXPECT r11 = 0x80020200128000a6   psub1.sss R = r2, r3
// EXPECT r12 = 0xff000000148102a4   padd1.uus R = r2, r3
// EXPECT r13 = 0x10202ff127f00a6   psub1.uus R = r2, r3
// EXPECT r14 = 0xfffefeff148102ff   padd1.uuu R = r2, r3
// EXPECT r15 = 0x1000000127f0000   psub1.uuu R = r2, r3
// EXPECT r16 = 0x7f7f7f800a4101d2   pavg1 R = r2, r3
// EXPECT r17 = 0x807f7f800a4101d2   pavg1.raz R = r2, r3
// EXPECT r18 = 0x1818100093f00d3   pavgsub1 R = r2, r3
// EXPECT r19 = 0xfffeff00148103a4   padd2 R = r2, r3
// EXPECT r20 = 0x20200127fffa6   psub2 R = r2, r3
// EXPECT r21 = 0xfffeff00148103a4   padd2.sss R = r2, r3
// EXPECT r22 = 0x80000200127fffa6   psub2.sss R = r2, r3
// EXPECT r23 = 0xfffe0000148103a4   padd2.uus R = r2, r3
// EXPECT r24 = 0x20200127f0000   psub2.uus R = r2, r3
// EXPECT r25 = 0xfffeff00148103a4   padd2.uuu R = r2, r3
// EXPECT r26 = 0x20000127f0000   psub2.uuu R = r2, r3
// EXPECT r27 = 0x7fff7f800a4101d2   pavg2 R = r2, r3
// EXPECT r28 = 0x7fff7f800a4101d2   pavg2.raz R = r2, r3
// EXPECT r29 = 0x18100093fffd3   pavgsub2 R = r2, r3
// EXPECT r30 = 0xfffeff00148103a4   padd4 R = r2, r3
// EXPECT r31 = 0x10200127effa6   psub4 R = r2, r3
// EXPECT r32 = 0xff0000ff00   pcmp1.eq R = r2, r3
// EXPECT r33 = 0xffff00ff000000   pcmp1.gt R = r2, r3
// EXPECT r34 = 0x0   pcmp2.eq R = r2, r3
// EXPECT r35 = 0xffffffff0000   pcmp2.gt R = r2, r3
// EXPECT r36 = 0x0   pcmp4.eq R = r2, r3
// EXPECT r37 = 0xffffffff   pcmp4.gt R = r2, r3
// EXPECT r38 = 0x80fefe80138001ff   pmax1.u R = r2, r3
// EXPECT r39 = 0x7f000080010101a5   pmin1.u R = r2, r3
// EXPECT r40 = 0x7ffe0080138001ff   pmax2 R = r2, r3
// EXPECT r41 = 0x8000fe80010101a5   pmin2 R = r2, r3
// EXPECT r42 = 0xffff40000003485b   pmpy2.r R = r2, r3
// EXPECT r43 = 0xc001000000139380   pmpy2.l R = r2, r3
// EXPECT r44 = 0x40009380485b   pmpyshr2 R = r2, r3, 0
// EXPECT r45 = 0x40009380485b   pmpyshr2.u R = r2, r3, 0
// EXPECT r46 = 0x200fe8027270690   pmpyshr2 R = r2, r3, 7
// EXPECT r47 = 0xfe00fe8027270690   pmpyshr2.u R = r2, r3, 7
// EXPECT r48 = 0x8002fffe00270006   pmpyshr2 R = r2, r3, 15
// EXPECT r49 = 0x7ffe00fe00270006   pmpyshr2.u R = r2, r3, 15
// EXPECT r50 = 0xc001ffff00130003   pmpyshr2 R = r2, r3, 16
// EXPECT r51 = 0x3fff007f00130003   pmpyshr2.u R = r2, r3, 16
// EXPECT r52 = 0x80000080138001a5   pshl2 R = r2, 0
// EXPECT r53 = 0x80000080138001a5   pshl4 R = r2, 0
// EXPECT r54 = 0x7ffefe80010101ff   pshr2 R = r3, 0
// EXPECT r55 = 0x7ffefe80010101ff   pshr2.u R = r3, 0
// EXPECT r56 = 0x7ffefe80010101ff   pshr4 R = r3, 0
// EXPECT r57 = 0x7ffefe80010101ff   pshr4.u R = r3, 0
// EXPECT r58 = 0x1002700034a   pshl2 R = r2, 1
// EXPECT r59 = 0x1002700034a   pshl4 R = r2, 1
// EXPECT r60 = 0x3fffff40008000ff   pshr2 R = r3, 1
// EXPECT r61 = 0x3fff7f40008000ff   pshr2.u R = r3, 1
// EXPECT r62 = 0x3fff7f40008080ff   pshr4 R = r3, 1
// EXPECT r63 = 0x3fff7f40008080ff   pshr4.u R = r3, 1
// EXPECT r64 = 0x8000   pshl2 R = r2, 15
// EXPECT r65 = 0x40000000d28000   pshl4 R = r2, 15
// EXPECT r66 = 0xffff00000000   pshr2 R = r3, 15
// EXPECT r67 = 0x100000000   pshr2.u R = r3, 15
// EXPECT r68 = 0xfffd00000202   pshr4 R = r3, 15
// EXPECT r69 = 0xfffd00000202   pshr4.u R = r3, 15
// EXPECT r70 = 0x0   pshl2 R = r2, 16
// EXPECT r71 = 0x80000001a50000   pshl4 R = r2, 16
// EXPECT r72 = 0xffff00000000   pshr2 R = r3, 16
// EXPECT r73 = 0x0   pshr2.u R = r3, 16
// EXPECT r74 = 0x7ffe00000101   pshr4 R = r3, 16
// EXPECT r75 = 0x7ffe00000101   pshr4.u R = r3, 16
// EXPECT r76 = 0x0   pshl2 R = r2, 31
// EXPECT r77 = 0x80000000   pshl4 R = r2, 31
// EXPECT r78 = 0xffff00000000   pshr2 R = r3, 31
// EXPECT r79 = 0x0   pshr2.u R = r3, 31
// EXPECT r80 = 0x0   pshr4 R = r3, 31
// EXPECT r81 = 0x0   pshr4.u R = r3, 31
// EXPECT r82 = 0x1002700034a   pshl2 R = r2, r4
// EXPECT r83 = 0x1002700034a   pshl4 R = r2, r4
// EXPECT r84 = 0x3fffff40008000ff   pshr2 R = r3, r4
// EXPECT r85 = 0x3fff7f40008000ff   pshr2.u R = r3, r4
// EXPECT r86 = 0x3fff7f40008080ff   pshr4 R = r3, r4
// EXPECT r87 = 0x3fff7f40008080ff   pshr4.u R = r3, r4
// EXPECT r88 = 0x8000ff8028010549   pshladd2 R = r2, 1, r3
// EXPECT r89 = 0x3ffefec00ac102d1   pshradd2 R = r2, 1, r3
// EXPECT r90 = 0x800000804f010893   pshladd2 R = r2, 2, r3
// EXPECT r91 = 0x5ffefea005e10268   pshradd2 R = r2, 2, r3
// EXPECT r92 = 0x800002807fff0f27   pshladd2 R = r2, 3, r3
// EXPECT r93 = 0x6ffefe9003710233   pshradd2 R = r2, 3, r3
// EXPECT r94 = 0x2e8   psad1 R = r2, r3
// EXPECT r95 = 0xa501801380000080   mux1 R = r2, @rev
// EXPECT r96 = 0x80130001008080a5   mux1 R = r2, @mix
// EXPECT r97 = 0x80130080000180a5   mux1 R = r2, @shuf
// EXPECT r98 = 0x80001301008080a5   mux1 R = r2, @alt
// EXPECT r99 = 0xa5a5a5a5a5a5a5a5   mux1 R = r2, @brcst
// EXPECT r100 = 0x1a5138000808000   mux2 R = r2, 0x1b
// EXPECT r101 = 0x8001a580001380   mux2 R = r2, 0x8d
// EXPECT r102 = 0x80001380008001a5   mux2 R = r2, 0xd8
// EXPECT r103 = 0x80008000800080   mux2 R = r2, 0xaa
// EXPECT r104 = 0x80000080138001a5   mux2 R = r2, 0xe4
// EXPECT r105 = 0x807f00fe13010101   mix1.l R = r2, r3
// EXPECT r106 = 0xfe80808001a5ff   mix1.r R = r2, r3
// EXPECT r107 = 0x807f00fe00fe8080   unpack1.h R = r2, r3
// EXPECT r108 = 0x130180010101a5ff   unpack1.l R = r2, r3
// EXPECT r109 = 0x80007ffe13800101   mix2.l R = r2, r3
// EXPECT r110 = 0x80fe8001a501ff   mix2.r R = r2, r3
// EXPECT r111 = 0x80007ffe0080fe80   unpack2.h R = r2, r3
// EXPECT r112 = 0x1380010101a501ff   unpack2.l R = r2, r3
// EXPECT r113 = 0x800000807ffefe80   mix4.l R = r2, r3
// EXPECT r114 = 0x138001a5010101ff   mix4.r R = r2, r3
// EXPECT r115 = 0x800000807ffefe80   unpack4.h R = r2, r3
// EXPECT r116 = 0x138001a5010101ff   unpack4.l R = r2, r3
// EXPECT r117 = 0x7f807f7f807f7f7f   pack2.sss R = r2, r3
.text
.global _start
_start:
  movl r2 = 0x80000080138001a5
  movl r3 = 0x7ffefe80010101ff
  movl r4 = 0x1
  movl r5 = 0x8000000000800100
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
