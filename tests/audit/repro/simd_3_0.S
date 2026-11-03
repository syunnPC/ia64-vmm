/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x7efefe7f70807ff0   padd1 R = r2, r3
// EXPECT r9 = 0x800202817080810c   psub1 R = r2, r3
// EXPECT r10 = 0x7efefe8080807ff0   padd1.sss R = r2, r3
// EXPECT r11 = 0x800202817080810c   psub1.sss R = r2, r3
// EXPECT r12 = 0xff00007f70807ff0   padd1.uus R = r2, r3
// EXPECT r13 = 0x80020281ff8000ff   psub1.uus R = r2, r3
// EXPECT r14 = 0xfffefeffff807fff   padd1.uuu R = r2, r3
// EXPECT r15 = 0x800000007080000c   psub1.uuu R = r2, r3
// EXPECT r16 = 0xbf7f7fbfb8403ff8   pavg1 R = r2, r3
// EXPECT r17 = 0xbf7f7fc0b84040f8   pavg1.raz R = r2, r3
// EXPECT r18 = 0x408181c13840c106   pavgsub1 R = r2, r3
// EXPECT r19 = 0x7efeff7f708080f0   padd2 R = r2, r3
// EXPECT r20 = 0x7f0201817080810c   psub2 R = r2, r3
// EXPECT r21 = 0x7efeff7f80007fff   padd2.sss R = r2, r3
// EXPECT r22 = 0x800001817080810c   psub2.sss R = r2, r3
// EXPECT r23 = 0xffff0000708080f0   padd2.uus R = r2, r3
// EXPECT r24 = 0x7f020181ffff0000   psub2.uus R = r2, r3
// EXPECT r25 = 0xffffff7fffff80f0   padd2.uuu R = r2, r3
// EXPECT r26 = 0x7f02000070800000   psub2.uuu R = r2, r3
// EXPECT r27 = 0xbf7f7fbfb8404078   pavg2 R = r2, r3
// EXPECT r28 = 0xbf7f7fc0b8404078   pavg2.raz R = r2, r3
// EXPECT r29 = 0x3f8180c13840c086   pavgsub2 R = r2, r3
// EXPECT r30 = 0x7efeff7f708080f0   padd4 R = r2, r3
// EXPECT r31 = 0x7f010181707f810c   psub4 R = r2, r3
// EXPECT r32 = 0x0   pcmp1.eq R = r2, r3
// EXPECT r33 = 0xffff00ff0000ff   pcmp1.gt R = r2, r3
// EXPECT r34 = 0x0   pcmp2.eq R = r2, r3
// EXPECT r35 = 0xffffffff0000   pcmp2.gt R = r2, r3
// EXPECT r36 = 0x0   pcmp4.eq R = r2, r3
// EXPECT r37 = 0xffffffff   pcmp4.gt R = r2, r3
// EXPECT r38 = 0xfffefefff0807ffe   pmax1.u R = r2, r3
// EXPECT r39 = 0x7f000080800000f2   pmin1.u R = r2, r3
// EXPECT r40 = 0x7ffe0080f0807ff2   pmax2 R = r2, r3
// EXPECT r41 = 0xff00feff800000fe   pmin2 R = r2, r3
// EXPECT r42 = 0xffff7f80007ef21c   pmpy2.r R = r2, r3
// EXPECT r43 = 0xff80020007c00000   pmpy2.l R = r2, r3
// EXPECT r44 = 0x2007f800000f21c   pmpyshr2 R = r2, r3, 0
// EXPECT r45 = 0x2007f800000f21c   pmpyshr2.u R = r2, r3, 0
// EXPECT r46 = 0x4feff8000fde4   pmpyshr2 R = r2, r3, 7
// EXPECT r47 = 0xfc04feff8000fde4   pmpyshr2.u R = r2, r3, 7
// EXPECT r48 = 0xff00fffe0f8000fd   pmpyshr2 R = r2, r3, 15
// EXPECT r49 = 0xfefc00fef08000fd   pmpyshr2.u R = r2, r3, 15
// EXPECT r50 = 0xff80ffff07c0007e   pmpyshr2 R = r2, r3, 16
// EXPECT r51 = 0x7f7e007f7840007e   pmpyshr2.u R = r2, r3, 16
// EXPECT r52 = 0xff000080f08000fe   pshl2 R = r2, 0
// EXPECT r53 = 0xff000080f08000fe   pshl4 R = r2, 0
// EXPECT r54 = 0x7ffefeff80007ff2   pshr2 R = r3, 0
// EXPECT r55 = 0x7ffefeff80007ff2   pshr2.u R = r3, 0
// EXPECT r56 = 0x7ffefeff80007ff2   pshr4 R = r3, 0
// EXPECT r57 = 0x7ffefeff80007ff2   pshr4.u R = r3, 0
// EXPECT r58 = 0xfe000100e10001fc   pshl2 R = r2, 1
// EXPECT r59 = 0xfe000100e10001fc   pshl4 R = r2, 1
// EXPECT r60 = 0x3fffff7fc0003ff9   pshr2 R = r3, 1
// EXPECT r61 = 0x3fff7f7f40003ff9   pshr2.u R = r3, 1
// EXPECT r62 = 0x3fff7f7fc0003ff9   pshr4 R = r3, 1
// EXPECT r63 = 0x3fff7f7f40003ff9   pshr4.u R = r3, 1
// EXPECT r64 = 0x0   pshl2 R = r2, 15
// EXPECT r65 = 0x400000007f0000   pshl4 R = r2, 15
// EXPECT r66 = 0xffffffff0000   pshr2 R = r3, 15
// EXPECT r67 = 0x100010000   pshr2.u R = r3, 15
// EXPECT r68 = 0xfffdffff0000   pshr4 R = r3, 15
// EXPECT r69 = 0xfffd00010000   pshr4.u R = r3, 15
// EXPECT r70 = 0x0   pshl2 R = r2, 16
// EXPECT r71 = 0x80000000fe0000   pshl4 R = r2, 16
// EXPECT r72 = 0xffffffff0000   pshr2 R = r3, 16
// EXPECT r73 = 0x0   pshr2.u R = r3, 16
// EXPECT r74 = 0x7ffeffff8000   pshr4 R = r3, 16
// EXPECT r75 = 0x7ffe00008000   pshr4.u R = r3, 16
// EXPECT r76 = 0x0   pshl2 R = r2, 31
// EXPECT r77 = 0x0   pshl4 R = r2, 31
// EXPECT r78 = 0xffffffff0000   pshr2 R = r3, 31
// EXPECT r79 = 0x0   pshr2.u R = r3, 31
// EXPECT r80 = 0xffffffff   pshr4 R = r3, 31
// EXPECT r81 = 0x1   pshr4.u R = r3, 31
// EXPECT r82 = 0x0   pshl2 R = r2, r4
// EXPECT r83 = 0x0   pshl4 R = r2, r4
// EXPECT r84 = 0xffffffff0000   pshr2 R = r3, r4
// EXPECT r85 = 0x0   pshr2.u R = r3, r4
// EXPECT r86 = 0xffffffff   pshr4 R = r3, r4
// EXPECT r87 = 0x0   pshr4.u R = r3, r4
// EXPECT r88 = 0x7dfeffff80007fff   pshladd2 R = r2, 1, r3
// EXPECT r89 = 0x7f7eff3f80007fff   pshradd2 R = r2, 1, r3
// EXPECT r90 = 0x7bfe00ff80007fff   pshladd2 R = r2, 2, r3
// EXPECT r91 = 0x7fbeff1f80007fff   pshradd2 R = r2, 2, r3
// EXPECT r92 = 0x77fe02ff80007fff   pshladd2 R = r2, 3, r3
// EXPECT r93 = 0x7fdeff0f80007fff   pshradd2 R = r2, 3, r3
// EXPECT r94 = 0x476   psad1 R = r2, r3
// EXPECT r95 = 0xfe0080f0800000ff   mux1 R = r2, @rev
// EXPECT r96 = 0xfff00000008080fe   mux1 R = r2, @mix
// EXPECT r97 = 0xfff00080000080fe   mux1 R = r2, @shuf
// EXPECT r98 = 0xff00f000008080fe   mux1 R = r2, @alt
// EXPECT r99 = 0xfefefefefefefefe   mux1 R = r2, @brcst
// EXPECT r100 = 0xfef0800080ff00   mux2 R = r2, 0x1b
// EXPECT r101 = 0x8000feff00f080   mux2 R = r2, 0x8d
// EXPECT r102 = 0xff00f080008000fe   mux2 R = r2, 0xd8
// EXPECT r103 = 0x80008000800080   mux2 R = r2, 0xaa
// EXPECT r104 = 0xff000080f08000fe   mux2 R = r2, 0xe4
// EXPECT r105 = 0xff7f00fef080007f   mix1.l R = r2, r3
// EXPECT r106 = 0xfe80ff8000fef2   mix1.r R = r2, r3
// EXPECT r107 = 0xff7f00fe00fe80ff   unpack1.h R = r2, r3
// EXPECT r108 = 0xf0808000007ffef2   unpack1.l R = r2, r3
// EXPECT r109 = 0xff007ffef0808000   mix2.l R = r2, r3
// EXPECT r110 = 0x80feff00fe7ff2   mix2.r R = r2, r3
// EXPECT r111 = 0xff007ffe0080feff   unpack2.h R = r2, r3
// EXPECT r112 = 0xf080800000fe7ff2   unpack2.l R = r2, r3
// EXPECT r113 = 0xff0000807ffefeff   mix4.l R = r2, r3
// EXPECT r114 = 0xf08000fe80007ff2   mix4.r R = r2, r3
// EXPECT r115 = 0xff0000807ffefeff   unpack4.h R = r2, r3
// EXPECT r116 = 0xf08000fe80007ff2   unpack4.l R = r2, r3
// EXPECT r117 = 0x7f80807f807f807f   pack2.sss R = r2, r3
.text
.global _start
_start:
  movl r2 = 0xff000080f08000fe
  movl r3 = 0x7ffefeff80007ff2
  movl r4 = 0x21
  movl r5 = 0xff00000000800000
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
