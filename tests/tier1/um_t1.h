/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit-test machine: the RAM and vCPUs (um_machine.h) run by
 * the real Tier1 compiler and runtime -- generated code, host windows of
 * the hosted backend, the trap client -- with in-place captures from the
 * restore observer compared against an interp_run reference at the same
 * retired count, and the final state compared as a whole.
 */
#ifndef IA64VMM_TESTS_TIER1_UM_T1_H
#define IA64VMM_TESTS_TIER1_UM_T1_H
#include "tests/tier1/um_machine.h"
#include "engine/tier1/tier1.h"

/* One-time set-up: hosted window backend, trap client, code-page tracking
 * hook, a window instance, t1_init. */
void umt_init(void);
struct hostwin_vcpu *umt_hostwin(void);

/* The arming of one run (0 = none). */
struct umt_arm {
    uint32_t point; /* force: point id, UINT32_MAX = none */
    uint64_t occurrence;
    enum t1_force_action action;
    t1_before_resolve_fn before;
    void *before_arg;
    bool corrupt;    /* wrong-old-value: corrupt instead of force ... */
    bool force_too;  /* ... or as well as forcing `point` */
    uint32_t cpoint; /* corrupt point/occurrence when force_too */
    uint64_t coccurrence;
    uint16_t element;
    uint64_t mask;
    /* Called after the restore and arming, right before t1_run. */
    void (*before_run)(struct t1_vcpu *tv, struct vcpu *v, void *arg);
    void *run_arg;
    bool resume_requested; /* continue after VCPU_EXIT_DEBUG exits until the budget is used */
    bool final_only;       /* compare the end state only (no in-place captures) */
};

struct umt_run {
    struct um_run run; /* captures (in place), counts */
    unsigned records, retries, materialized, forced, traps, misses;
    struct vcpu_exit exit;
    unsigned nexits, resumed;
};

/* Compiles the region at v's ip/slot with the given flags (pin optional);
 * returns its id, 0 on failure (with the reason printed). */
uint32_t umt_compile(struct vcpu *v, uint32_t flags, uint16_t pin_element, uint8_t pin_reg,
                     struct t1_compile_report *rep, const char *label);
/* Restores snap into v, arms, runs t1_run(budget) and compares the captures
 * and the end state with interp_run from the same snapshot. Returns the
 * number of differences (0 = equal); fails the test unless expect_diff. */
unsigned umt_case(struct vcpu *v, struct vcpu *ref, const struct um_snapshot *snap, uint32_t flags,
                  uint64_t budget, uint32_t region, const struct umt_arm *arm, struct umt_run *out,
                  bool expect_diff, const char *label);
struct t1_vcpu *umt_tv(struct vcpu *v);
void umt_release(struct vcpu *v);
/* Decodes all of a region's code (body and
 * stubs): false (reported under label) if an instruction addresses a
 * stacked GR/NaT home R12-relative; *image = its R14-relative loads from
 * the entry image (exec.h frame_in). */
bool umt_no_stacked_home(const uint8_t *code, size_t size, const char *label, unsigned *image);
#endif
