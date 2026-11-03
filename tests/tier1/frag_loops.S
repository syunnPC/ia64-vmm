/* SPDX-License-Identifier: GPL-2.0-or-later */
// Fragments: the
// gather and shladd_xor loop bodies, position-independent (IP-relative branches only),
// so tests/tier1/unit_eval.c loads them anywhere and sets every register,
// predicate, CFM and ar.lc itself. tests/tier1/programs/*.S are the
// guest programs proper. Offsets are fixed with .org for the harness:
//   0x000  gather   (p6..p10)          0x200  gather with p16..p20 (gather-rotp)
//   0x400  shladd_xor                      0x600  .Lfound targets / ends
.text
.explicit
.global _start
_start:
// ---- gather: index gather, filter, sum -------------------------------------------
.Lloop:
    { .mmi
      ld8 r16 = [r32], 8
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      cmp.eq p8, p9 = r16, r0
      shladd r17 = r16, 3, r33
      nop.i 0 ;;
    }
    { .mib
      (p8) adds r15 = 1, r15
      nop.i 0
      (p8) br.cond.dpnt .Lnext ;;
    }
    { .mmi
      ld8 r18 = [r17]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      add r14 = r14, r18
      cmp.ltu p6, p7 = r18, r35
      nop.i 0 ;;
    }
    { .mii
      (p6) st8 [r34] = r18, 8
      (p7) cmp.eq.unc p10, p0 = r18, r36
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      (p10) br.cond.dpnt .Lfound ;;
    }
.Lnext:
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lloop ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lend ;;
    }

// ---- gather-rotp: the same loop on rotating predicates p16..p20 ----------
.org 0x200
.Lloop_p:
    { .mmi
      ld8 r16 = [r32], 8
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      cmp.eq p18, p19 = r16, r0
      shladd r17 = r16, 3, r33
      nop.i 0 ;;
    }
    { .mib
      (p18) adds r15 = 1, r15
      nop.i 0
      (p18) br.cond.dpnt .Lnext_p ;;
    }
    { .mmi
      ld8 r18 = [r17]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      add r14 = r14, r18
      cmp.ltu p16, p17 = r18, r35
      nop.i 0 ;;
    }
    { .mii
      (p16) st8 [r34] = r18, 8
      (p17) cmp.eq.unc p20, p0 = r18, r36
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      (p20) br.cond.dpnt .Lfound ;;
    }
.Lnext_p:
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lloop_p ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lend ;;
    }

// ---- shladd_xor kernel -------------------------------------
.org 0x400
.Lkernel:
    { .mii
      nop.m 0
      shladd r16 = r16, 3, r17
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      xor r16 = r16, r18
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      add r17 = r17, r16
      br.cloop.sptk.few .Lkernel ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lend ;;
    }

.org 0x600
.Lfound:
    { .mmi
      mov r19 = 1
      nop.m 0
      nop.i 0 ;;
    }
.Lend:
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lend ;;
    }
