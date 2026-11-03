/* SPDX-License-Identifier: GPL-2.0-or-later */
// Exact helper and native coverage fragments, entered from the harness.
.text
.explicit
.global _start
_start:
.Lhelpers:
    { .mib
      add r40 = r41, r42
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      (p6) mov ar.unat = r40
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      (p6) mov r43 = ar.unat
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      (p7) mov b6 = r40
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      (p7) mov r44 = b6
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      padd4 r45 = r41, r42
      nop.b 0 ;;
    }
    { .mib
      (p6) setf.sig f32 = r41
      nop.i 0
      nop.b 0 ;;
    }
    { .mfi
      nop.m 0
      (p7) xma.l f33 = f32, f34, f35
      nop.i 0 ;;
    }
    { .mib
      nop.m 0
      pshr4.u r47 = r42, 7
      nop.b 0 ;;
    }
    { .mib
      getf.sig r46 = f33
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      st8.spill [r2] = r46
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lhelpers ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x200
.Laddp4:
    { .mib
      nop.m 0
      (p6) addp4 r40 = r41, r42
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      (p7) addp4 r43 = -17, r42
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      shladdp4 r44 = r41, 3, r42
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Laddp4 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x400
.Llc:
    { .mib
      nop.m 0
      (p6) mov ar.lc = r41
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      (p7) mov r40 = ar.lc
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      (p6) mov ar.lc = 7
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      mov r43 = ar.lc
      nop.b 0 ;;
    }
    { .mib
      adds r42 = -1, r42
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      cmp.ne p8, p9 = r42, r0
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      (p8) br.cond.sptk.few .Llc ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x600
.Lstitch:
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lstitch_target ;;
    }
    { .mii
      alloc r40 = ar.pfs, 4, 4, 0, 0
      nop.i 0
      nop.i 0 ;;
    }
.org 0x700
.Lstitch_target:
    { .mib
      add r40 = r41, r42
      nop.i 0
      br.cloop.sptk.few .Lstitch ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x800
.Lstore:
    { .mib
      st8.spill [r2] = r40
      add r41 = 1, r41
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lstore ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0xa00
.Lwalk:
    { .mib
      ld8.fill r40 = [r2]
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      ld8 r41 = [r3]
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lwalk ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0xc00
.Lback_target:
    { .mib
      add r40 = r41, r42
      nop.i 0
      br.cloop.sptk.few .Lback_stitch ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0xe00
.Lback_stitch:
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lback_target ;;
    }
.org 0x1000
.Ldeferred:
    { .mib
      ld8.s r40 = [r2]
      nop.i 0
      br.cloop.sptk.few .Ldeferred ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1200
.Lspill_nat:
    { .mib
      st8.spill [r2] = r46
      nop.i 0
      br.cloop.sptk.few .Lspill_nat ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1400
.Lfalse_clear:
    { .mib
      (p6) mov r40 = ar.unat
      nop.i 0
      nop.b 0 ;;
    }
    { .mib
      st8.spill [r2] = r40
      nop.i 0
      br.cloop.sptk.few .Lfalse_clear ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1600
.Lpredicates:
    { .mib
      nop.m 0
      mov pr = r40, 0x1fffe
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      mov r41 = pr
      nop.b 0 ;;
    }
    { .mib
      nop.m 0
      mov pr.rot = 0x15550000
      nop.b 0 ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1800
    { .mib
      st8.spill [r2] = r40
      add r41 = 7, r41
      nop.b 0 ;;
    }
.org 0x1a00
    { .mib
      mov ar.k0 = r40
      nop.i 0
      nop.b 0 ;;
    }
.org 0x1c00
.Lfpsr:
    { .mib
      mov ar.fpsr = r40
      nop.i 0
      br.cloop.sptk.few .Lfpsr ;;
    }
.org 0x1e00
.Lnative_store:
    { .mib
      st8 [r2] = r40
      nop.i 0
      br.cloop.sptk.few .Lnative_store ;;
    }
.org 0x2000
.Lpage_stitch:
    { .mib
      nop.m 0
      nop.i 0
      br.cond.sptk.few .Lpage_target ;;
    }
    { .mii
      alloc r40 = ar.pfs, 4, 4, 0, 0
      nop.i 0
      nop.i 0 ;;
    }
.org 0x4000
.Lpage_target:
    { .mib
      add r40 = 1, r40
      nop.i 0
      br.cloop.sptk.few .Lpage_stitch ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x4200
    { .mib
      add r40 = 7, r40
      nop.i 0
      nop.b 0 ;;
    }
.org 0x5000
.Lpr_read:
    { .mib
      nop.m 0
      mov r41 = pr
      br.cloop.sptk.few .Lpr_read ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x5200
.Lpr_loop:
    { .mii
      nop.m 0
      mov pr = r40, 0x1fffe
      adds r40 = 3, r40 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lpr_loop ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x5400
.Lspec_loads:
    { .mmi
      ld8.a r40 = [r2]
      ld1.s r41 = [r3], 1
      nop.i 0 ;;
    }
    { .mmi
      ld8.c.clr r42 = [r2]
      ld4.sa r43 = [r2]
      nop.i 0 ;;
    }
    { .mmb
      ld2.c.nc r44 = [r2]
      ld8.s r45 = [r2]
      br.cloop.sptk.few .Lspec_loads ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
.org 0x5600
.Lpcmp:
    { .mii
      nop.m 0
      cmp.eq p6, p7 = r0, r0
      cmp.eq p8, p9 = r0, r0 ;;
    }
    { .mii
      nop.m 0
      cmp.eq.and p6, p7 = r40, r41
      cmp4.eq.or.andcm p8, p9 = 5, r42 ;;
    }
    { .mii
      nop.m 0
      cmp.gt.and p10, p11 = r0, r43
      (p6) cmp.ne.or p12, p13 = r40, r42 ;;
    }
    { .mii
      nop.m 0
      tbit.z p14, p15 = r41, 3
      (p8) tbit.nz.unc p16, p17 = r42, 63 ;;
    }
    { .mii
      nop.m 0
      tbit.nz.or.andcm p18, p19 = r43, 0
      (p9) tbit.z.and p20, p21 = r40, 1 ;;
    }
    { .mii
      nop.m 0
      tnat.nz p22, p23 = r42
      (p10) adds r44 = 1, r44 ;;
    }
    { .mii
      (p12) adds r45 = 1, r45
      (p16) adds r46 = 1, r46
      (p18) adds r47 = 1, r47 ;;
    }
    { .mii
      adds r41 = 1, r41
      adds r42 = -3, r42
      shladd r43 = r43, 1, r41 ;;
    }
    { .mib
      nop.m 0
      nop.i 0
      br.cloop.sptk.few .Lpcmp ;;
    }
    { .mib
      break.m 0x1ffff
      nop.i 0
      nop.b 0 ;;
    }
