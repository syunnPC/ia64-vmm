/* SPDX-License-Identifier: GPL-2.0-or-later */
// Concurrent Tier1, driven by
// tests/host/smp/tier1.c: one loop per entry point, each a Tier1 region
// once hot. Every loop ends with `break.i 0x1ffffe`.
//   0x000 shared: r20 iterations over r23 pages from r21 (stride r24);
//         each visit adds the word it reads to r14 and stores it + r9.
//   0x400 xmod:   until the word at r21 is nonzero; the bundle at 0x430
//         adds r9 to r14. Its replacement at 0x1400 adds r12 to r15.
//   0x800 flag:   ld.acq of the word at r21; the bundle at 0x820 sets
//         r11 = 1 (replacement at 0x1440: r11 = 2); once the word is
//         nonzero, r11 of that same iteration is stored at r22.
//   0xc00 purge:  ld.acq of the word at r21, then the word at r20; once
//         the first is nonzero, the second is stored at r22.
//   0x2800 checked: width-specific loads, one store, and a trailing load;
//         forced retries let a peer change data and the selected source.
//   0x2a00 checked memory-helper boundary (xchg8).
//   0x1800 chain: r19 passes of loop R (r20 iterations, this page), which
//         leaves through its exit branch to loop S on the next page
//         (0x2000): r11 iterations, in the last pass until the word at r21
//         (read in every pass) is nonzero; the bundle at 0x2020 adds r9 to
//         r14, its replacement at 0x2400 adds r12 to r15. Each pass starts
//         with an ar.itc read, outside the Tier1 subset, so R and S stay
//         separate regions.
.text
.explicit
.global _start
_start:
    { .mii
      mov r14 = r0
      mov r16 = r0
      mov r17 = r21 ;;
    }
.Lshared:
    { .mmi
      ld8 r10 = [r17]
      nop.m 0
      adds r20 = -1, r20 ;;
    }
    { .mii
      nop.m 0
      add r14 = r14, r10
      add r10 = r10, r9 ;;
    }
    { .mmi
      st8 [r17] = r10
      cmp.ne p8, p0 = r20, r0
      adds r16 = 1, r16 ;;
    }
    { .mii
      nop.m 0
      cmp.eq p6, p0 = r16, r23
      add r17 = r17, r24 ;;
    }
    { .mii
      nop.m 0
(p6)  mov r16 = r0
(p6)  mov r17 = r21 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p8)  br.cond.sptk.few .Lshared ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }

        .org 0x400
.Lxmod_entry:
    { .mii
      mov r14 = r0
      mov r15 = r0
      nop.i 0 ;;
    }
.Lxmod:
    { .mmi
      ld8 r10 = [r21]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      cmp.ne p6, p0 = r10, r0
      nop.i 0 ;;
    }
        .org 0x430                      // no gap: .org fills with zeros
    { .mii
      nop.m 0
      add r14 = r14, r9
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p6)  br.cond.dpnt.few .Lxmod_end ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lxmod ;;
    }
.Lxmod_end:
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }

        .org 0x800
.Lflag:
    { .mmi
      ld8.acq r10 = [r21]
      nop.m 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      cmp.ne p6, p0 = r10, r0
      nop.i 0 ;;
    }
        .org 0x820
    { .mii
      nop.m 0
      mov r11 = 1
      nop.i 0 ;;
    }
    { .mmi
(p6)  st8 [r22] = r11
      nop.m 0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p6)  br.cond.dpnt.few .Lflag_end ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lflag ;;
    }
.Lflag_end:
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }

        .org 0xc00
.Lpurge:
    { .mmi
      ld8.acq r10 = [r21]
      nop.m 0
      nop.i 0 ;;
    }
    { .mmi
      ld8 r11 = [r20]
      cmp.ne p6, p0 = r10, r0
      nop.i 0 ;;
    }
    { .mmi
(p6)  st8 [r22] = r11
      nop.m 0
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p6)  br.cond.dpnt.few .Lpurge_end ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lpurge ;;
    }
.Lpurge_end:
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }

        .org 0x1400                     // replacements, never executed here
    { .mii
      nop.m 0
      add r15 = r15, r12
      nop.i 0 ;;
    }
        .org 0x1440
    { .mii
      nop.m 0
      mov r11 = 2
      nop.i 0 ;;
    }

        .org 0x1800
.Lchain:
    { .mii
      mov r14 = r0
      mov r15 = r0
      nop.i 0 ;;
    }
.Lchain_pass:
    { .mii
      mov r30 = ar.itc
      mov r18 = r20
      cmp.eq p9, p0 = 1, r19 ;;
    }
    { .mii
      mov r16 = r0
      mov r17 = r11 ;;
(p9)  mov r17 = r0 ;;
    }
    { .mii
      nop.m 0
(p9)  mov r16 = -1
      nop.i 0 ;;
    }
.Lchain_r:
    { .mii
      nop.m 0
      adds r18 = -1, r18 ;;
      cmp.eq p7, p0 = r0, r18 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p7)  br.cond.dpnt.few .Lchain_s ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lchain_r ;;
    }

        .org 0x2000
.Lchain_s:
    { .mmi
      ld8.acq r10 = [r21]
      nop.m 0
      adds r17 = -1, r17 ;;
    }
    { .mii
      and r10 = r10, r16
      cmp.eq p8, p0 = r0, r17 ;;
      cmp.ne p6, p0 = r10, r0 ;;
    }
        .org 0x2020
    { .mii
      nop.m 0
      add r14 = r14, r9
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p6)  br.cond.dpnt.few .Lchain_end ;;
    }
    { .mib
      nop.m 0
      nop.i 0
(p8)  br.cond.dpnt.few .Lchain_next ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lchain_s ;;
    }
.Lchain_next:
    { .mii
      nop.m 0
      adds r19 = -1, r19
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lchain_pass ;;
    }
.Lchain_end:
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .org 0x2400                     // replacement, never executed here
    { .mii
      nop.m 0
      add r15 = r15, r12
      nop.i 0 ;;
    }

        .org 0x2800
.Lchecked:
    { .mii
      ld1 r10 = [r21]
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      ld2 r11 = [r21]
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      ld4 r12 = [r21]
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      ld8 r13 = [r21]
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      st8 [r22] = r13
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      ld8 r14 = [r23]
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      adds r15 = 17, r0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
        .org 0x2900
    { .mii
      nop.m 0
      adds r15 = 1000, r0
      nop.i 0 ;;
    }

        .org 0x2a00
    { .mii
      xchg8 r10 = [r21], r12
      nop.i 0
      nop.i 0 ;;
    }

        .org 0x2b00
    { .mii
      ldfs f6 = [r21], 8
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      ldfd f7 = [r21], 8
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      ldf8 f8 = [r21], 8
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      stfs [r22] = f6, 8
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      stfd [r22] = f7, 8
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      stf8 [r22] = f8, 8
      nop.i 0
      nop.i 0 ;;
    }
    { .mii
      nop.m 0
      break.i 0x1ffffe
      nop.i 0 ;;
    }
