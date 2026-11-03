/* SPDX-License-Identifier: GPL-2.0-or-later */
// chk.s on a NaT set at entry: a loop whose header uses
// r20, whose NaT bit the harness sets, in an add (the result's NaT bit is
// set) and a cmp (both targets cleared, SDM Vol.3 cmp), then runs chk.s.i
// on it, then chk.s on f6, which holds NaTVal; every iteration branches to
// both recovery codes (SDM Vol.3 chk), which count and return. A Tier1
// region at the header reads r20's NaT bit and f6's NaTVal at run time and
// leaves to the recovery code: no entry is declined at the NaT guard.
// Inputs (tests/tier1/upper_tests.c): r20 (NaT set), f6 (NaTVal), r8 ar.lc
// (n - 1). Results: r14, r15, r19 (n), r16 (NaT), r17, r18 (0). Ends with
// `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      mov r14 = r0
      mov ar.lc = r8
      mov r15 = r0 ;;
    }
    { .mii
      mov r19 = r0
      mov r17 = r0
      mov r18 = r0 ;;
    }
.Lloop:
    { .mii
      add r16 = r20, r14
      cmp.eq p6, p7 = r20, r20
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
(p6)  adds r17 = 1, r17
(p7)  adds r18 = 1, r18 ;;
    }
    { .mii
      nop.m 0
      chk.s.i r20, .Lrecover
      nop.i 0 ;;
    }
.Lback:
    { .mii
      chk.s f6, .Lrecoverf
      nop.i 0
      nop.i 0 ;;
    }
.Lbackf:
    { .mib
      nop.m 0
      adds r14 = 1, r14
      br.cloop.sptk.few .Lloop ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
.Lrecover:
    { .mib
      nop.m 0
      adds r15 = 1, r15
      br.sptk.few .Lback ;;
    }
.Lrecoverf:
    { .mib
      nop.m 0
      adds r19 = 1, r19
      br.sptk.few .Lbackf ;;
    }
        .include "ivt.inc"
