/* SPDX-License-Identifier: GPL-2.0-or-later */
// st8.spill/ld8.fill at addr 0x108038 -> bit 7
// EXPECT nat41 = 1
// EXPECT r41 = 0
// EXPECT ar36 = 0x80
// EXPECT r42 = 5
// EXPECT mem[0x108040] = 0x00000000fffffff5
// EXPECT r43 = 0x1234
// EXPECT mem[0x108048] = 0x1234
// EXPECT r44 = 0x12345678
// EXPECT mem[0x108050] = 0x99999999aaaaaaaa
// EXPECT r45 = 0xaaaaaaaa
// EXPECT r46 = 0x0000000000001111
// EXPECT mem[0x108058] = 0x00000000ffff2222
// EXPECT r47 = 0x108068
// EXPECT nat47 = 1
// EXPECT r48 = 0xfedcba9876543210
// EXPECT r50 = 0x7
// EXPECT r51 = 0xffffffffffffffff
// EXPECT mem[0x108070] = 0xfffffffffffffffe
.text
.global _start
_start:
  movl r2 = 0x108038
  movl r3 = 0x108000
  ;;
  mov ar.unat = r0
  ld8.fill r40 = [r3]      // unat bit0 = 0 => not NaT
  mov r9 = 1
  ;;
  mov ar.unat = r9          // bit 0
  ;;
  ld8.fill r40 = [r3]      // r40 NaT now
  ;;
  mov ar.unat = r0
  ;;
  st8.spill [r2] = r40
  ;;
  ld8.fill r41 = [r2]
  mov r20 = ar.unat
  // fetchadd4 -16 on 5
  movl r4 = 0x108040
  mov r5 = 5
  ;;
  st8 [r4] = r5
  ;;
  fetchadd4.acq r42 = [r4], -16
  // xchg2
  movl r6 = 0x108048
  movl r7 = 0x1234
  ;;
  st8 [r6] = r7
  ;;
  xchg2 r43 = [r6], r7
  // cmpxchg4 with CCV high bits set -> must fail
  movl r8 = 0x108050
  movl r10 = 0x9999999912345678
  movl r11 = 0x1000000012345678
  movl r12 = 0x99999999aaaaaaaa
  ;;
  st8 [r8] = r10
  mov ar.ccv = r11
  ;;
  cmpxchg4.acq r44 = [r8], r12, ar.ccv
  ;;
  movl r11 = 0x12345678
  ;;
  mov ar.ccv = r11
  ;;
  cmpxchg4.acq r45 = [r8], r12, ar.ccv
  ;;
  ld4 r45 = [r8]
  // st2 then ld2
  movl r13 = 0x108058
  movl r14 = 0xffffffff11112222
  ;;
  st4 [r13] = r14
  ;;
  st2 [r13] = r14
  ld2 r46 = [r13], 2
  ;;
  ld2 r46 = [r13]
  // post-increment with NaT increment register
  movl r15 = 0x108060
  ;;
  ld8 r49 = [r15], r40       // r15 += r40, nat(r15)=1
  ;;
  mov r47 = r15
  // MMIO read 8 bytes
  movl r16 = 0x120000
  ;;
  ld8 r48 = [r16]
  // fetchadd8 -1 on 0xffffffffffffffff
  movl r17 = 0x108070
  mov r18 = -1
  ;;
  st8 [r17] = r18
  ;;
  fetchadd8.rel r51 = [r17], -1
  mov r50 = 7
  ;;
  break.i 0x1ffffe
