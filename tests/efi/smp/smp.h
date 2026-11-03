/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Layout shared by smp.c and entry.S.
 *
 * entry.S is the OS_BOOT_RENDEZ handler registered with SAL. It stores
 * the registers an AP arrives with (SAL 245359-005 §3.2.6) into the
 * 128-byte record of its LID id before anything else changes them, then branches
 * to smp_ap_main (smp.c) with b0, gp, r12 and ar.pfs untouched. */
#ifndef SMP_H
#define SMP_H

#define SMP_RAW_PSR 0 /* mov r=psr: PSR{36:35} and PSR{31:0} */
#define SMP_RAW_GP 8
#define SMP_RAW_SP 16
#define SMP_RAW_B0 24
#define SMP_RAW_BSP 32
#define SMP_RAW_BSPSTORE 40
#define SMP_RAW_RSC 48
#define SMP_RAW_PFS 56
#define SMP_RAW_TPR 64
#define SMP_RAW_LID 72
#define SMP_RAW_ITC 80 /* ar.itc at handler entry */
#define SMP_RAW_IVA 88
#define SMP_RAW_ENTRIES 96 /* handler entries counted by the stub */
#define SMP_RAW_SIZE 128
#define SMP_RAW_SHIFT 7
#define SMP_RAW_SLOTS 256 /* indexed by the LID id byte, cr.lid{31:24} */

#ifndef __ASSEMBLER__
struct smp_raw {
    UINT64 psr, gp, sp, b0, bsp, bspstore, rsc, pfs, tpr, lid, itc, iva, entries;
    UINT64 pad[3];
};
_Static_assert(sizeof(struct smp_raw) == SMP_RAW_SIZE &&
                   __builtin_offsetof(struct smp_raw, gp) == SMP_RAW_GP &&
                   __builtin_offsetof(struct smp_raw, sp) == SMP_RAW_SP &&
                   __builtin_offsetof(struct smp_raw, b0) == SMP_RAW_B0 &&
                   __builtin_offsetof(struct smp_raw, bsp) == SMP_RAW_BSP &&
                   __builtin_offsetof(struct smp_raw, bspstore) == SMP_RAW_BSPSTORE &&
                   __builtin_offsetof(struct smp_raw, rsc) == SMP_RAW_RSC &&
                   __builtin_offsetof(struct smp_raw, pfs) == SMP_RAW_PFS &&
                   __builtin_offsetof(struct smp_raw, tpr) == SMP_RAW_TPR &&
                   __builtin_offsetof(struct smp_raw, lid) == SMP_RAW_LID &&
                   __builtin_offsetof(struct smp_raw, itc) == SMP_RAW_ITC &&
                   __builtin_offsetof(struct smp_raw, iva) == SMP_RAW_IVA &&
                   __builtin_offsetof(struct smp_raw, entries) == SMP_RAW_ENTRIES,
               "entry.S record layout");
#endif
#endif
