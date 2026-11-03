/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Log completed test-data stores as (GVA, width, value) in per-vCPU completion
 * order; count completed integer RAM stores outside the data ranges too.
 * A memory_hook proxy performs RAM accesses, notifies code writes and logs
 * completions. hostwin classifies pages via gm_lookup, independently of it.
 *
 * The proxy receives GPAs. Before each reference step, storelog_predict()
 * decodes without side effects or the decoded cache to predict the GVA.
 * Use that prediction only while ip/slot still matches the stepped boundary;
 * otherwise derive the GVA from the callback's current instruction (including
 * a handler's first instruction or a Tier0 helper). Unknown writers are errors.
 * The base register is updated only after the access completes.
 *
 * The proxy keeps physical_region NULL, forcing Tier0 accesses through helpers.
 * Tests of Tier0's inline physical accesses must run without the store log.
 */
#ifndef IA64VMM_TESTS_TIER1_STORELOG_H
#define IA64VMM_TESTS_TIER1_STORELOG_H
#include "tests/tier1/harness.h"

struct t1_store_record;

/* Data pages (GPA ranges) whose accesses are logged; default: index array,
 * status word, output array and table (harness.h). */
struct storelog_range {
    gpa_t lo, hi;
};
void storelog_set_ranges(const struct storelog_range *ranges, unsigned n);
void storelog_default_ranges(void);

/* v must be bound (t1h_bind). Sets logs->stores_on; snapshot_restore
 * re-attaches after replacing *v. */
void storelog_attach(struct vcpu *v);
void storelog_detach(struct vcpu *v);

/* Called by the stepping runner around each interp_run(v, 1). */
void storelog_predict(struct vcpu *v);
void storelog_step_done(struct vcpu *v);

/* Appends one record (native Tier1 records at ). */
void storelog_append(struct t1h_logs *logs, uint64_t gva, uint64_t value, unsigned width);
/* The same with the bytes the store replaced and where they
 * came from (enum t1h_pre_kind). */
void storelog_append_pre(struct t1h_logs *logs, uint64_t gva, uint64_t value, unsigned width,
                         uint64_t pre, unsigned kind);
/* adapter: a t1_store_fn (engine/tier1/tier1.h) whose arg is the
 * struct t1h_logs of the Tier1 vCPU. */
void storelog_t1_store(const struct t1_store_record *r, void *arg);
/* The same for a vCPU (arg = struct vcpu *,
 * bound to its logs), logging only the stores the reference's proxy logs:
 * those whose physical address lies in the ranges (storelog_set_ranges),
 * found through the vCPU's D-TLB entry (side-effect free; physical mode:
 * the address without bit 63). The product-path engine compiles trace
 * regions too (stack, backing store); out-of-range stores are counted on
 * both sides. A store whose translation is gone is a harness error. */
void storelog_t1_store_ranged(const struct t1_store_record *r, void *arg);

/* Side-effect-free decode of the instruction at (ip, slot): no TLB walk,
 * no decoded-bundle cache. False if the fetch has no translation or the
 * bytes are outside direct memory. */
bool storelog_decode(const struct vcpu *v, uint64_t ip, unsigned slot, Ia64Instruction *out);
/* Width of a store opcode (st1..st8, .rel, st8.spill, stfs, stfd, stf8), else 0. */
unsigned storelog_store_width(Ia64Opcode opcode);
#endif
