/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-MMU windows expose guest RAM to generated AMD64 loads/stores:
 * host_access = gva + delta only while (gva >> w) == tag. Contracts and context
 * diffs cover RID/CPL/PSR/PKR; low bits never select mappings. Host faults
 * return control; ia64_translate determines guest exceptions in SDM order.
 * Host A/D bits never replace guest A/D. Classify GPAs with gm_lookup;
 * this interface has no Tier1 dependency.
 */
#ifndef IA64VMM_HOSTWIN_H
#define IA64VMM_HOSTWIN_H
#include "ia64/state.h"
#include "engine/cache/validity.h"
/* Forward reference to the validity.h enum code_track_event (a GNU C
 * extension; C11 has no incomplete enum types). */
enum code_track_event;

#define HOSTWIN_MAX_SLOTS 8u
#define HOSTWIN_TAG_INVALID UINT64_MAX
#define HOSTWIN_FLUSH_ALL UINTPTR_MAX
/* A (tag, delta) pair; host_access = gva + delta is valid iff (gva >> w) == tag.
 * hostwin_bind writes only the caller's temporary; the caller owns its caches. */
struct hostwin_tag {
    uint64_t tag, delta;
};

/*
 * Backends: tests/tier1/hostwin_linux.c and host/amd64/hostwin_amd64.c.
 * Translation/fault reporting uses the original gva; backend operations take
 * 4 KiB page addresses (host_page=(gva+delta)&~4095, pa_page=pa&~4095).
 * Aligned accesses <=8 bytes cannot cross a page or window.
 */
enum hostwin_map_status {
    HOSTWIN_MAP_OK,
    HOSTWIN_MAP_UNBACKED,
    HOSTWIN_MAP_ERROR,
    HOSTWIN_MAP_EXHAUSTED,
    HOSTWIN_MAP_WRONG_CPU
};
struct hostwin_backend {
    /* Reserve slots << w bytes, 2^w aligned, inaccessible, plus one poison page. */
    bool (*reserve)(void *ctx, unsigned w, unsigned slots, uintptr_t *base, uintptr_t *poison);
    void (*release)(void *ctx, uintptr_t base, unsigned w, unsigned slots);
    /*
     * Pages are 4 KiB aligned. UNBACKED means no backing; ERROR panics with
     * VMA statistics. EXHAUSTED reclaims this window and retries once; WRONG_CPU
     * is terminal. Stop the vCPU if mapping still cannot proceed.
     */
    enum hostwin_map_status (*map)(void *ctx, uintptr_t host_page, gpa_t pa_page, bool writable);
    /*
     * On false, no range guarantee remains. Escalate protect_ro failure to
     * page unmap, then whole-slot unmap; panic if that fails. Never retain a
     * stale/writable alias. Hosted operations can fail from VMA exhaustion.
     */
    bool (*unmap)(void *ctx, uintptr_t host_page,
                  uint64_t bytes); /* page aligned, 4 KiB multiple */
    bool (*protect_ro)(void *ctx, uintptr_t host_page);
    /* Make earlier unmap/protect visible to the CPU running this instance's
     * vCPU; sync = return only when done. Hosted: no-op. */
    void (*flush)(void *ctx, uintptr_t host_page_or_all, bool sync);
    /* Optional, hosts with a private root per host CPU: the context of host_cpu's root, NULL if there is
     * none. Reservations in different contexts may reuse one address range;
     * only host_cpu fills its context. Without it every instance shares ctx.
     * Stopped executors only. */
    void *(*cpu_context)(void *ctx, unsigned host_cpu);
};
/*
 * w is in the Tier1 contract: bare metal min(53,H-3), hosted default 40.
 * check enables full MMU-log completeness checks (both TR/TC sides, RR, PKR);
 * hosted harness only, unavailable in vmm.elf.
 */
struct hostwin_config {
    unsigned w, slots;
    bool check;
};
/* At this limit, a fill drops the instance's existing shadows. The hosted
 * backend reserves headroom for 2 * HOSTWIN_MAX_SHADOWS + 1 VMAs. */
#define HOSTWIN_MAX_SHADOWS 8192u
struct hostwin_vcpu; /* one per vCPU, owned by its executor; other threads
                          only through hostwin_code_page_event */

/*
 * Return the host backend with *ctx and installed trap path, or NULL
 * (registry.c default). Missing windows make engine=tier1 a setup error.
 * Stopped executors only.
 */
const struct hostwin_backend *hostwin_host_backend(void **ctx);

/* Stopped executors only; registers the instance for code-page events.
 * The product glue passes the context of the vCPU's host CPU where the
 * backend has cpu_context. */
struct hostwin_vcpu *hostwin_create(const struct hostwin_config *cfg,
                                    const struct hostwin_backend *backend, void *ctx);
/* A backend failure that stops the vCPU; static diagnostic, NULL otherwise. */
const char *hostwin_stop_detail(const struct hostwin_vcpu *hw);
void hostwin_destroy(struct hostwin_vcpu *hw);
unsigned hostwin_bits(const struct hostwin_vcpu *hw);
/* A never-mapped page inside the reservation; verification hooks
 * load from it to raise a real host fault. */
uintptr_t hostwin_poison_va(const struct hostwin_vcpu *hw);

/*
 * Synchronize shadows with v's MMU context without changing guest state.
 * Skip unchanged mmu_seq/PSR.dt,pk,cpl. Otherwise replay the log when its
 * unsigned distance fits IA64_MMU_LOG and contains no ALL; use a full diff
 * after overflow, ALL, create or resync. Changed RR/PKR/PSR drops all shadows;
 * changed D-side entries drop their sourced and newly covered shadows.
 * I-side changes need no shadow action. Compare values before dropping.
 *
 * Check mode also verifies unlogged changes and replay/full-diff agreement,
 * panicking on mismatch; full diffs restart the check snapshot. The same
 * update runs after hostwin_resolve access checks and memory helpers.
 */
void hostwin_enter(struct hostwin_vcpu *hw, const struct vcpu *v);
/* Whether unchanged-context entries may reuse the last synchronization.
 * Explicit completeness checks and a pending full diff require enter. */
bool hostwin_can_reenter(const struct hostwin_vcpu *hw);
/* Account entries that proved the synchronized context unchanged. */
void hostwin_note_reentries(struct hostwin_vcpu *hw, uint64_t entries);
/* Force a full diff after snapshot restore, which may reuse an mmu_seq
 * with different state. Called by t1_vcpu_state_restored. */
void hostwin_resync(struct hostwin_vcpu *hw);
/* Slot rebinding increments the epoch and invalidates cached (tag, delta)
 * pairs. Bind into a temporary; on an epoch change, clear all cached tags
 * and update the observed epoch before publishing it. Resolve never rebinds. */
uint64_t hostwin_bind_epoch(const struct hostwin_vcpu *hw);

/* HOSTWIN_SLOW: tags never bound -- physical tags with bit 63 set
 * (UC alias) or pa >= 2^50, virtual addresses with unimplemented bits.
 * Replacement is LRU among the slots. */
enum hostwin_status { HOSTWIN_OK, HOSTWIN_SLOW };
enum hostwin_status hostwin_bind(struct hostwin_vcpu *hw, const struct vcpu *v, uint64_t gva,
                                 struct hostwin_tag *out);
/*
 * Read the PA currently served by gva's bound slot/shadow; false if access
 * would miss or fault. Checked lockstep converts store records before any
 * C-side action can change shadows. No side effects.
 */
bool hostwin_window_pa(const struct hostwin_vcpu *hw, uint64_t gva, gpa_t *pa);

enum hostwin_fill { HOSTWIN_MAPPED, HOSTWIN_EXIT };
enum hostwin_exit_reason {
    HW_EXIT_GUEST_FAULT,
    HW_EXIT_ATTRIBUTE,
    HW_EXIT_NOT_RAM,
    HW_EXIT_PARTIAL_PAGE,
    HW_EXIT_PERMISSION_SPLIT,
    HW_EXIT_UNBACKED,
    HW_EXIT_CODE_PAGE_STORE,
    HW_EXIT_INJECTED,
    HW_EXIT_BACKEND /* terminal backend failure; never execute the access below */,
    HW_EXIT_STALE_SLOT /* defensive: the tag of gva is no longer bound */
};
/* Resolve in normal context using the original aligned gva and size 1/2/4/8.
 * Checks follow SDM priority; faults return to the lower tier for delivery.
 * A virtual miss translates once, including VHPT effects, then uses that
 * entry's lasting permissions (PSR.da=0). Only complete, host-aligned 4 KiB
 * WB RAM pages are mapped; tracked code pages remain read-only.
 * Injected, stale-slot and malformed accesses exit before translation.
 * Backend failure handling follows hostwin_backend's contracts. */
enum hostwin_fill hostwin_resolve(struct hostwin_vcpu *hw, struct vcpu *v, uint64_t gva,
                                  unsigned size, bool write, enum hostwin_exit_reason *why);

/* Code-page events from engine/cache; registered by the harness with
 * code_validity_set_track_hook. TRACKED: every alias of page in every
 * instance is read-only and flushed (sync) on return. */
void hostwin_code_page_event(gpa_t page, enum code_track_event event);
void hostwin_invalidate(struct hostwin_vcpu *hw);

/* Host trap plumbing; the client must be async-signal-safe. gpr[] uses AMD64
 * register numbers (0 = RAX ... 15 = R15); gpr[4] (RSP) must not change. */
struct hostwin_trap {
    uint64_t gpr[16], rip, rflags, fault_va;
    bool write;
};
typedef bool (*hostwin_trap_client_fn)(struct hostwin_trap *t);
void hostwin_set_trap_client(hostwin_trap_client_fn fn);
bool hostwin_trap(struct hostwin_trap *t);
/* host_va lies in an instance's reservation or poison page. Instances in
 * different contexts may share one range; the trap client takes the
 * faulting CPU's own. */
bool hostwin_owns(uintptr_t host_va);

/* binds excludes cache hits; rebinds replaces occupied slots. entry_diffs
 * counts changed D-side entries. probe_calls stays zero: translation supplies
 * permissions. Hosted shadows use at most 2 * live_mappings + 1 VMAs.
 * context_* counts updates, including those in resolve; check_diffs counts
 * completeness checks. snapshot_ticks requires IA64VMM_TIER1_STATS. */
struct hostwin_stats {
    uint64_t binds, rebinds, fills, fills_ro, exits[HW_EXIT_STALE_SLOT + 1];
    uint64_t context_full_drops, entry_diffs, shadow_drops, snapshot_ticks;
    uint64_t translate_calls, probe_calls, code_events, live_mappings;
    uint64_t context_unchanged, context_replays, context_full_diffs, check_diffs;
    /* Failed unmap/protect operations, map exhaustion and terminal map failures. */
    uint64_t backend_failures;
};
void hostwin_get_stats(const struct hostwin_vcpu *hw, struct hostwin_stats *out);
#ifdef IA64VMM_HOSTED
void hostwin_test_drop(struct hostwin_vcpu *hw, uint64_t gva); /* unmap the shadow page of gva */
void hostwin_test_inject(struct hostwin_vcpu *hw,
                         uint64_t nth); /* n-th resolve exits, HW_EXIT_INJECTED */
/* Every live shadow: its window address as a guest
 * address (tag << w plus the offset in the slot), PA page, writability. */
typedef void (*hostwin_test_shadow_fn)(uint64_t gva_page, gpa_t pa_page, bool writable, void *arg);
void hostwin_test_shadows(const struct hostwin_vcpu *hw, hostwin_test_shadow_fn fn, void *arg);
#endif
#endif
