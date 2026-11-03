/* SPDX-License-Identifier: GPL-2.0-or-later */
// Exit-outcome program: one write and one
// read of the test MMIO device, PAL_HALT_LIGHT through the static PAL entry
// (VCPU_EXIT_HALT), then a load from a GPA outside every region
// (VCPU_EXIT_BAD_ACCESS). Inputs (tests/tier1/dispatch_tests.c): r2 device
// address, r3 the value written, r14 the unmapped address (PAL
// procedures return in r8-r11). The PAL entry
// bundle is at image offset 0x200 (vcpu_config.pal_entry, ia64/pal.c:
// break.m 0x100000 in slot 0 of an M unit).
.text
.explicit
.global _start
_start:
    { .mmi
      st8 [r2] = r3
      nop.m 0
      nop.i 0 ;;
    }
    { .mmi
      ld8 r4 = [r2]
      nop.m 0
      mov r28 = 29 ;;                   // PAL_HALT_LIGHT (ia64/pal.h)
    }
    { .mii
      mov r29 = r0
      mov r30 = r0
      mov r31 = r0 ;;
    }
    { .mlx
      nop.m 0
      movl r5 = .Lreturn ;;
    }
    { .mlx
      nop.m 0
      movl r6 = .Lpal ;;
    }
    { .mii
      nop.m 0
      mov b0 = r5
      mov b6 = r6 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few b6 ;;
    }
.Lreturn:
    { .mmi
      ld8 r7 = [r14]                    // no region: VCPU_EXIT_BAD_ACCESS
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .org 0x200
.Lpal:
    { .mib
      break.m 0x100000
      nop.i 0
      br.sptk.few b0 ;;
    }
        .include "ivt.inc"
