/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT nat41 = 1   add R = N, G
// EXPECT nat42 = 1   add R = G, N
// EXPECT nat43 = 0   add R = G, G
// EXPECT nat44 = 1   adds R = 5, N
// EXPECT nat45 = 1   sub R = 5, N
// EXPECT nat46 = 1   and R = 5, N
// EXPECT nat47 = 1   andcm R = N, G
// EXPECT nat48 = 1   shladd R = N, 2, G
// EXPECT nat49 = 1   shladd R = G, 2, N
// EXPECT nat50 = 1   addp4 R = N, G
// EXPECT nat51 = 1   addp4 R = 5, N
// EXPECT nat52 = 1   shladdp4 R = N, 1, G
// EXPECT nat53 = 1   shl R = G, N
// EXPECT nat54 = 1   shl R = N, G
// EXPECT nat55 = 1   shr R = N, G
// EXPECT nat56 = 1   shr R = G, N
// EXPECT nat57 = 1   shr.u R = G, N
// EXPECT nat58 = 1   shl R = N, 3
// EXPECT nat59 = 1   shr R = N, 3
// EXPECT nat60 = 1   shr.u R = N, 3
// EXPECT nat61 = 1   shrp R = N, G, 3
// EXPECT nat62 = 1   shrp R = G, N, 3
// EXPECT nat63 = 1   dep.z R = N, 3, 4
// EXPECT nat64 = 0   dep.z R = 5, 3, 4
// EXPECT nat65 = 1   dep R = N, G, 3, 4
// EXPECT nat66 = 1   dep R = G, N, 3, 4
// EXPECT nat67 = 1   dep R = -1, N, 3, 4
// EXPECT nat68 = 1   extr R = N, 3, 4
// EXPECT nat69 = 1   extr.u R = N, 3, 4
// EXPECT nat70 = 1   sxt1 R = N
// EXPECT nat71 = 1   zxt4 R = N
// EXPECT nat72 = 1   popcnt R = N
// EXPECT nat73 = 0   movl R = 5
// EXPECT nat74 = 0   mov R = ar.lc
// EXPECT nat75 = 0   mov R = pr
// EXPECT nat76 = 1   padd1 R = N, G
// EXPECT nat77 = 1   psub2.sss R = G, N
// EXPECT nat78 = 1   pavg1 R = N, G
// EXPECT nat79 = 1   pcmp1.eq R = G, N
// EXPECT nat80 = 1   pmpyshr2 R = N, G, 7
// EXPECT nat81 = 1   pshl2 R = N, 3
// EXPECT nat82 = 1   pshl2 R = G, N
// EXPECT nat83 = 1   pshr2 R = N, 3
// EXPECT nat84 = 1   pshr2 R = G, N
// EXPECT nat85 = 1   pshr4.u R = N, G
// EXPECT nat86 = 1   mux1 R = N, @rev
// EXPECT nat87 = 1   mux2 R = N, 0x1b
// EXPECT nat88 = 1   mix1.l R = G, N
// EXPECT nat89 = 1   pack2.sss R = N, G
// EXPECT nat90 = 1   unpack2.h R = G, N
// EXPECT nat91 = 1   czx1.l R = N
// EXPECT nat92 = 1   psad1 R = G, N
// EXPECT nat93 = 1   pshladd2 R = N, 1, G
// EXPECT nat94 = 1   pshradd2 R = G, 1, N
// EXPECT nat95 = 1   pmin2 R = N, G
// EXPECT nat96 = 1   pmpy2.r R = G, N
// EXPECT nat97 = 0   mov R = b0
// EXPECT nat98 = 0   mov R = ip
.text
.global _start
_start:
  movl r2 = 0x108000
  mov r3 = 1
  mov r9 = 7
  ;;
  mov ar.unat = r3
  ;;
  ld8.fill r40 = [r2]
  ;;
  add r41 = r40, r9
  add r42 = r9, r40
  add r43 = r9, r9
  adds r44 = 5, r40
  sub r45 = 5, r40
  and r46 = 5, r40
  andcm r47 = r40, r9
  shladd r48 = r40, 2, r9
  shladd r49 = r9, 2, r40
  addp4 r50 = r40, r9
  addp4 r51 = 5, r40
  shladdp4 r52 = r40, 1, r9
  shl r53 = r9, r40
  shl r54 = r40, r9
  shr r55 = r40, r9
  shr r56 = r9, r40
  shr.u r57 = r9, r40
  shl r58 = r40, 3
  shr r59 = r40, 3
  shr.u r60 = r40, 3
  shrp r61 = r40, r9, 3
  shrp r62 = r9, r40, 3
  dep.z r63 = r40, 3, 4
  dep.z r64 = 5, 3, 4
  dep r65 = r40, r9, 3, 4
  dep r66 = r9, r40, 3, 4
  dep r67 = -1, r40, 3, 4
  extr r68 = r40, 3, 4
  extr.u r69 = r40, 3, 4
  sxt1 r70 = r40
  zxt4 r71 = r40
  popcnt r72 = r40
  movl r73 = 5
  mov r74 = ar.lc
  mov r75 = pr
  padd1 r76 = r40, r9
  psub2.sss r77 = r9, r40
  pavg1 r78 = r40, r9
  pcmp1.eq r79 = r9, r40
  pmpyshr2 r80 = r40, r9, 7
  pshl2 r81 = r40, 3
  pshl2 r82 = r9, r40
  pshr2 r83 = r40, 3
  pshr2 r84 = r9, r40
  pshr4.u r85 = r40, r9
  mux1 r86 = r40, @rev
  mux2 r87 = r40, 0x1b
  mix1.l r88 = r9, r40
  pack2.sss r89 = r40, r9
  unpack2.h r90 = r9, r40
  czx1.l r91 = r40
  psad1 r92 = r9, r40
  pshladd2 r93 = r40, 1, r9
  pshradd2 r94 = r9, 1, r40
  pmin2 r95 = r40, r9
  pmpy2.r r96 = r9, r40
  mov r97 = b0
  mov r98 = ip
  ;;
  break.i 0x1ffffe
