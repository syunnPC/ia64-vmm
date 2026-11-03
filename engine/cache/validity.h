/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_CODE_VALIDITY_H
#define IA64VMM_CODE_VALIDITY_H
#include "ia64/state.h"

#define CODE_GRANULES 256u
struct PageValidity;
/* One mutation lock. Helpers must not wait for guest instruction
 * boundaries while holding it; host shootdown acknowledgements run in IRQs. */
void code_lock(void);
void code_unlock(void);
bool code_lock_held(void);
/* Fixed host executor/helper counter slot; no shared hot counter writes. */
unsigned code_counter_index(void) __attribute__((const)); /* constant per thread */
/* Optional lock-wait instrumentation, enabled by TIER0_STATS. */
struct code_lock_stats {
    uint64_t acquisitions, wait_ticks;
};
void code_lock_get_stats(struct code_lock_stats *out);
uint64_t code_epoch(void);
/* Advance the code lifetime epoch for retirement or registration changes;
 * code_lock held. */
uint64_t code_retire_epoch(void);
void code_quiescent(struct vcpu *v);
/* Lock-free sequentially consistent read of the code lifetime epoch: every
 * retirement stamped before it in that order is at or below it. Executor
 * registration changes also advance it, invalidating stopped code pointers. */
uint64_t code_epoch_sample(void);
bool code_epoch_reached(uint64_t stamp);
/* code_lock held: the smallest quiescent epoch of the registered running
 * executors (UINT64_MAX: none). A stamp at or below it is reached; one read
 * serves a whole reclamation pass. */
uint64_t code_epoch_horizon(void);
void code_register_vcpu(struct vcpu *v);
void code_unregister_vcpu(struct vcpu *v);
/* Flush must retain pooled records while registered executors can read stays. */
bool code_executors_registered(void);
/* Private lifecycle query; a registered executor uses its epoch pin. */
bool code_vcpu_is_registered(const struct vcpu *v);
/* One link per source page: a decoded-page bitmap or a generated region.
 * Multi-page regions must attach a separate link to every source page. */
struct code_dependency {
    _Atomic(struct PageValidity *) page;
    struct code_dependency *next, **previous;
    void (*invalidate)(struct code_dependency *);
    uint64_t mask[4];
    /* Callback receives the changed subset; full retirement uses all bits. */
    uint64_t invalidated[4];
};
/* Fetch translation of a region entry; a change makes the page leave the
 * stay. */
struct code_fetch {
    uint64_t psr, rr, pta, pte;
};
/* PSR fields (SDM Vol.2 Table 3-2) that no instruction fetch translation
 * reads and no generated region depends on: up, ac, mfl, mfh, i, dfl, dfh,
 * sp, pp, di, si and db are read at run time by helpers, the interpreter and
 * the engine loop tops; ri repeats the slot of the executing instruction.
 * Fetch contexts compare PSR without them; Tier0 region keys omit them. */
#define CODE_PSR_RUNTIME                                                                           \
    ((UINT64_C(0xf) << 2) | (UINT64_C(1) << 14) | (UINT64_C(0x7f) << 18) | (UINT64_C(3) << 41))
void code_dependency_attach(gpa_t page, struct code_dependency *dependency);
void code_dependency_extend(struct code_dependency *dependency, unsigned granule);
/* A stay holds at most this many source pages, also the
 * bound of a Tier1 region's page set. */
#define CODE_STAY_PAGES 4u
/* Entry of a region whose source pages hold dependencies[0..n-1], the entry
 * page first: continues the stay, adding the pages it
 * does not hold, and compares the bundles not yet compared in it. fetch
 * (required) is the entry's fetch translation. false: not entered (a
 * mismatch retired a dependency, or one is detached). code_dependency_enter:
 * one page. */
bool code_dependencies_enter(struct vcpu *v, struct code_dependency *dependencies, unsigned n,
                             const struct code_fetch *fetch);
bool code_dependency_enter(struct vcpu *v, struct code_dependency *dependency,
                           const struct code_fetch *fetch);
/* A one-page entry's stay after a successful entry: the page, its
 * admission to the vCPU's stay (unique per vCPU index; its low two bits name
 * the stay's record of the page) and the generation that record holds.
 * While the stay continues, the record holds that admission (the page has
 * not left the stay) and the page that generation, the record only gains
 * compared bundles and keeps its fetch translation, so the next entry of the
 * same dependency needs neither the page search nor the fetch and mask
 * comparisons. A zero hint matches nothing. */
struct code_stay_hint {
    const struct PageValidity *page;
    uint64_t admission, generation;
};
/* true: entered through the hint, as code_dependency_enter in that stay
 * would; false: use code_dependency_enter_hint. */
bool code_dependency_reenter(struct vcpu *v, const struct code_dependency *dependency,
                             const struct code_stay_hint *hint);
/* code_dependency_enter that leaves the entry's hint (zero if not entered). */
bool code_dependency_enter_hint(struct vcpu *v, struct code_dependency *dependency,
                                const struct code_fetch *fetch, struct code_stay_hint *hint);
/* The hint of a dependency entered last in v's stay (zero if none). */
void code_stay_hint_take(const struct vcpu *v, const struct code_dependency *dependency,
                         struct code_stay_hint *hint);
/* A read-only description of one already validated admission. The dependency
 * mask must remain immutable, and its owner must remain pinned while a
 * witness is used. Only registered executors receive witnesses: page metadata
 * words then survive quiescence, and their generation rejects reuse. Discard
 * witnesses before unregistering or stopped teardown.
 *
 * Generated reentry must check owner, v's entry token and mmu_iseq, fetch
 * equality (ignoring CODE_PSR_RUNTIME), and every exported predicate before
 * touching recency. Shared page/epoch predicates require acquire loads;
 * admission, clock and entered belong to this executor. entry_tick uses
 * relaxed atomic accesses. Baseline AMD64 loads provide acquire ordering.
 * The region's execution/lifetime guards remain the caller's responsibility. */
struct code_stay_witness {
    const struct vcpu *owner;
    const _Atomic(struct PageValidity *) *page_word;
    const struct PageValidity *page;
    const _Atomic(bool) *ready_word;
    const uint64_t *epoch_word;
    const _Atomic(uint64_t) *generation_word;
    const uint64_t *admission_word;
    uint64_t epoch, generation, admission, token, iseq;
    struct code_fetch fetch;
    uint64_t *clock, *entered;
    _Atomic(uint64_t) *entry_tick;
};
/* No admission or comparison is performed. Expected epoch/generation come
 * from the validated stay; false leaves a zero witness. */
bool code_stay_witness_take(const struct vcpu *v, const struct code_dependency *dependency,
                            const struct code_fetch *fetch, struct code_stay_witness *out);
/* Reference implementation of generated reentry, including recency updates.
 * false: the caller must use the ordinary validation/entry path. */
bool code_stay_witness_reenter(struct vcpu *v, const struct code_stay_witness *witness,
                               const struct code_fetch *fetch);
/* Entries with at most two dependencies. Only page[0] needs the
 * stay's fetched entry context; each page needs its full immutable mask.
 * Every page's fetch field binds the group's entry context. The witness
 * lifetime and load-order contract above applies to every member.
 * The caller must separately prove current fetch translation/permissions
 * for every virtual page before taking or reentering a group. Source
 * validation and the entry context alone do not establish that proof. */
#define CODE_STAY_WITNESS_PAGES 2u
struct code_stay_witness_group {
    unsigned n;
    struct code_stay_witness page[CODE_STAY_WITNESS_PAGES];
};
/* No admission or comparison; failure leaves the whole group zero. */
bool code_stay_witness_group_take(const struct vcpu *v, const struct code_dependency *dependencies,
                                  unsigned n, const struct code_fetch *fetch,
                                  struct code_stay_witness_group *out);
/* Check every member before updating recency. Then update entered/clock for
 * every dependency in order, including aliases, and finally touch all pages,
 * as code_dependencies_enter does. Failure updates no recency. */
bool code_stay_witness_group_reenter(struct vcpu *v, const struct code_stay_witness_group *group,
                                     const struct code_fetch *fetch);
/* Stays: pages admitted; whole stays ended by another
 * owner or a cleared entry generation (engine_run return, interruption,
 * device access), by a storage-epoch change, by an instruction-side MMU
 * change; pages that left alone after a validity event or with another
 * fetch translation, or as the least recently entered; bundles compared. */
struct code_stay_stats {
    uint64_t page, reset, event, epoch, fetch, bundles_compared, mmu, evicted;
};
void code_stay_get_stats(struct code_stay_stats *out);
/* Publication of an exact mask built from bundles read at generation. */
bool code_dependency_publish(struct vcpu *v, gpa_t gpa, uint64_t generation,
                             const struct code_dependency *dependency);
bool code_validity_active(void);
bool code_page_is_tracked(gpa_t page);
/* One acquire load of a bitmap word, with no record/hash walk. */
bool code_page_tracked_fast(gpa_t page);
/* The tracked-page bitmap span holding page (page numbers first ..
 * first + pages - 1; bit n of bits is page first + n), for an inline test
 * that loads the page's word with acquire semantics. False while code
 * validity is inactive or no span holds the page. The bits stay valid until
 * code_validity_configure or code_validity_flush (stopped executors), which
 * reset the executors' cached spans (tier0_code_spans_reset). */
struct code_page_span {
    uint64_t first, pages;
    const uint64_t *bits;
};
bool code_page_span(gpa_t page, struct code_page_span *out);
bool code_page_has_dependencies(gpa_t page);
void code_bundle_mask(gpa_t page, gpa_t gpa, uint64_t len, uint64_t mask[4]);
void code_dependency_remove(struct code_dependency *dependency);
struct ia64_result code_page_enter(struct vcpu *v, gpa_t gpa, bool region_entry);
struct ia64_result code_source_read(struct vcpu *v, gpa_t gpa, uint64_t *low, uint64_t *high,
                                    uint64_t *generation);
/* code_source_read without a vCPU:
 * the region comes from gm_lookup instead of ia64_physical_region (no
 * memory hook, no per-vCPU region cache). For compilers that read only a
 * compile request (engine/tier1/tier1.h), on any thread. */
struct ia64_result code_source_read_physical(gpa_t gpa, uint64_t *low, uint64_t *high,
                                             uint64_t *generation);
bool code_source_publish(struct vcpu *v, gpa_t gpa, uint64_t generation);
uint64_t code_source_generation(gpa_t gpa);
bool code_page_has_snapshot(gpa_t page);
void code_validity_invalidate(gpa_t gpa, uint64_t len);
void code_validity_flush(void);
/* Code-page tracking. TRACKED runs in
 * prepare() before the epoch latch and before the snapshot is read;
 * UNTRACKED after the final dependency callback removed its coverage. */
enum code_track_event { CODE_PAGE_TRACKED, CODE_PAGE_UNTRACKED };
typedef void (*code_track_hook_fn)(gpa_t page, enum code_track_event event);
/* Stopped executors only. NULL (default) disables. The hook runs under
 * code_lock on the tracking thread (an executor or a helper), must not call
 * back into engine/cache, and must have write-protected every alias of the
 * page when it returns. */
void code_validity_set_track_hook(code_track_hook_fn hook);

#ifdef IA64VMM_HOSTED
/* Deterministic DMA races; hooks write backing RAM without notification. */
/* CODE_TEST_STAY: a region entry's stay was continued or begun, before its
 * bundles are compared (a peer's event may end the stay there).
 * WITNESS/GROUP: captured predicates, before the final live check. */
enum code_test_stage {
    CODE_TEST_SNAPSHOT,
    CODE_TEST_DECODE,
    CODE_TEST_PUBLISH,
    CODE_TEST_SEAL,
    CODE_TEST_STORE,
    CODE_TEST_CLOCK,
    CODE_TEST_STAY,
    CODE_TEST_WITNESS,
    CODE_TEST_WITNESS_GROUP
};
typedef void (*code_test_hook_fn)(gpa_t page, enum code_test_stage stage);
void code_validity_set_test_hook(code_test_hook_fn hook);
void code_validity_test_hook(gpa_t page, enum code_test_stage stage);
#endif
#endif
