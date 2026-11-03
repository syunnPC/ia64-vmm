/* SPDX-License-Identifier: GPL-2.0-or-later */
// Entry and exit of a benchmark job (see docs/testing.md).
//
// bench_run(ctx) enters a job with the machine state the hosted Tier1
// harness gives it (tests/tier1/programs.c: vcpu_reset + guest_setup): every
// static GR, bank-0 r16-r31, b0-b7, pr, the ARs the jobs read, cr.iva,
// cr.pta, cr.dcr, then rfi to the job's ip with its psr and CFM.sof = 0.
// benchwrap.c sets region registers, TRs and the TC purges beforehand and
// ar.k5-k7 (resume psr, resume ip, ctx) for the exit path.
//
// The jobs end with `break.i 0x1ffffe`; the harness stops there, the
// wrapper's copy of the job's IVT has bench_exit_stub at the Break vector
// (0x2c00, a vector the jobs otherwise never reach), which records ar.itc
// and rfi's to bench_resume in physical mode with interruptions off.
// bench_resume stores the job's final state, restores the firmware state
// with PSR.i clear (the stub overwrote ar.k1-k4, benchwrap.c restores them
// and sets PSR.i) and returns to bench_run's caller.
//
// RSE: flushrs empties the dirty partition before the job runs, so the
// job's own frame (alloc ..., 0, 8, 0, 0 at most) never spills and its
// backing-store pointers are irrelevant; br.ret at the end refills the
// caller's frame from the firmware's backing store (SDM Vol.2 §6.5).
// cr.tpr.mmi masks external interrupts for the job (PSR stays the job's,
// PSR.i included) so firmware timer interrupts cannot enter the job's IVT.
#include "ctx.h"

        .explicit
        .text

// UINT64 bench_pal_static(UINT64 index, UINT64 a1, UINT64 a2, UINT64 a3,
//                         UINT64 entry, UINT64 out[3])
// PAL static calling convention (SDM Vol.2 §11.10.2.1): r28 index,
// r29-r31 arguments, return status r8 and values r9-r11, return through
// b0; called with interruptions masked and the RSE in enforced lazy mode.
        .align 32
        .global bench_pal_static
        .proc bench_pal_static
bench_pal_static:
        alloc loc0 = ar.pfs, 6, 4, 0, 0
        mov loc1 = b0
        ;;
        mov loc2 = psr
        mov loc3 = ar.rsc
        mov r28 = in0
        mov r29 = in1
        ;;
        mov r30 = in2
        mov r31 = in3
        mov b7 = in4
        ;;
        mov ar.rsc = 0
        ;;
        rsm psr.i
        ;;
1:      mov r8 = ip
        ;;
        adds r8 = 2f - 1b, r8
        ;;
        mov b0 = r8
        ;;
        br.cond.sptk.many b7
        ;;
2:      mov ar.rsc = loc3
        ;;
        mov psr.l = loc2
        ;;
        srlz.d
        ;;
        st8 [in5] = r9, 8
        ;;
        st8 [in5] = r10, 8
        ;;
        st8 [in5] = r11
        mov b0 = loc1
        mov ar.pfs = loc0
        ;;
        br.ret.sptk.many b0
        ;;
        .endp bench_pal_static

// Stores \reg at ctx + \off (r2 = ctx, r3 scratch).
        .macro PUT reg, off
        adds r3 = \off, r2
        ;;
        st8 [r3] = \reg
        ;;
        .endm
// Loads ctx + \off into \reg.
        .macro GET reg, off
        adds r3 = \off, r2
        ;;
        ld8 \reg = [r3]
        ;;
        .endm

// void bench_run(struct ctx *ctx)
        .align 32
        .global bench_run
        .proc bench_run
bench_run:
        mov r2 = r32
        ;;
        // Drop the frame: the job starts with CFM.sof = 0 as in vcpu_reset.
        alloc r14 = ar.pfs, 0, 0, 0, 0
        ;;
        PUT r14, SAV_PFS
        PUT r1, SAV_GP
        PUT r12, SAV_SP
        PUT r13, SAV_TP
        PUT r4, SAV_R4
        PUT r5, SAV_R4 + 8
        PUT r6, SAV_R4 + 16
        PUT r7, SAV_R4 + 24
        mov r14 = b0
        ;;
        PUT r14, SAV_B0
        mov r14 = b1
        ;;
        PUT r14, SAV_B0 + 8
        mov r14 = b2
        ;;
        PUT r14, SAV_B0 + 16
        mov r14 = b3
        ;;
        PUT r14, SAV_B0 + 24
        mov r14 = b4
        ;;
        PUT r14, SAV_B0 + 32
        mov r14 = b5
        ;;
        PUT r14, SAV_B0 + 40
        mov r14 = ar.lc
        ;;
        PUT r14, SAV_LC
        mov r14 = ar.ec
        ;;
        PUT r14, SAV_EC
        mov r14 = ar.unat
        ;;
        PUT r14, SAV_UNAT
        mov r14 = ar.fpsr
        ;;
        PUT r14, SAV_FPSR
        mov r14 = ar.rsc
        ;;
        PUT r14, SAV_RSC
        mov r14 = ar.ccv
        ;;
        PUT r14, SAV_CCV
        mov r14 = pr
        ;;
        PUT r14, SAV_PR
        mov r14 = psr
        ;;
        PUT r14, SAV_PSR
        mov r14 = cr.iva
        ;;
        PUT r14, SAV_IVA
        mov r14 = cr.pta
        ;;
        PUT r14, SAV_PTA
        mov r14 = cr.dcr
        ;;
        PUT r14, SAV_DCR
        mov r14 = cr.tpr
        ;;
        PUT r14, SAV_TPR
        rsm psr.i | psr.ic
        ;;
        srlz.d
        ;;
        // Bank 0: save the firmware's r16-r31, load the job's.
        adds r14 = SAV_BANK0, r2
        adds r15 = CTX_BANK0, r2
        bsw.0
        ;;
        .irp r, r16, r17, r18, r19, r20, r21, r22, r23, r24, r25, r26, r27, r28, r29, r30, r31
        st8 [r14] = \r, 8
        ;;
        ld8 \r = [r15], 8
        ;;
        .endr
        bsw.1
        ;;
        flushrs
        ;;
        mov ar.rsc = 0
        ;;
        // The job's backing store: the dirty partition is empty, so the
        // write makes ar.bsp = ar.bspstore (SDM Vol.2 §6.4); RNAT is
        // undefined after it and is set from the job (vcpu_reset: 0).
        mov r14 = ar.bspstore
        ;;
        PUT r14, SAV_BSPSTORE
        mov r14 = ar.rnat
        ;;
        PUT r14, SAV_RNAT
        GET r14, CTX_BSPSTORE
        mov ar.bspstore = r14
        ;;
        GET r14, CTX_RNAT
        mov ar.rnat = r14
        ;;
        movl r14 = TPR_MMI
        ;;
        mov cr.tpr = r14
        ;;
        GET r14, CTX_IVA
        mov cr.iva = r14
        ;;
        // vcpu_reset leaves PTA = 0 for physical-mode jobs; PTA.size = 0
        // is reserved (SDM Vol.2 §5.3.2, the fork faults on it) and PTA is
        // unused in physical mode, so the firmware's value stays then.
        GET r14, CTX_PTA
        cmp.eq p6, p7 = 0, r14
        ;;
(p7)    mov cr.pta = r14
        ;;
        srlz.d
        ;;
        GET r14, CTX_DCR
        mov cr.dcr = r14
        ;;
        srlz.d
        ;;
        srlz.i
        ;;
        GET r14, CTX_FPSR
        mov ar.fpsr = r14
        ;;
        GET r14, CTX_LC
        mov ar.lc = r14
        ;;
        GET r14, CTX_EC
        mov ar.ec = r14
        ;;
        GET r14, CTX_UNAT
        mov ar.unat = r14
        ;;
        GET r14, CTX_CCV
        mov ar.ccv = r14
        ;;
        GET r14, CTX_PFS
        mov ar.pfs = r14
        ;;
        GET r14, CTX_BR
        mov b0 = r14
        GET r14, CTX_BR + 8
        mov b1 = r14
        GET r14, CTX_BR + 16
        mov b2 = r14
        GET r14, CTX_BR + 24
        mov b3 = r14
        GET r14, CTX_BR + 32
        mov b4 = r14
        GET r14, CTX_BR + 40
        mov b5 = r14
        GET r14, CTX_BR + 48
        mov b6 = r14
        GET r14, CTX_BR + 56
        mov b7 = r14
        GET r14, CTX_PR
        mov pr = r14, -1
        ;;
        GET r14, CTX_IP
        mov cr.iip = r14
        GET r14, CTX_PSR
        mov cr.ipsr = r14
        ;;
        mov cr.ifs = r0
        GET r14, CTX_RSC
        mov ar.rsc = r14
        ;;
        mov r14 = ar.itc
        ;;
        PUT r14, OUT_ITC0
        // Static GRs last, through r2 as the moving pointer (slot 2 is
        // skipped); the job's r2 itself is loaded last of all.
        adds r2 = CTX_GR + 8, r2
        ;;
        ld8 r1 = [r2], 16
        ;;
        .irp r, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15, r16, r17, r18, r19, r20, r21, r22, r23, r24, r25, r26, r27, r28, r29, r30, r31
        ld8 \r = [r2], 8
        ;;
        .endr
        adds r2 = -(32 - 2) * 8, r2
        ;;
        ld8 r2 = [r2]
        ;;
        rfi
        ;;
        .endp bench_run

// Copied by benchwrap.c to the job IVT's Break vector (0x2c00, 64 bundles);
// position independent. Runs with PSR.ic = 0 and bank 0 (r16-r19 are the
// IVT's scratch registers, tests/tier1/programs/ivt.inc).
        .align 32
        .global bench_exit_stub, bench_exit_stub_end
bench_exit_stub:
        mov r16 = ar.itc
        mov r17 = cr.iim
        ;;
        mov r18 = cr.iip
        mov r19 = cr.ipsr
        ;;
        mov ar.k4 = r16
        ;;
        mov ar.k3 = r17
        ;;
        mov ar.k2 = r18
        ;;
        mov ar.k1 = r19
        ;;
        mov r16 = ar.k6
        mov r17 = ar.k5
        ;;
        mov cr.iip = r16
        mov cr.ipsr = r17
        ;;
        mov cr.ifs = r0
        ;;
        rfi
        ;;
        .align 32
bench_exit_stub_end:

// Reached from bench_exit_stub: physical mode, PSR.ic = PSR.i = 0, bank 1,
// CFM = the job's frame.
        .align 32
        .global bench_resume
        .proc bench_resume
bench_resume:
        mov r2 = ar.k7
        ;;
        // The job's static GRs first, through r2 as the moving pointer.
        adds r2 = OUT_GR + 8, r2
        ;;
        st8 [r2] = r1, 16
        ;;
        .irp r, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15, r16, r17, r18, r19, r20, r21, r22, r23, r24, r25, r26, r27, r28, r29, r30, r31
        st8 [r2] = \r, 8
        ;;
        .endr
        mov r2 = ar.k7
        ;;
        mov r14 = pr
        ;;
        PUT r14, OUT_PR
        mov r14 = ar.k4
        ;;
        PUT r14, OUT_ITC1
        mov r14 = ar.k3
        ;;
        PUT r14, OUT_IIM
        mov r14 = ar.k2
        ;;
        PUT r14, OUT_IIP
        mov r14 = ar.k1
        ;;
        PUT r14, OUT_IPSR
#ifdef BENCH_WRAP_WAIT_IRQ
        // Test builds: PSR.i is clear and TPR masks the timer, so its
        // interrupt stays in cr.irr (vector 0xe8 = irr3 bit 40). Wait for it
        // (bounded by 2^32 ITC ticks) so the return always has one pending.
        mov r14 = ar.itc
        movl r15 = 1 << 32
        ;;
        add r14 = r14, r15
        ;;
1:      mov r16 = cr.irr3
        mov r17 = ar.itc
        ;;
        tbit.nz p6, p7 = r16, 40
        cmp.ltu p8, p9 = r17, r14
        ;;
(p6)    br.cond.sptk 2f
        ;;
(p8)    br.cond.sptk 1b
        ;;
2:
#endif
        // r32-r39 exist only if the job allocated them (guest gather: sof = 8).
        GET r14, CTX_NSTACKED
        cmp.eq p6, p7 = 0, r14
        ;;
(p6)    br.cond.dpnt.few 3f
        ;;
        adds r14 = OUT_STACKED, r2
        ;;
        .irp r, r32, r33, r34, r35, r36, r37, r38, r39
        st8 [r14] = \r, 8
        ;;
        .endr
3:      // Bank 0: store the job's r16-r31, restore the firmware's.
        adds r14 = OUT_BANK0, r2
        adds r15 = SAV_BANK0, r2
        bsw.0
        ;;
        .irp r, r16, r17, r18, r19, r20, r21, r22, r23, r24, r25, r26, r27, r28, r29, r30, r31
        st8 [r14] = \r, 8
        ;;
        ld8 \r = [r15], 8
        ;;
        .endr
        bsw.1
        ;;
        // Back to the firmware's backing store. flushrs writes whatever
        // the job left dirty to the job's store; the job's current frame
        // is then discarded by br.ret, which refills the caller's frame
        // from the firmware store (flushed by bench_run).
        flushrs
        ;;
        mov ar.rsc = 0
        ;;
        GET r14, SAV_BSPSTORE
        mov ar.bspstore = r14
        ;;
        GET r14, SAV_RNAT
        mov ar.rnat = r14
        ;;
        GET r14, SAV_IVA
        mov cr.iva = r14
        ;;
        GET r14, SAV_PTA
        mov cr.pta = r14
        ;;
        srlz.d
        ;;
        GET r14, SAV_DCR
        mov cr.dcr = r14
        ;;
        srlz.d
        ;;
        GET r14, SAV_TPR
        mov cr.tpr = r14
        ;;
        srlz.i
        ;;
        GET r1, SAV_GP
        GET r12, SAV_SP
        GET r13, SAV_TP
        GET r4, SAV_R4
        GET r5, SAV_R4 + 8
        GET r6, SAV_R4 + 16
        GET r7, SAV_R4 + 24
        GET r14, SAV_B0
        mov b0 = r14
        GET r14, SAV_B0 + 8
        mov b1 = r14
        GET r14, SAV_B0 + 16
        mov b2 = r14
        GET r14, SAV_B0 + 24
        mov b3 = r14
        GET r14, SAV_B0 + 32
        mov b4 = r14
        GET r14, SAV_B0 + 40
        mov b5 = r14
        GET r14, SAV_LC
        mov ar.lc = r14
        ;;
        GET r14, SAV_EC
        mov ar.ec = r14
        ;;
        GET r14, SAV_UNAT
        mov ar.unat = r14
        ;;
        GET r14, SAV_FPSR
        mov ar.fpsr = r14
        ;;
        GET r14, SAV_CCV
        mov ar.ccv = r14
        ;;
        GET r14, SAV_PFS
        mov ar.pfs = r14
        ;;
        GET r14, SAV_PR
        mov pr = r14, -1
        ;;
        // Back to the firmware's PSR through rfi (mov psr.l cannot set
        // PSR.it or PSR.bn): ipsr = the saved PSR{36:0} with PSR.bn = 1 and
        // PSR.i = 0. The break stub overwrote ar.k1-k4 and benchwrap.c puts
        // them back after the return; the firmware keeps its per-CPU
        // interrupt context in ar.k1, so no interrupt may be taken before
        // that. benchwrap.c sets PSR.i again.
        GET r14, SAV_PSR
        movl r15 = 1 << 44
        ;;
        or r14 = r14, r15
        ;;
#ifndef BENCH_WRAP_KEEP_PSR_I
        dep r14 = r0, r14, 14, 1
        ;;
#endif
        mov cr.ipsr = r14
4:      mov r15 = ip
        ;;
        adds r15 = 5f - 4b, r15
        ;;
        mov cr.iip = r15
        mov cr.ifs = r0
        ;;
        rfi
        ;;
5:      GET r14, SAV_RSC
        mov ar.rsc = r14
        ;;
        br.ret.sptk.many b0
        ;;
        .endp bench_resume
