/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * In-place restoration comparison, harness side. The restore observer captures the comparison state right
 * after t1_materialize() -- the vCPU by value, one hash per 4 KiB page of
 * the test RAM, the device-log and store-log lengths -- and the harness
 * later compares every capture with the reference stopped at the same
 * dynamic instruction boundary: the reference starts from the same
 * snapshot and is stepped (reference.h) until ref->retired equals the
 * capture's retired count, applying the same injection schedule. Running
 * the reference inside the callback is not used: both vCPUs share the
 * global decoded and validity caches.
 *
 * Comparison phase per capture: strict (forced exits, checkpoints, side
 * exits, helper exits), or, for natural guest faults (translated), after
 * applying to the reference the same ia64_translate(gva, IA64_REF_DATA,
 * READ or WRITE, cpl, non-speculative) that Tier1's hostwin_resolve ran.
 *
 * Logs are append-only, so the in-place comparison checks their lengths;
 * the final comparison (results.h) checks their contents.
 */
#ifndef IA64VMM_TESTS_TIER1_OBSERVE_H
#define IA64VMM_TESTS_TIER1_OBSERVE_H
#include "tests/tier1/snapshot.h"
#include "tests/tier1/schedule.h"

struct t1_vcpu;
struct t1_restore_record;

struct observe_capture {
    uint64_t retired;
    bool translated, write; /* natural guest fault: phase after translation */
    /* (product dispatch) Taken at the region's entry boundary
     * (no instruction retired since the entry): the dispatcher had run the
     * fetch translation of the IP before entering -- Tier0's loop on the
     * product path, which with CFLE set performs the mandatory RSE loads
     * (SDM Vol.2 §6.8) and may insert an I-TLB entry -- so the reference
     * applies the same fetch translation before the comparison. */
    bool at_entry;
    uint64_t gva;
    uint32_t region, point; /* diagnostics */
    uint64_t occurrence;
    struct vcpu v;
    uint64_t hash[T1H_RAM_PAGES];
    size_t device_n, store_n;
};

struct observer {
    struct observe_capture *c;
    unsigned n, cap;
    unsigned dropped;       /* records without materialization */
    struct schedule *sched; /* pending injections resolved at captures */
};

void observe_init(struct observer *o, unsigned cap, struct schedule *sched);
void observe_free(struct observer *o);
void observe_reset(struct observer *o);
/* Captures *v as materialized now (v bound to its run logs). Resolves the
 * boundary of T1H_AT_PENDING schedule entries (DMA from a before-resolve
 * callback, which runs before t1_materialize) to v->retired. */
void observe_capture(struct observer *o, const struct vcpu *v, bool translated, uint64_t gva,
                     bool write, uint32_t region, uint32_t point, uint64_t occurrence);
/* The last capture was taken at its region's entry boundary. */
void observe_mark_entry(struct observer *o);
/* : the t1_restore_fn (engine/tier1/tier1.h) to register with
 * t1_test_set_restore_observer(observe_t1_restore, observer). */
void observe_t1_restore(struct t1_vcpu *tv, const struct vcpu *v, const struct t1_restore_record *r,
                        void *arg);
/* Restores ref from snap, steps it through every capture in order and
 * compares there. ref must be bound to logs with the same stores_on as the
 * run under test. Returns the number of captures that differ (details on
 * stderr under label); a capture the reference cannot reach counts too. */
unsigned observe_verify(const struct observer *o, const struct snapshot *snap, struct vcpu *ref,
                        struct schedule *sched, const char *label);
#endif
