/* SPDX-License-Identifier: GPL-2.0-or-later */
// Interruption vector table shared by every guest program,
// included last by each tests/tier1/programs/*.S. It sits at image offset 0x8000
// (GPA 0x108000, 32 KiB aligned as CR.IVA requires, SDM Vol.2 §3.3.5.4) and
// covers all 68 vectors of SDM Vol.2 Table 5-7. Vectors a guest run must
// never reach end the run with `break.i 0x1ffffd`, which the harness
// reports as "unexpected vector" (tests/tier1/harness.c).
//
// Handlers run with PSR.ic = 0 and PSR.bn = 0 (SDM Vol.2 §5.5), so they use
// bank-0 r16-r31, which the programs (PSR.bn = 1) never see. Bank-0
// registers the harness reads (tests/tier1/programs.c):
//   r20  Data TLB vector fills        (walker could not fill: VHPT page with A = 0)
//   r21  Alternate Data TLB fills     (regions without VHPT walker)
//   r22  Dirty-Bit faults (0x2000, SDM Vol.2 Table 5-7)
//   r24  external interrupts, r25 ar.itc at the last delivery (its first
//        instruction, so r25 - clock_offset is the retired count at which
//        the interrupt was delivered), r26 the last vector read from cr.ivr
//   r27  single-step traps
//   r30  (input) table PA - table VA payload, for region-2 fills
//   r29  Page Not Present fills (the walker found a VHPT entry with p = 0,
//        e.g. after a schedule UNMAP injection, tests/tier1/schedule.h)
//   r28  sum of cr.iip over all data-miss/dirty handler entries (guest-IP
//        aliases, gather-alias)
// Handlers may change p1 and p2; the programs do not use them.

        .set GUEST_IVT, 0x8000

.macro UNEXPECTED
    { .mii
      nop.m 0
      break.i 0x1ffffd
      nop.i 0 ;;
    }
.endm

// Insert the translation of cr.ifa's page with the PTE flags in r17 into
// the D-TC (SDM Vol.3 itc: PSR.ic = 0; ITIR comes from the TLB fault, i.e.
// RR.ps and RR.rid), then resume the faulting instruction. PA = VA payload,
// plus bank-0 r30 for the table (region 2), which the harness places at a
// PA other than its VA payload.
.macro FILL_AND_RETURN table
    { .mii
      mov r16 = cr.ifa
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      mov r18 = cr.iip
      dep r16 = 0, r16, 61, 3 ;;
      dep r16 = 0, r16, 0, 12 ;;
    }
.if \table
    { .mii
      add r16 = r16, r30
      nop.i 0
      nop.i 0 ;;
    }
.endif
    { .mmi
      or r16 = r16, r17
      add r28 = r28, r18
      nop.i 0 ;;
    }
    { .mmi
      itc.d r16 ;;
      srlz.d
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      rfi ;;
    }
.endm

        .org GUEST_IVT + 0x0000            // VHPT Translation
        UNEXPECTED
        .org GUEST_IVT + 0x0400            // Instruction TLB
        UNEXPECTED
        .org GUEST_IVT + 0x0800            // Data TLB: the walker gave up
    { .mlx
      adds r20 = 1, r20
      movl r17 = 0x421 ;;               // p, a, ar = 2 (RW), d = 0
    }
        FILL_AND_RETURN 1
        .org GUEST_IVT + 0x0c00            // Alternate Instruction TLB
        UNEXPECTED
        .org GUEST_IVT + 0x1000            // Alternate Data TLB: walker disabled
    { .mlx
      adds r21 = 1, r21
      movl r17 = 0x421 ;;               // d = 0: the first store per page takes Dirty-Bit
    }
        FILL_AND_RETURN 0
        .org GUEST_IVT + 0x1400            // Data Nested TLB
        UNEXPECTED
        .org GUEST_IVT + 0x1800            // Instruction Key Miss
        UNEXPECTED
        .org GUEST_IVT + 0x1c00            // Data Key Miss
        UNEXPECTED
        .org GUEST_IVT + 0x2000            // Dirty-Bit
    { .mlx
      adds r22 = 1, r22
      movl r17 = 0x461 ;;               // p, a, d, ar = 2
    }
        FILL_AND_RETURN 0
.irp off, 0x2400, 0x2800, 0x2c00
        .org GUEST_IVT + \off
        UNEXPECTED
.endr
        .org GUEST_IVT + 0x3000            // External Interrupt
    { .mmi
      mov r25 = ar.itc                  // first instruction: the delivery boundary
      mov r16 = cr.ivr
      adds r24 = 1, r24 ;;
    }
    { .mmi
      mov r26 = r16
      srlz.d                            // IVR side effects before the EOI (SDM Vol.2 §5.8.3.2)
      nop.i 0 ;;
    }
    { .mmi
      mov cr.eoi = r0 ;;
      srlz.d
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      rfi ;;
    }
.irp off, 0x3400, 0x3800, 0x3c00, 0x4000, 0x4400, 0x4800, 0x4c00
        .org GUEST_IVT + \off
        UNEXPECTED
.endr
        .org GUEST_IVT + 0x5000            // Page Not Present: walker inserted PTE.p = 0
    { .mlx
      adds r29 = 1, r29
      movl r17 = 0x421 ;;               // (an UNMAP injection cleared the VHPT entry)
    }
        FILL_AND_RETURN 1
.irp off, 0x5100, 0x5200, 0x5300, 0x5400, 0x5500, 0x5600, 0x5700, 0x5800, 0x5900, 0x5a00, 0x5b00, 0x5c00, 0x5d00, 0x5e00, 0x5f00
        .org GUEST_IVT + \off
        UNEXPECTED
.endr
        .org GUEST_IVT + 0x6000            // Single Step trap (lower-tier-step, PSR.ss = 1)
    { .mii
      nop.m 0
      adds r27 = 1, r27
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      rfi ;;
    }
.irp off, 0x6100, 0x6200, 0x6300, 0x6400, 0x6500, 0x6600, 0x6700, 0x6800, 0x6900, 0x6a00, 0x6b00, 0x6c00, 0x6d00, 0x6e00, 0x6f00
        .org GUEST_IVT + \off
        UNEXPECTED
.endr
.irp off, 0x7000, 0x7100, 0x7200, 0x7300, 0x7400, 0x7500, 0x7600, 0x7700, 0x7800, 0x7900, 0x7a00, 0x7b00, 0x7c00, 0x7d00, 0x7e00, 0x7f00
        .org GUEST_IVT + \off
        UNEXPECTED
.endr
        .org GUEST_IVT + 0x8000
