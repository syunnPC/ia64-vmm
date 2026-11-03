/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only IR evaluator: the
 * one place outside the interpreter that implements the IR ops' semantics,
 * written independently of ia64/integer.c so that ir_eval vs interp_run
 * checks the lifting.
 *
 * It executes one region entry the way the generated code will: LIVEINs
 * read the homes at entry; block charges are subtracted from a raw budget
 * counter at block ends; a back-edge is a checkpoint that exits when the
 * counter is below max_path or the event hook asks; loads and
 * stores are native only on physical-mode, aligned, whole-access GM_RAM
 * (stores: no code page), else the region exits at the site before effect;
 * HELPER writes its read set to the homes, sets ip/slot and calls
 * ia64_execute. At the exit point the frame is applied to the
 * homes, ip/slot set and v->retired advanced by consumed(P).
 */
#ifndef IA64VMM_TESTS_TIER1_IR_EVAL_H
#define IA64VMM_TESTS_TIER1_IR_EVAL_H
#include "engine/tier1/ir.h"

struct ir_eval_hooks {
    /* Site and helper points, before the access/call: true = exit there
     * (an forced-exit-like forced exit; occurrence counts from 1 per point). */
    bool (*force)(void *arg, uint32_t point, uint64_t occurrence);
    /* Every checkpoint after the budget test: true = event exit. */
    bool (*event)(void *arg, uint64_t iteration);
    void *arg;
};

struct ir_eval_result {
    uint32_t point;      /* the materialized point */
    uint64_t consumed;   /* consumed(P) */
    uint64_t iterations; /* back-edges taken */
    bool forced;
    char error[200]; /* evaluator-detected invariant violation */
};

/* Precondition: v is at the region entry and budget >= max_path. Returns
 * false (with out->error) if an invariant of the IR is violated at run
 * time (e.g. a NATCHK of a set NaT, an unknown op). */
bool ir_eval_run(const struct t1_ir *ir, struct vcpu *v, uint64_t budget,
                 const struct ir_eval_hooks *hooks, struct ir_eval_result *out);
#endif
