/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test machine for the unit tests: guest RAM,
 * region compilation (form, lift, optimize with the
 * verifier after every pass), and a minimal dispatcher that runs regions
 * with the IR evaluator and everything else with interp_run(v, 1), with
 * in-place captures after every materialization and their comparison
 * against an interp_run reference at the same retired count.
 * The harness (harness.h) replaces this for the generated-code tests.
 */
#ifndef IA64VMM_TESTS_TIER1_UM_MACHINE_H
#define IA64VMM_TESTS_TIER1_UM_MACHINE_H
#include "engine/tier1/form.h"
#include "tests/tier1/ir_eval.h"
#include "tests/tier1/ia64_enc.h"

/* The machine's RAM, disjoint from the other suites' regions: code, IVT, data. */
#define UM_RAM_BASE UINT64_C(0x800000) /* clear of harness's harness map (harness.h) */
#define UM_RAM_SIZE 0x100000u
#define UM_CODE UM_RAM_BASE
#define UM_IVT (UM_RAM_BASE + 0x8000) /* 32 KiB aligned (cr.iva) */
#define UM_DATA (UM_RAM_BASE + 0x10000)
#define UM_DATA_SIZE 0x40000u

#define UM_CHECK(c)                                                                                \
    do {                                                                                           \
        if (!(c))                                                                                  \
            um_fail(__FILE__, __LINE__, #c);                                                       \
    } while (0)
_Noreturn void um_fail(const char *file, unsigned line, const char *what);

void um_machine_init(void);
uint8_t *um_ram(uint64_t gpa); /* host pointer of a machine RAM address */
/* Copies code into RAM (notifying the code cache). */
void um_load(uint64_t gpa, const void *bytes, size_t n);
/* Writes bundles at gpa. */
void um_emit(uint64_t gpa, const struct ienc_bundle *b, unsigned n);
/* Loads an image file (assembled .S) at gpa. */
void um_load_file(uint64_t gpa, const char *path);
/* A vCPU in physical mode at ip with PSR.ic = 1, icount ITC, cr.iva = UM_IVT
 * whose every bundle branches to itself (interruptions spin, retiring). */
struct vcpu *um_vcpu(uint64_t ip);
/* Instruction translation on (PSR.it = 1, data stays physical): ITR
 * entries 0-2 map the 4 MiB page holding the machine's RAM at VRN 0 (identity, for
 * the IVT), 1 and 2 (aliases of the same physical code, gather-alias). */
void um_virtual_code(struct vcpu *v);
/* Sets a logical GR (through the current CFM renaming) / predicate. */
void um_gr(struct vcpu *v, unsigned reg, uint64_t value, bool nat);
void um_pr(struct vcpu *v, unsigned reg, bool value);
/* CFM with sof, sol, sor (in registers, multiple of 8) and rename bases. */
uint64_t um_cfm(unsigned sof, unsigned sol, unsigned sor, unsigned rrb_gr, unsigned rrb_pr);

struct um_region {
    struct t1_form *form;
    struct t1_ir *ir;
    unsigned max_path, guards;
    bool valid;
};
/* Forms, lifts and optimizes the region at v's ip/slot, verifying after
 * every pass. On a lift limit the region is re-formed shorter. Returns the
 * status; err gets the reason. */
enum t1_compile_status um_compile(struct vcpu *v, struct um_region *r, char *err, size_t n);
void um_region_free(struct um_region *r);
/* Entry checks the dispatcher makes (contract, NaT guard set,
 * code pages, budget >= max_path). */
bool um_can_enter(struct um_region *r, struct vcpu *v, uint64_t budget);

struct um_capture {
    struct vcpu v;
    uint64_t data_hash;
    uint32_t point;
    bool forced;
};
struct um_run {
    const struct ir_eval_hooks *hooks;
    struct um_capture *captures; /* NULL: none */
    unsigned max_captures, ncaptures;
    unsigned entries, guard_rejects, exits[4], lower_steps, forced;
    uint64_t tier1_retired;
};
/* Runs v for exactly budget instructions unless an exit intervenes; region
 * r (may be NULL) is entered whenever um_can_enter allows. Returns the
 * outermost exit, like interp_run. */
struct vcpu_exit um_dispatch(struct um_region *r, struct vcpu *v, uint64_t budget,
                             struct um_run *run);

/* Snapshots of vCPU + machine RAM. */
struct um_snapshot {
    struct vcpu v;
    uint8_t *ram;
};
void um_snapshot_take(struct um_snapshot *s, const struct vcpu *v);
void um_snapshot_restore(const struct um_snapshot *s, struct vcpu *v);
void um_snapshot_free(struct um_snapshot *s);
uint64_t um_data_hash(void);

/* Restores the snapshot into ref, runs the reference with interp_run one
 * instruction at a time, compares every capture at its retired count
 * (in place) and the final state after budget instructions with
 * the outer exits. Returns the number of differences (first reported). */
unsigned um_compare(const struct um_snapshot *snap, struct vcpu *ref, const struct um_run *run,
                    const struct vcpu *got, const struct vcpu_exit *got_exit, uint64_t budget,
                    const char *label);
#endif
