/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x45fefd5d70fd8000   padd1 R = r2, r3
// EXPECT r9 = 0x490001a370ff8000   psub1 R = r2, r3
// EXPECT r10 = 0x457ffd5d80fd8000   padd1.sss R = r2, r3
// EXPECT r11 = 0x490001a370ff7f00   psub1.sss R = r2, r3
// EXPECT r12 = 0x45fefd5d70fd0000   padd1.uus R = r2, r3
// EXPECT r13 = 0x4900ff00ffff8000   psub1.uus R = r2, r3
// EXPECT r14 = 0xfffeff5dffff8000   padd1.uuu R = r2, r3
// EXPECT r15 = 0x10070000000   psub1.uuu R = r2, r3
// EXPECT r16 = 0xa37fff2fb8ff4000   pavg1 R = r2, r3
// EXPECT r17 = 0xa37fff2fb8ff4000   pavg1.raz R = r2, r3
// EXPECT r18 = 0xa50001d138ffc000   pavgsub1 R = r2, r3
// EXPECT r19 = 0x45fefd5d71fd8000   padd2 R = r2, r3
// EXPECT r20 = 0x490000a36fff8000   psub2 R = r2, r3
// EXPECT r21 = 0x45fefd5d80008000   padd2.sss R = r2, r3
// EXPECT r22 = 0x490000a36fff7fff   psub2.sss R = r2, r3
// EXPECT r23 = 0x45fefd5d71fd0000   padd2.uus R = r2, r3
// EXPECT r24 = 0x4900ffffffff8000   psub2.uus R = r2, r3
// EXPECT r25 = 0xffffffffffff8000   padd2.uuu R = r2, r3
// EXPECT r26 = 0xa36fff0000   psub2.uuu R = r2, r3
// EXPECT r27 = 0xa2fffeafb8ff4000   pavg2 R = r2, r3
// EXPECT r28 = 0xa2fffeafb8ff4000   pavg2.raz R = r2, r3
// EXPECT r29 = 0xa480005137ffc000   pavgsub2 R = r2, r3
// EXPECT r30 = 0x45fffd5d71fd8000   padd4 R = r2, r3
// EXPECT r31 = 0x490000a36ffe8000   psub4 R = r2, r3
// EXPECT r32 = 0xff0000000000ff   pcmp1.eq R = r2, r3
// EXPECT r33 = 0xff00ff00ff00ff00   pcmp1.gt R = r2, r3
// EXPECT r34 = 0x0   pcmp2.eq R = r2, r3
// EXPECT r35 = 0xffffffffffffffff   pcmp2.gt R = r2, r3
// EXPECT r36 = 0x0   pcmp4.eq R = r2, r3
// EXPECT r37 = 0xffffffffffffffff   pcmp4.gt R = r2, r3
// EXPECT r38 = 0xfe7fff5df0ff8000   pmax1.u R = r2, r3
// EXPECT r39 = 0x477ffe0080fe0000   pmin1.u R = r2, r3
// EXPECT r40 = 0x477fff00f0fe0000   pmax2 R = r2, r3
// EXPECT r41 = 0xfe7ffe5d80ff8000   pmin2 R = r2, r3
// EXPECT r42 = 0x1a30000000000   pmpy2.r R = r2, r3
// EXPECT r43 = 0xff947a0107720d02   pmpy2.l R = r2, r3
// EXPECT r44 = 0x7a01a3000d020000   pmpyshr2 R = r2, r3, 0
// EXPECT r45 = 0x7a01a3000d020000   pmpyshr2.u R = r2, r3, 0
// EXPECT r46 = 0x28f40346e41a0000   pmpyshr2 R = r2, r3, 7
// EXPECT r47 = 0x26f4bd46de1a0000   pmpyshr2.u R = r2, r3, 7
// EXPECT r48 = 0xff2800030ee40000   pmpyshr2 R = r2, r3, 15
// EXPECT r49 = 0x8e26fabdf2de0000   pmpyshr2.u R = r2, r3, 15
// EXPECT r50 = 0xff94000107720000   pmpyshr2 R = r2, r3, 16
// EXPECT r51 = 0x4713fd5e796f0000   pmpyshr2.u R = r2, r3, 16
// EXPECT r52 = 0x477fff00f0fe0000   pshl2 R = r2, 0
// EXPECT r53 = 0x477fff00f0fe0000   pshl4 R = r2, 0
// EXPECT r54 = 0xfe7ffe5d80ff8000   pshr2 R = r3, 0
// EXPECT r55 = 0xfe7ffe5d80ff8000   pshr2.u R = r3, 0
// EXPECT r56 = 0xfe7ffe5d80ff8000   pshr4 R = r3, 0
// EXPECT r57 = 0xfe7ffe5d80ff8000   pshr4.u R = r3, 0
// EXPECT r58 = 0x8efefe00e1fc0000   pshl2 R = r2, 1
// EXPECT r59 = 0x8efffe00e1fc0000   pshl4 R = r2, 1
// EXPECT r60 = 0xff3fff2ec07fc000   pshr2 R = r3, 1
// EXPECT r61 = 0x7f3f7f2e407f4000   pshr2.u R = r3, 1
// EXPECT r62 = 0xff3fff2ec07fc000   pshr4 R = r3, 1
// EXPECT r63 = 0x7f3fff2e407fc000   pshr4.u R = r3, 1
// EXPECT r64 = 0x8000000000000000   pshl2 R = r2, 15
// EXPECT r65 = 0xff80000000000000   pshl4 R = r2, 15
// EXPECT r66 = 0xffffffffffffffff   pshr2 R = r3, 15
// EXPECT r67 = 0x1000100010001   pshr2.u R = r3, 15
// EXPECT r68 = 0xfffffcffffff01ff   pshr4 R = r3, 15
// EXPECT r69 = 0x1fcff000101ff   pshr4.u R = r3, 15
// EXPECT r70 = 0x0   pshl2 R = r2, 16
// EXPECT r71 = 0xff00000000000000   pshl4 R = r2, 16
// EXPECT r72 = 0xffffffffffffffff   pshr2 R = r3, 16
// EXPECT r73 = 0x0   pshr2.u R = r3, 16
// EXPECT r74 = 0xfffffe7fffff80ff   pshr4 R = r3, 16
// EXPECT r75 = 0xfe7f000080ff   pshr4.u R = r3, 16
// EXPECT r76 = 0x0   pshl2 R = r2, 31
// EXPECT r77 = 0x0   pshl4 R = r2, 31
// EXPECT r78 = 0xffffffffffffffff   pshr2 R = r3, 31
// EXPECT r79 = 0x0   pshr2.u R = r3, 31
// EXPECT r80 = 0xffffffffffffffff   pshr4 R = r3, 31
// EXPECT r81 = 0x100000001   pshr4.u R = r3, 31
// EXPECT r82 = 0x8efefe00e1fc0000   pshl2 R = r2, r4
// EXPECT r83 = 0x8efffe00e1fc0000   pshl4 R = r2, r4
// EXPECT r84 = 0xff3fff2ec07fc000   pshr2 R = r3, r4
// EXPECT r85 = 0x7f3f7f2e407f4000   pshr2.u R = r3, r4
// EXPECT r86 = 0xff3fff2ec07fc000   pshr4 R = r3, r4
// EXPECT r87 = 0x7f3fff2e407fc000   pshr4.u R = r3, r4
// EXPECT r88 = 0x7ffffc5d80008000   pshladd2 R = r2, 1, r3
// EXPECT r89 = 0x223efddd80008000   pshradd2 R = r2, 1, r3
// EXPECT r90 = 0x7ffffa5d80008000   pshladd2 R = r2, 2, r3
// EXPECT r91 = 0x105efe1d80008000   pshradd2 R = r2, 2, r3
// EXPECT r92 = 0x7ffff65d80008000   pshladd2 R = r2, 3, r3
// EXPECT r93 = 0x76efe3d80008000   pshradd2 R = r2, 3, r3
// EXPECT r94 = 0x206   psad1 R = r2, r3
// EXPECT r95 = 0xfef000ff7f47   mux1 R = r2, @rev
// EXPECT r96 = 0x47f0ff007ffe0000   mux1 R = r2, @mix
// EXPECT r97 = 0x47f07ffeff000000   mux1 R = r2, @shuf
// EXPECT r98 = 0x47fff0007f00fe00   mux1 R = r2, @alt
// EXPECT r99 = 0x0   mux1 R = r2, @brcst
// EXPECT r100 = 0xf0feff00477f   mux2 R = r2, 0x1b
// EXPECT r101 = 0xff000000477ff0fe   mux2 R = r2, 0x8d
// EXPECT r102 = 0x477ff0feff000000   mux2 R = r2, 0xd8
// EXPECT r103 = 0xff00ff00ff00ff00   mux2 R = r2, 0xaa
// EXPECT r104 = 0x477fff00f0fe0000   mux2 R = r2, 0xe4
// EXPECT r105 = 0x47fefffef0800080   mix1.l R = r2, r3
// EXPECT r106 = 0x7f7f005dfeff0000   mix1.r R = r2, r3
// EXPECT r107 = 0x47fe7f7ffffe005d   unpack1.h R = r2, r3
// EXPECT r108 = 0xf080feff00800000   unpack1.l R = r2, r3
// EXPECT r109 = 0x477ffe7ff0fe80ff   mix2.l R = r2, r3
// EXPECT r110 = 0xff00fe5d00008000   mix2.r R = r2, r3
// EXPECT r111 = 0x477ffe7fff00fe5d   unpack2.h R = r2, r3
// EXPECT r112 = 0xf0fe80ff00008000   unpack2.l R = r2, r3
// EXPECT r113 = 0x477fff00fe7ffe5d   mix4.l R = r2, r3
// EXPECT r114 = 0xf0fe000080ff8000   mix4.r R = r2, r3
// EXPECT r115 = 0x477fff00fe7ffe5d   unpack4.h R = r2, r3
// EXPECT r116 = 0xf0fe000080ff8000   unpack4.l R = r2, r3
// EXPECT r117 = 0x808080807f808000   pack2.sss R = r2, r3
.text
.global _start
_start:
  movl r2 = 0x477fff00f0fe0000
  movl r3 = 0xfe7ffe5d80ff8000
  movl r4 = 0x1
  movl r5 = 0x4700ff0000fe0000
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
