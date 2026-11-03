/* SPDX-License-Identifier: GPL-2.0-or-later */
// RSE incomplete-frame returns (Tier1): every return lands on a hot loop header with the
// caller's frame unloaded. The callee flushes the register stack
// (flushrs), so br.ret restores the caller frame with all sol = 8 of its
// registers missing (rse_missing = 8, CFLE set); the mandatory loads happen
// at the next instruction fetch (SDM Vol.2 §6.8, ia64_rse_resume from
// ia64_translate(IA64_REF_FETCH)). The callee also adds 100 to the
// backing-store copy of r32, so the loaded r32 differs from the stale
// physical register: an engine that ran the header before the loads would
// sum the stale value. The dispatch test (dispatch_tests.c, rse-dispatch) also
// returns to the header through rfi from an external interrupt.
// Inputs from the harness (tests/tier1/programs.c): r2 the initial r32,
// r8 ar.lc for each inner loop (n - 1), r9 the number of calls. Physical
// mode, so Tier1's fetch guard performs no translation at the header.
// Results: r14, r32 (= r2 + 100 * calls). Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      alloc r37 = ar.pfs, 0, 8, 0, 0     // sof = sol = 8: r32-r39 are locals
      mov r14 = r0
      mov r32 = r2 ;;
    }
.Lcall:
    { .mii
      nop.m 0
      adds r9 = -1, r9
      mov ar.lc = r8 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lfunc ;;
    }
.Lhead:                                 // br.ret target and loop header
    { .mii
      nop.m 0
      add r14 = r14, r32                // reads the caller frame
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      shladd r14 = r14, 1, r32
      br.cloop.sptk.few .Lhead ;;
    }
    { .mii
      nop.m 0
      cmp.eq p6, p7 = r0, r9
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p7)  br.cond.sptk.few .Lcall ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
.Lfunc:                                 // CFM.sof = 0: no frame of its own
    { .mii
      flushrs                           // first in its instruction group
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      mov r2 = ar.bsp                   // = BSPSTORE after flushrs
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      adds r2 = -64, r2                 // the caller's r32 (8 slots, no RNaT slot between)
      nop.i 0 ;;
    }
    { .mmi
      ld8 r3 = [r2]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      adds r3 = 100, r3
      nop.i 0 ;;
    }
    { .mmi
      st8 [r2] = r3
      nop.m 0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
        .include "ivt.inc"
