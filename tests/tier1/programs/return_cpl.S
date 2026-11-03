/* SPDX-License-Identifier: GPL-2.0-or-later */
// br.ret to a less privileged level (SDM Vol.3 br.ret: PSR.cpl = PFS.ppl
// when PFS.ppl is numerically higher): a loop at cpl 0 calls .Lf, which on
// iteration r9 (p6) sets PFS.ppl = 3 and returns. A Tier1 region at the
// header follows the call; its return guard fails at that iteration and
// the region leaves before the br.ret, so the lower tier returns with
// PSR.cpl = 3 and the loop finishes there.
// Inputs (tests/tier1/upper_tests.c): r8 ar.lc (n - 1), r9 the iteration.
// Results: r14, r15 (n). Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      alloc r32 = ar.pfs, 0, 4, 1, 0
      mov r14 = r0
      mov r15 = r0 ;;
    }
    { .mlx
      nop.m 0
      movl r3 = 0xc000000000000000 ;;
    }
    { .mii
      nop.m 0
      mov.i ar.lc = r8
      nop.i 0 ;;
    }
.Lloop:
    { .mii
      nop.m 0
      cmp.eq p6, p7 = r14, r9
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lf ;;
    }
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
.Lf:
    { .mii
      nop.m 0
      mov.i r2 = ar.pfs ;;
(p6)  or r2 = r2, r3 ;;
    }
    { .mii
      nop.m 0
      mov.i ar.pfs = r2
      adds r15 = 1, r15 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
        .include "ivt.inc"
