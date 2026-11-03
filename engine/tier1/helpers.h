/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * In-region helper classes; helpers.c supplies one row per supported opcode.
 * Missing rows are native-lowered or end the region (form.h). Execute the
 * immutable rebased instruction after materializing reads, reload writes on
 * COMPLETE, and otherwise exit before effect.
 *
 * Read/write sets use canonical decoder operand slots (ia64/decode/operand.h)
 * and must include every dependency/effect. Every possible write belongs to W.
 * Helper-table fuzz checks state and guest-memory coverage. qp is an implicit
 * read evaluated by ia64_execute and is omitted from rows.
 */
#ifndef IA64VMM_TIER1_HELPERS_H
#define IA64VMM_TIER1_HELPERS_H
#include "engine/tier1/tier1.h"

enum t1_helper_class {
    /* Register only: mov r = ar.x and mov ar.x = r/imm for x not in {RSC,
     * BSP, BSPSTORE, RNAT, ITC, EFLAG}; mov r = b, mov b = r; mov pr = r,
     * mask and mov r = pr; integer SIMD (pshr, padd, psub, mux, czx); the
     * helper tnat. None changes CFM, PSR,
     * BOF, RR or a rotating base; privilege faults are static (CPL is in
     * the contract). */
    T1_HC_R = 1,
    /* FR integer: setf.sig/exp, getf.sig/exp, xma.l/h/hu, xmpy.*, fand,
     * for, fxor, fandcm, fmix*, fsxt*. PSR.dfl/dfh are contract constants
     * (a disabled FR makes the instruction a boundary); an FR write sets
     * PSR.mfl or mfh (T1_HL_PSR_MF); these forms raise no FP traps. */
    T1_HC_F,
    /* FP arithmetic, compares, classification, conversions and the other
     * FR transfers without a native form: ia64_hostfp (host execution where
     * bit-identical, else the soft-float) on FPSR's authoritative home. An
     * FP trap completes with its effects; the stub undoes them and exits
     * before effect, so the lower tier executes and delivers it. */
    T1_HC_A,
    /* Memory slow path: demoted sites, ld.s/.a/.c (zero-
     * entry ALAT), ld8.fill and st8.spill (UNAT), ldf/stf/ldfp, lfetch,
     * ld16/st16, cmpxchg, xchg, fetchadd. ia64_memory performs no partial
     * access, so faults are before effect. */
    T1_HC_M,
};

/* The canonical operand slot an element's register number comes from. */
enum t1_hslot { T1_HS_NONE, T1_HS_R1, T1_HS_R2, T1_HS_R3, T1_HS_P1, T1_HS_P2, T1_HS_B1, T1_HS_B2 };

/*
 * Where an element lives. GR/NaT, PR, LC, PFS, EC and BR are cached Tier1
 * elements: synchronize reads before the call and snapshot writes into
 * frames.
 * FPI caches FR significand/metadata and PSR_MF too; helper sets
 * synchronize all three through the same point frame and snapshots.
 */
enum t1_hloc {
    T1_HL_GR,     /* GR of the slot, value and NaT (stacked: frame-relative after BOF) */
    T1_HL_FR,     /* FR of the slot (T1_ELEMENT_FR) */
    T1_HL_PR,     /* predicate of the slot */
    T1_HL_PR_ALL, /* p1..p63 (mov pr = r, mask; mov r = pr) */
    T1_HL_LC,     /* ar.lc (T1_ELEMENT_LC) */
    T1_HL_PSR_MF, /* PSR.mfl/mfh (T1_ELEMENT_PSR_MF): written by FR writes */
    T1_HL_AR,     /* AR numbered by the slot: LC, PFS and EC cached (T1_ELEMENT_LC, _PFS, _EC),
                        the others authoritative homes */
    T1_HL_BR,     /* BR of the slot (T1_ELEMENT_BR) */
    T1_HL_UNAT,   /* ar.unat (ld8.fill, st8.spill) */
    T1_HL_FPSR,   /* ar.fpsr (forms that read or set its status fields) */
    T1_HL_CCV,    /* ar.ccv (cmpxchg) */
    T1_HL_MEM,    /* guest memory at the instruction's address */
    T1_HL_DTLB,   /* D-side TC: a VHPT walk may insert an entry (MMU log) */
    T1_HL_CSD,    /* (COV additive) ld16/st16/cmp8xchg16: authoritative home */
};
struct t1_hset {
    uint8_t loc, slot;
}; /* enum t1_hloc, enum t1_hslot (NONE for fixed places) */
#define T1_HSET_MAX 6u

/* Row flags: what the helper stub and its return path must handle. */
#define T1_HROW_FAULTS 1u /* may fault (before effect): exit at the helper point */
#define T1_HROW_STORE                                                                              \
    2u /* writes guest memory: re-check the executing region's
                                validity after the call, exit after completion if a
                                code write retired it */
#define T1_HROW_WALK                                                                               \
    4u /* may walk the VHPT and insert a TC entry: compare
                                mmu_seq and replay hostwin before resuming */
#define T1_HROW_ONCE                                                                               \
    8u                      /* MMIO, atomics, notifications: the single execution
                                happens inside the helper (class M (c)) */
#define T1_HROW_POSTINC 16u /* updates its base GR (slot r3) after the access */
#define T1_HROW_ITC                                                                                \
    32u /* reads ar.itc: v->retired = retired_entry + consumed(P)
                                before the call */

struct t1_helper_row {
    uint16_t opcode; /* Ia64Opcode */
    uint8_t cls;     /* enum t1_helper_class */
    uint8_t flags;   /* T1_HROW_* */
    uint8_t nread, nwrite;
    struct t1_hset read[T1_HSET_MAX], write[T1_HSET_MAX];
};

struct t1_insn;
/* Cached parts of the table sets; authoritative homes need no SSA slot.
 * Conditional writes read the old destination for a false qp; partial
 * PR-mask/ld.c writes retain it even with qp = p0. Every cached FR write
 * also reads both old halves, so reloading a preserved home stays exact.
 * alloc, which has no table row (it changes the register stack, which no
 * set names), reads ar.pfs and writes r1. */
unsigned t1_helper_elements(const struct t1_insn *ti, bool write, uint16_t *elements);
/* The row of an opcode; NULL: not an in-region helper. */
const struct t1_helper_row *t1_helper_row(Ia64Opcode op);
#endif
