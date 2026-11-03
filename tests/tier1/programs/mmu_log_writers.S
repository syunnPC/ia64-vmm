/* SPDX-License-Identifier: GPL-2.0-or-later */
// MMU log: every MMU-log writer
// family runs between two Tier1 region entries. Fourteen loops; each loop
// header is a region head whose body loads through windows (A: region 1,
// a D-TC entry refilled by the Alternate Data TLB handler; B: region 3,
// DTR 0; W: region 6, 64 KiB pages through the VHPT walker) and ends at the
// loop's writer block, which the lower tier executes before the back-edge:
//   itc.d (re-insertion of A: overlap purge + insertion), ptc.l, ptc.g,
//   ptc.ga, ptc.e, ptr.d of A's TC entry (each followed by a load of A,
//   whose Alternate Data TLB fault refills it before the next entry, so the
//   region runs its whole body), itr.d (two contents in turn),
//   itr.d/ptr.d of a TR in turn, itc.i/ptr.i and itr.i/ptr.i in turn
//   (I side), mov rr and mov pkr (two values in turn), mov pkr of one key
//   into two registers in turn (the other loses its valid bit), and the
//   walker (ptc.l of W's 64 KiB entry and itc.d of a 4 KiB entry in its
//   page; the next load walks, inserts 64 KiB and purges the 4 KiB entry,
//   inside hostwin_resolve when Tier1 makes the access).
// Virtual mode, PSR.pk = 0; the harness (tests/tier1/programs.c) sets the
// registers below, the TRs, RRs, PTA and VHPT. Result: r15 = sum of all
// loaded words (r31 accumulates half of them until the end). Ends with
// `break.i 0x1ffffe`.
//   r2 A, r3 B, r4/r5 the two itr.d VAs, r6 the itr.d/ptr.d VA, r7 the
//   itc.i VA, r8 the itr.i VA, r9 ITIR (4 KiB), r10 ITIR (64 KiB), r11 a
//   PTE for the TR/I-side entries, r12 A's PTE, r13 W + 0x3000, r14 loop
//   count - 1, r17 W, r18 region 5, r19/r20 its two RR values, r21/r22 two
//   PKR values, r23 the duplicated key's PKR value, r24 W's 4 KiB PTE.
.text
.explicit
.global _start

// One M-slot instruction alone in its instruction group.
.macro M1 insn:vararg
    { .mmi
      \insn
      nop.m 0
      nop.i 0 ;;
    }
.endm
// Every loop starts its turns at p6 (r28 = 0).
.macro HEAD label
    { .mii
      nop.m 0
      mov ar.lc = r14
      mov r28 = r0 ;;
    }
\label:
.endm
// The region body: twelve loads of A and of B, summed in r15 and r31 (the
// regions must retire most of the program, tests/tier1/unit_programs.c).
.macro BODY_AB
.rept 12
    { .mmi
      ld8 r16 = [r2]
      ld8 r25 = [r3]
      nop.i 0 ;;
    }
    { .mii
      add r15 = r15, r16
      add r31 = r31, r25
      nop.i 0 ;;
    }
.endr
.endm
// p6/p7 alternate per iteration, p6 first (r28 toggles); r29 = \slot.
.macro TURN slot
    { .mii
      nop.m 0
      tbit.z p6, p7 = r28, 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      xor r28 = 1, r28
      mov r29 = \slot ;;
    }
.endm
.macro IC_OFF
    M1 rsm psr.ic
    M1 srlz.d
.endm
.macro IC_ON
    M1 ssm psr.ic
    M1 srlz.d
.endm
.macro BACK label
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few \label ;;
    }
.endm

_start:
    { .mii
      nop.m 0
      mov r15 = r0
      mov r28 = r0 ;;
    }
    { .mii
      nop.m 0
      mov r30 = 7
      mov r31 = r0 ;;
    }

    HEAD .Litc                          // itc.d: A re-inserted (overlap purge + insertion)
    BODY_AB
    IC_OFF
    M1 mov cr.ifa = r2
    M1 mov cr.itir = r9
    M1 itc.d r12
    IC_ON
    BACK .Litc

    HEAD .Lptcl                         // ptc.l of A; the next load refills it
    BODY_AB
    M1 ptc.l r2, r9
    M1 srlz.d
    M1 ld8 r26 = [r2]
    BACK .Lptcl

    HEAD .Lptcg                         // ptc.g
    BODY_AB
    M1 ptc.g r2, r9
    M1 srlz.d
    M1 ld8 r26 = [r2]
    BACK .Lptcg

    HEAD .Lptcga                        // ptc.ga
    BODY_AB
    M1 ptc.ga r2, r9
    M1 srlz.d
    M1 ld8 r26 = [r2]
    BACK .Lptcga

    HEAD .Lptce                         // ptc.e: every TC entry
    BODY_AB
    M1 ptc.e r2
    M1 srlz.d
    M1 ld8 r26 = [r2]
    BACK .Lptce

    HEAD .Lptrtc                        // ptr.d of A's TC entry
    BODY_AB
    M1 ptr.d r2, r9
    M1 srlz.d
    M1 ld8 r26 = [r2]
    BACK .Lptrtc

    HEAD .Litr                          // itr.d dtr[3], two contents in turn
    BODY_AB
    TURN 3
    IC_OFF
    M1 (p6) mov cr.ifa = r4
    M1 (p7) mov cr.ifa = r5
    M1 mov cr.itir = r9
    M1 itr.d dtr[r29] = r11
    IC_ON
    BACK .Litr

    HEAD .Lptrtr                        // itr.d dtr[4] and ptr.d in turn
    BODY_AB
    TURN 4
    IC_OFF
    M1 mov cr.ifa = r6
    M1 mov cr.itir = r9
    M1 (p6) itr.d dtr[r29] = r11
    M1 (p7) ptr.d r6, r9
    IC_ON
    BACK .Lptrtr

    HEAD .Litci                         // I side: itc.i and ptr.i in turn
    BODY_AB
    TURN 0
    IC_OFF
    M1 mov cr.ifa = r7
    M1 mov cr.itir = r9
    M1 (p6) itc.i r11
    M1 (p7) ptr.i r7, r9
    IC_ON
    M1 srlz.i
    BACK .Litci

    HEAD .Litri                         // I side: itr.i itr[5] and ptr.i in turn
    BODY_AB
    TURN 5
    IC_OFF
    M1 mov cr.ifa = r8
    M1 mov cr.itir = r9
    M1 (p6) itr.i itr[r29] = r11
    M1 (p7) ptr.i r8, r9
    IC_ON
    M1 srlz.i
    BACK .Litri

    HEAD .Lrr                           // mov rr[region 5], two values in turn
    BODY_AB
    TURN 0
    M1 (p6) mov rr[r18] = r19
    M1 (p7) mov rr[r18] = r20
    M1 srlz.d
    BACK .Lrr

    HEAD .Lpkr                          // mov pkr[5], two values in turn
    BODY_AB
    TURN 5
    M1 (p6) mov pkr[r29] = r21
    M1 (p7) mov pkr[r29] = r22
    M1 srlz.d
    BACK .Lpkr

    HEAD .Lpkrdup                       // one key into pkr[6] and pkr[7] in turn
    BODY_AB
    TURN 6
    M1 (p6) mov pkr[r29] = r23
    M1 (p7) mov pkr[r30] = r23
    M1 srlz.d
    BACK .Lpkrdup

    HEAD .Lwalk                         // the VHPT walker
.rept 6
    { .mmi
      ld8 r16 = [r13]
      ld8 r25 = [r13]
      nop.i 0 ;;
    }
    { .mii
      add r15 = r15, r16
      add r31 = r31, r25
      nop.i 0 ;;
    }
.endr
    IC_OFF
    M1 ptc.l r17, r10
    M1 srlz.d
    M1 mov cr.ifa = r17
    M1 mov cr.itir = r9
    M1 itc.d r24
    IC_ON
    BACK .Lwalk

    { .mii
      nop.m 0
      add r15 = r15, r31
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .include "ivt.inc"
