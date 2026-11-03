/* SPDX-License-Identifier: GPL-2.0-or-later */
// The guest gather loop and its driver, shared by the gather variants.
// Each variant .S sets the switches below with .set and includes this file;
// the harness (tests/tier1/programs.c) supplies every address and count in
// static registers, so the image itself holds no data layout:
//
//   r11  index array            -> r32 (restarts every pass)
//   r2   table base             -> r33
//   r3   output array           -> r34 (first pass; continues across passes
//                                  unless r10 bit 0 restarts it every pass)
//   r4   threshold              -> r35
//   r5   sentinel               -> r36 in the last pass (0 before: never
//                                  equal, every compared value is >= r4 > 0)
//   r6   status word (gather-load-after-store)    -> r38, loaded by S4, never stored to
//   r7   alternate code VRN (gather-alias; 0 = no alternation)
//   r12, r13  output VRN of the primary / alternate code alias (gather-alias)
//   r8   ar.lc for every pass (n - 1), r9 passes (>= 1), r10 flags
//
// Results: r14 sum, r15 skip count, r22 ar.lc at the side exit .Lfound,
// r21 sum of `mov ip` results (gather-alias). The run ends with
// `break.i 0x1ffffe`. The loop body is fixed: `.Lloop` and
// `.Lnext` start bundles, `br.cloop` sits in slot 2. p11-p13 belong to the
// driver, p1/p2 to the IVT handlers.
//
// Switches (0 or 1): GUEST_ROT (sor = 8 and one br.ctop before the loop, so
// CFM.rrb.gr != 0), GUEST_ROTP (p16-p20 instead of p6-p10, rotated so that
// they cross the p63 -> p16 wrap), GUEST_LOAD_AFTER_STORE (S4), GUEST_ALIAS (`mov ip` in the
// body, alias alternation), GUEST_2PAGE (loop straddles the 0x101000 page
// boundary).

.text
.explicit
.global _start

.macro GATHER_BODY pa, pb, ps, pn, pf
.Lloop:
    { .mmi
      ld8 r16 = [r32], 8                // S1: post-increment load
      nop.m 0
.if GUEST_ALIAS
      mov r23 = ip ;;
.else
      nop.i 0 ;;
.endif
    }
    { .mii
      nop.m 0
      cmp.eq \ps, \pn = r16, r0         // index 0 = skip
      shladd r17 = r16, 3, r33 ;;       // &table[index]
    }
    { .mib
      nop.m 0
(\ps) adds r15 = 1, r15                 // predicated pure op
(\ps) br.cond.dpnt.few .Lnext ;;        // inner forward branch
    }
    { .mmi
      ld8 r18 = [r17]                   // S2: data-dependent gather load
      nop.m 0
.if GUEST_ALIAS
      add r21 = r21, r23 ;;
.else
      nop.i 0 ;;
.endif
    }
    { .mii
      nop.m 0
      add r14 = r14, r18                // loop-carried sum
      cmp.ltu \pa, \pb = r18, r35 ;;
    }
    { .mii
(\pa) st8 [r34] = r18, 8                // S3: predicated store, post-increment
(\pb) cmp.eq.unc \pf, p0 = r18, r36     // unc compare under a predicate
      nop.i 0 ;;
    }
    { .mib
.if GUEST_LOAD_AFTER_STORE
      ld8 r19 = [r38]                   // S4: load after S3 in the same iteration
.else
      nop.m 0
.endif
      nop.i 0
(\pf) br.cond.dpnt.few .Lfound ;;       // side exit leaving the region
    }
.Lnext:
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lloop ;;       // counted back-edge (slot 2)
    }
.endm

_start:
    { .mii
.if GUEST_ROT
      alloc r37 = ar.pfs, 0, 8, 0, 8
.else
      alloc r37 = ar.pfs, 0, 8, 0, 0
.endif
      mov r14 = r0
      mov r15 = r0 ;;
    }
.if GUEST_ROT
    // One br.ctop (SDM Vol.3 br.ctop: LC != 0 -> LC--, PR[63] = 1, rotate)
    // leaves CFM.rrb.gr = 7 and CFM.rrb.pr = 47, so logical p16-p20 are
    // physical p63, p16-p19.
    { .mii
      nop.m 0
      mov ar.lc = 1
      mov ar.ec = r0 ;;
    }
    { .mii
      nop.m 0
      mov pr.rot = 0x2a5a50000
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.ctop.sptk.few .Lrotated ;;
    }
.Lrotated:
.endif
    { .mii
      mov r34 = r3
      mov r21 = r0
      mov r22 = r0 ;;
    }
.Lpass:
    { .mii
      mov r32 = r11
      mov ar.lc = r8
      cmp.eq p11, p12 = 1, r9 ;;
    }
    { .mii
      mov r33 = r2
(p11) mov r36 = r5
(p12) mov r36 = r0 ;;
    }
    { .mii
      mov r35 = r4
      tbit.nz p13, p0 = r10, 0
      extr.u r24 = r34, 61, 3 ;;
    }
    // r10 bit 0: the output restarts at r3's offset, in the output alias
    // (VRN) the pass currently uses.
.if GUEST_2PAGE
    { .mib
      mov r38 = r6
(p13) dep r34 = r24, r3, 61, 3
      br.sptk.few .Lloop ;;
    }
        .org 0x1000 - 0x30              // three bundles before the page boundary
.else
    { .mii
      mov r38 = r6
(p13) dep r34 = r24, r3, 61, 3
      nop.i 0 ;;
    }
.endif
.if GUEST_ROTP
        GATHER_BODY p16, p17, p18, p19, p20
.else
        GATHER_BODY p6, p7, p8, p9, p10
.endif
.Lend_pass:
    { .mii
      nop.m 0
      adds r9 = -1, r9 ;;
      cmp.eq p11, p0 = r0, r9 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p11) br.cond.dpnt.few .Ldone ;;
    }
.if GUEST_ALIAS
    // Alternate between the primary (VRN 0) and the alternate code alias
    // r7 of the same physical page, switching the output alias with it.
    // With r7 = 0 (physical bit-63 entry) the relative branch keeps the
    // executing alias.
    { .mii
      nop.m 0
      mov r24 = ip
      cmp.eq p11, p0 = r0, r7 ;;
    }
    { .mib
      nop.m 0
      extr.u r24 = r24, 61, 3
(p11) br.cond.dpnt.few .Lpass ;;
    }
    { .mlx
      cmp.eq p11, p12 = r0, r24
      movl r25 = .Lpass ;;
    }
    { .mii
      nop.m 0
(p11) dep r25 = r7, r25, 61, 3
(p11) dep r34 = r13, r34, 61, 3 ;;
    }
    { .mii
      nop.m 0
(p12) dep r34 = r12, r34, 61, 3
      mov b6 = r25 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few b6 ;;
    }
.else
    { .mib
      nop.m 0
      nop.i 0
      br.sptk.few .Lpass ;;
    }
.endif
.Lfound:
    { .mib
      nop.m 0
      mov r22 = ar.lc
      br.sptk.few .Lend_pass ;;
    }
.Ldone:
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .include "ivt.inc"
