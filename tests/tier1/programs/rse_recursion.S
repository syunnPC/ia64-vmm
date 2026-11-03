/* SPDX-License-Identifier: GPL-2.0-or-later */
// Frame-relative homes: one loop
// header reached at many frame bases, every time as the target of a br.ret
// with the caller's frame unloaded (the rse-return protocol of rse_return.S at every
// call depth). .Lf recurses r9 levels deep and every level allocates
// sof = sol = 9 (r32-r40), so level k runs at BOF 9k mod 96: level 10 at
// BOF 90, whose frame wraps the 96-register ring (90 + 9 > 96), and from
// level 11 on the ring is reused (alloc spills the oldest levels, SDM Vol.2
// §6.5). After its callee returns, each level calls .Lflush, which flushes
// the register stack and adds 100 to the backing-store copy of the level's
// r32 (its address, the level's ar.bsp, passed in r3), so br.ret lands on
// .Lhead with all 9 registers of the level unloaded (rse_missing = 9, CFLE
// set): the mandatory loads belong to the next instruction fetch (SDM
// Vol.2 §6.8), and an engine that ran the header before them would read the
// stale r32. The header loop reads r32 (frame offset 0) and r38 (offset 6:
// physical r32 at BOF 90). One Tier1 region serves the header at every
// BOF (one CFM).
// Inputs from the harness (tests/tier1/programs.c): r2 the first level's
// r32 (each level adds 7), r8 ar.lc of every header loop (n - 1), r9 the
// depth (>= 1). Physical mode. Results: r14, r15 (programs.c models them),
// r11 the number of levels.
// Ends with `break.i 0x1ffffe`.
.text
.explicit
.global _start
_start:
    { .mii
      nop.m 0
      mov r14 = r0
      mov r15 = r0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lf ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
.Lf:                                   // the recursive br.call is a backward branch: a hot head
    { .mii
      nop.m 0
      adds r11 = 1, r11                 // levels entered; a region of its own (ends at alloc)
      nop.i 0 ;;
    }
    { .mii
      alloc r40 = ar.pfs, 0, 9, 0, 0     // sof = sol = 9: r32-r40 are locals
      mov r39 = b0
      adds r9 = -1, r9 ;;
    }
    { .mii
      mov r37 = ar.bsp                   // where this level's r32 is spilled
      mov r32 = r2
      mov r38 = r9 ;;
    }
    { .mii
      nop.m 0
      cmp.eq p6, p7 = r0, r9
      adds r2 = 7, r2 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p7)  br.call.sptk.many b0 = .Lf ;;     // the next level, at BOF + 9
    }
    { .mii
      mov r3 = r37
      mov ar.lc = r8
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.call.sptk.many b0 = .Lflush ;;
    }
.Lhead:                                 // br.ret target and loop header
    { .mii
      nop.m 0
      add r14 = r14, r32                // reads the frame the br.ret unloaded
      add r15 = r15, r38 ;;
    }
    { .mib
      nop.m 0
      shladd r14 = r14, 1, r38
      br.cloop.sptk.few .Lhead ;;
    }
    { .mii
      nop.m 0
      mov b0 = r39
      mov ar.pfs = r40 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
.Lflush:                                // CFM.sof = 0: no frame of its own
    { .mii
      flushrs                           // first in its instruction group
      nop.i 0
      nop.i 0 ;;
    }
    { .mmi
      ld8 r10 = [r3]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      adds r10 = 100, r10
      nop.i 0 ;;
    }
    { .mmi
      st8 [r3] = r10
      nop.m 0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ret.sptk.many b0 ;;
    }
        .include "ivt.inc"
