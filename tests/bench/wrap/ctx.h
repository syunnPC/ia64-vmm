/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Layout of the wrapper context shared by benchwrap.c and enter.S: the
 * job's entry state (written by C), the firmware state enter.S saves and
 * restores around the job, and the job's final state. Plain offsets so
 * the assembler can use them; benchwrap.c checks them against its struct.
 */
#ifndef IA64VMM_TESTS_BENCH_WRAP_CTX_H
#define IA64VMM_TESTS_BENCH_WRAP_CTX_H

/* Job entry state (tests/tier1/programs.c guest_setup / vcpu_reset). */
#define CTX_GR 0x000    /* gr[0..31], bank 1 (gr[0] unused) */
#define CTX_BANK0 0x100 /* bank-0 r16..r31 */
#define CTX_BR 0x180    /* b0..b7 */
#define CTX_PR 0x1c0
#define CTX_IP 0x1c8
#define CTX_PSR 0x1d0
#define CTX_RSC 0x1d8
#define CTX_FPSR 0x1e0
#define CTX_LC 0x1e8
#define CTX_EC 0x1f0
#define CTX_PFS 0x1f8
#define CTX_UNAT 0x200
#define CTX_CCV 0x208
#define CTX_IVA 0x210
#define CTX_PTA 0x218
#define CTX_DCR 0x220
#define CTX_NSTACKED 0x228 /* 0 or 8: capture r32..r39 after the run */
#define CTX_BSPSTORE 0x230 /* ar.bspstore = ar.bsp at entry (CFM.sof = 0) */
#define CTX_RNAT 0x238

/* Firmware state saved by enter.S. */
#define SAV_GP 0x240
#define SAV_SP 0x248
#define SAV_TP 0x250
#define SAV_R4 0x258 /* r4..r7 */
#define SAV_B0 0x278 /* b0..b5 */
#define SAV_PFS 0x2a8
#define SAV_LC 0x2b0
#define SAV_EC 0x2b8
#define SAV_UNAT 0x2c0
#define SAV_FPSR 0x2c8
#define SAV_RSC 0x2d0
#define SAV_CCV 0x2d8
#define SAV_PR 0x2e0
#define SAV_PSR 0x2e8
#define SAV_IVA 0x2f0
#define SAV_PTA 0x2f8
#define SAV_DCR 0x300
#define SAV_TPR 0x308
#define SAV_BANK0 0x310    /* bank-0 r16..r31 */
#define SAV_BSPSTORE 0x390 /* after flushrs (= ar.bsp) */
#define SAV_RNAT 0x398

/* Job final state. */
#define OUT_GR 0x400 /* gr[0..31] (gr[2] is the context pointer, not captured) */
#define OUT_BANK0 0x500
#define OUT_STACKED 0x580 /* r32..r39 when CTX_NSTACKED = 8 */
#define OUT_PR 0x5c0
#define OUT_ITC0 0x5c8 /* ar.itc just before the rfi into the job */
#define OUT_ITC1 0x5d0 /* ar.itc, first instruction of the break vector */
#define OUT_IIM 0x5d8  /* cr.iim / cr.iip / cr.ipsr at the break */
#define OUT_IIP 0x5e0
#define OUT_IPSR 0x5e8
#define CTX_SIZE 0x600

/* Kernel registers the break-vector stub reads (enter.S saves k0-k7 in C). */
#define TPR_MMI 0x10000    /* cr.tpr.mmi: mask all maskable external interrupts */
#define BREAK_END 0x1ffffe /* tests/tier1/harness.h T1H_BREAK_END */
#endif
