/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Comparison relation for Tier1 tests and Tier0 harness self-tests:
 * engines and interpreter reference start from the same snapshot with the
 * same injection schedule, in icount mode (deterministic ITC).
 *
 * Compared:
 *   struct ia64_state       every member, excluding padding,
 *                           including the RSE incomplete-frame protocol state
 *                           (rse_missing, rse_cfle, rse_completion_*)
 *   v->retired, last_successful_bundle, clock_offset, timer_armed,
 *   timer_fired, in_service[4], requested_exit, and the per-CPU PAL TR
 *   buffer service state pal_tr_*, the native PAL service registers and
 *   pending PAL-based interruptions pal_*           exact
 *   guest RAM               all bytes of the test RAM (in-place captures:
 *                           one hash per 4 KiB page, the difference naming
 *                           the page)
 *   device effects          the test MMIO device's ordered transaction log
 *   completed stores        the ordered store log (verification builds):
 *                           (effective GVA, width, value truncated to width)
 *                           in completion order; ids and seq are diagnostics
 *   API outcome             the outermost struct vcpu_exit: reason, ip, slot,
 *                           info (detail is a diagnostic string); absent for
 *                           in-place observer captures
 * Exclusions and their reasons are listed below. VMM caches and statistics
 * outside struct vcpu are also excluded.
 *
 * Size/version assertions and compare_lists_complete() require these lists
 * to be reviewed when struct ia64_state or struct vcpu changes.
 */
#include "tests/tier1/compare.h"
#include "ia64vmm/string.h"
#include "core/log_internal.h"

_Static_assert(IA64_STATE_VERSION == 4u, "review the comparison lists for the new state version");
_Static_assert(sizeof(struct ia64_state) == 33584, "struct ia64_state changed: review the lists");
_Static_assert(sizeof(struct ia64_fr) == 16, "struct ia64_fr changed: review the lists");
_Static_assert(sizeof(struct ia64_tlb_entry) == 40,
               "struct ia64_tlb_entry changed: review the lists");
_Static_assert(sizeof(struct ia64_pal_state) == 64,
               "struct ia64_pal_state changed: review the lists");
_Static_assert(sizeof(struct vcpu) == 38280, "struct vcpu changed: review the lists");

/* ---- member lists ---------------------------------------------------- */

/* rse_missing .. rse_completion_demoted: the unloaded prefix of
 * the current frame, CFLE, and the br.ret completion deferred until the
 * mandatory loads finish (ia64/rse.c); architectural, compared exactly. */
#define STATE_SCALARS(X)                                                                           \
    X(version)                                                                                     \
    X(cpu_model)                                                                                   \
    X(pr)                                                                                          \
    X(psr)                                                                                         \
    X(cfm)                                                                                         \
    X(ip)                                                                                          \
    X(slot)                                                                                        \
    X(rse_bof)                                                                                     \
    X(rse_dirty)                                                                                   \
    X(rse_missing)                                                                                 \
    X(rse_cfle)                                                                                    \
    X(rse_completion_pending)                                                                      \
    X(rse_completion_psr)                                                                          \
    X(rse_completion_ip)                                                                           \
    X(rse_completion_slot)                                                                         \
    X(rse_completion_demoted)
#define STATE_ARRAYS(X)                                                                            \
    X(gr)                                                                                          \
    X(nat)                                                                                         \
    X(br)                                                                                          \
    X(ar)                                                                                          \
    X(cr)                                                                                          \
    X(bank_gr)                                                                                     \
    X(bank_nat)                                                                                    \
    X(rr)                                                                                          \
    X(pkr)                                                                                         \
    X(dbr)                                                                                         \
    X(ibr)                                                                                         \
    X(pmc)                                                                                         \
    X(pmd)                                                                                         \
    X(msr)                                                                                         \
    X(tc_next)
#define FR_MEMBERS(X)                                                                              \
    X(significand)                                                                                 \
    X(exponent)                                                                                    \
    X(sign)
#define TLB_MEMBERS(X)                                                                             \
    X(va)                                                                                          \
    X(pte)                                                                                         \
    X(itir)                                                                                        \
    X(rid)                                                                                         \
    X(page_shift)                                                                                  \
    X(valid)
#define PAL_MEMBERS(X)                                                                             \
    X(copy_addr)                                                                                   \
    X(interrupt_block_addr)                                                                        \
    X(io_block_addr)                                                                               \
    X(mc_save_addr)                                                                                \
    X(pmi_entry)                                                                                   \
    X(bus_disabled)                                                                                \
    X(proc_disabled)                                                                               \
    X(copy_valid)                                                                                  \
    X(mc_expected)
/* Exact vCPU state outside s includes guest-visible platform state:
 * pal_tr_* (PAL TR buffer MMIO), service_port (IA64VMM_SVC_PORT_SELECT),
 * and pal_* (native PAL service registers and pending interruptions). */
#define VCPU_COMPARED(X)                                                                           \
    X(retired)                                                                                     \
    X(last_successful_bundle)                                                                      \
    X(clock_offset)                                                                                \
    X(timer_armed)                                                                                 \
    X(timer_fired)                                                                                 \
    X(requested_exit)                                                                              \
    X(pal_tr_selector)                                                                             \
    X(pal_tr_buffer)                                                                               \
    X(pal_tr_valid)                                                                                \
    X(pal_tr_status)                                                                               \
    X(pal_tr_pending)                                                                              \
    X(service_port)                                                                                \
    X(pal_xr0)                                                                                     \
    X(pal_pale_pmi)                                                                                \
    X(pal_sale_pmi)                                                                                \
    X(pal_state_address)                                                                           \
    X(pal_state_pending)                                                                           \
    X(pal_state_valid)                                                                             \
    X(pal_event)                                                                                   \
    X(pal_pmi_vector)                                                                              \
    X(pal_pmi_pending)                                                                             \
    X(pal_expected)                                                                                \
    X(pal_init_pending)                                                                            \
    X(pal_mca_pending)
#define VCPU_COMPARED_ARRAYS(X)                                                                    \
    X(in_service)                                                                                  \
    X(pal_min_state)
/*
 * struct vcpu members excluded, and why:
 *   fault            last-fault diagnostic; the interpreter and Tier1 leave
 *                    it untouched, while Tier0 clears it
 *   timer_last       last ITC sampled by ia64_timer_poll; the interpreter
 *                    polls before every instruction, Tier1 only between
 *                    regions; the firing decision is compared through
 *                    timer_fired and cr.irr
 *   microtlb, tlb_index, timer_cache       derived lookup/poll caches
 *   physical_region, physical_page        lookup cache
 *   code_page, code_entry_generation      validator entry history (cache)
 *   trace, trace_count, trace_used, trace_next   diagnostics
 *   loader_sample_counter   wall-clock time of the last loader PC sample
 *                      (ia64/state.c, with trace=); host time, log output only
 *   irq_kick           asynchronous executor yield request (vcpu_kick);
 *                      not guest state, set by other CPUs at any time
 *   mmu_seq, mmu_log, mmu_iseq, mmu_dseq   MMU change tracking for hostwin and translation
 *                      caches. The underlying tr/tc/rr/pkr state is compared;
 *                      counters differ with walker timing and snapshot restore
 *   purge_seen         global-purge log cursor; the TC itself is compared
 *   code_quiescent     code-reclamation epoch; VMM bookkeeping
 *   decoded            per-vCPU decoded-bundle cache pointer (cache)
 *   upper              engine-private pointer of the upper tier (Tier1's
 *                      struct t1_vcpu), not guest state
 *   native             IA-32 private backend pointer, not guest state
 *   parked             executor idle bracket, host-side
 *   sync_i_waiting     executor inside sync.i, host-side
 *   check_context, check_workspace, check_workspace_pages   diagnostics
 *   memory_hook, clock_hook, access_hook   harness hooks
 *   cfg, clock_origin                      constants of the run
 */
#define VCPU_EXCLUDED(X)                                                                           \
    X(fault)                                                                                       \
    X(timer_last)                                                                                  \
    X(physical_region)                                                                             \
    X(physical_page)                                                                               \
    X(code_page)                                                                                   \
    X(code_entry_generation)                                                                       \
    X(trace)                                                                                       \
    X(trace_count)                                                                                 \
    X(trace_used)                                                                                  \
    X(trace_next)                                                                                  \
    X(loader_sample_counter)                                                                       \
    X(irq_kick)                                                                                    \
    X(mmu_seq)                                                                                     \
    X(mmu_iseq)                                                                                    \
    X(mmu_dseq)                                                                                    \
    X(mmu_log)                                                                                     \
    X(purge_seen)                                                                                  \
    X(code_quiescent)                                                                              \
    X(decoded)                                                                                     \
    X(upper)                                                                                       \
    X(parked)                                                                                      \
    X(sync_i_waiting)                                                                              \
    X(check_context)                                                                               \
    X(check_workspace)                                                                             \
    X(check_workspace_pages)                                                                       \
    X(memory_hook)                                                                                 \
    X(clock_hook)                                                                                  \
    X(access_hook)                                                                                 \
    X(cfg)                                                                                         \
    X(clock_origin)                                                                                \
    X(microtlb)                                                                                    \
    X(tlb_index)                                                                                   \
    X(timer_cache)                                                                                 \
    X(native)

/* ---- coverage check -------------------------------------------------- */

struct span {
    size_t offset, size, align;
    const char *name;
};
#define SPAN(type, m)                                                                              \
    {offsetof(type, m), sizeof(((type *)0)->m), _Alignof(__typeof__(((type *)0)->m)), #m},

static bool covered(const char *type, struct span *s, size_t n, size_t size, size_t align)
{
    for (size_t i = 1; i < n; ++i) /* insertion sort by offset */
        for (size_t j = i; j > 0 && s[j].offset < s[j - 1].offset; --j) {
            struct span t = s[j];
            s[j] = s[j - 1];
            s[j - 1] = t;
        }
    size_t end = 0;
    for (size_t i = 0; i < n; ++i) {
        /* Only the alignment padding before a member may be skipped. */
        size_t expected = (end + s[i].align - 1) & ~(s[i].align - 1);
        if (s[i].offset != expected) {
            LOGE("compare: %s.%s at %zu, expected %zu: an unlisted member precedes it", type,
                 s[i].name, s[i].offset, expected);
            return false;
        }
        end = s[i].offset + s[i].size;
    }
    if (((end + align - 1) & ~(align - 1)) != size) {
        LOGE("compare: %s ends at %zu of %zu: an unlisted member follows", type, end, size);
        return false;
    }
    return true;
}

bool compare_lists_complete(void)
{
#define S_STATE(m) SPAN(struct ia64_state, m)
#define S_FR(m) SPAN(struct ia64_fr, m)
#define S_TLB(m) SPAN(struct ia64_tlb_entry, m)
#define S_PAL(m) SPAN(struct ia64_pal_state, m)
#define S_VCPU(m) SPAN(struct vcpu, m)
    struct span state[] = {STATE_SCALARS(S_STATE) STATE_ARRAYS(S_STATE) S_STATE(fr) S_STATE(tr)
                               S_STATE(tc) S_STATE(pal)};
    struct span fr[] = {FR_MEMBERS(S_FR)};
    struct span tlb[] = {TLB_MEMBERS(S_TLB)};
    struct span pal[] = {PAL_MEMBERS(S_PAL)};
    struct span vcpu[] = {S_VCPU(s) VCPU_COMPARED(S_VCPU) VCPU_COMPARED_ARRAYS(S_VCPU)
                              VCPU_EXCLUDED(S_VCPU)};
    return covered("ia64_state", state, IA64VMM_ARRAY_SIZE(state), sizeof(struct ia64_state),
                   _Alignof(struct ia64_state)) &&
           covered("ia64_fr", fr, IA64VMM_ARRAY_SIZE(fr), sizeof(struct ia64_fr),
                   _Alignof(struct ia64_fr)) &&
           covered("ia64_tlb_entry", tlb, IA64VMM_ARRAY_SIZE(tlb), sizeof(struct ia64_tlb_entry),
                   _Alignof(struct ia64_tlb_entry)) &&
           covered("ia64_pal_state", pal, IA64VMM_ARRAY_SIZE(pal), sizeof(struct ia64_pal_state),
                   _Alignof(struct ia64_pal_state)) &&
           covered("vcpu", vcpu, IA64VMM_ARRAY_SIZE(vcpu), sizeof(struct vcpu),
                   _Alignof(struct vcpu));
}

/* ---- the relation ---------------------------------------------------- */

struct differ {
    compare_diff_fn fn;
    void *arg;
    unsigned count;
};

static void diff(struct differ *d, const char *what, uint64_t index, uint64_t a, uint64_t b)
{
    if (a == b)
        return;
    ++d->count;
    if (d->fn)
        d->fn(what, index, a, b, d->arg);
}

static void compare_state(struct differ *d, const struct ia64_state *a, const struct ia64_state *b)
{
#define C_SCALAR(m) diff(d, "s." #m, 0, (uint64_t)a->m, (uint64_t)b->m);
#define C_ARRAY(m)                                                                                 \
    for (size_t i = 0; i < IA64VMM_ARRAY_SIZE(a->m); ++i)                                          \
        diff(d, "s." #m, i, (uint64_t)a->m[i], (uint64_t)b->m[i]);
#define C_FR(m) diff(d, "s.fr[]." #m, i, (uint64_t)a->fr[i].m, (uint64_t)b->fr[i].m);
#define C_TR(m)                                                                                    \
    diff(d, "s.tr[][]." #m, side *IA64_ITR_COUNT + i, (uint64_t)a->tr[side][i].m,                  \
         (uint64_t)b->tr[side][i].m);
#define C_TC(m)                                                                                    \
    diff(d, "s.tc[][]." #m, side *IA64_TC_ENTRIES + i, (uint64_t)a->tc[side][i].m,                 \
         (uint64_t)b->tc[side][i].m);
#define C_PAL(m) diff(d, "s.pal." #m, 0, (uint64_t)a->pal.m, (uint64_t)b->pal.m);
    STATE_SCALARS(C_SCALAR)
    STATE_ARRAYS(C_ARRAY)
    for (size_t i = 0; i < IA64VMM_ARRAY_SIZE(a->fr); ++i) {
        FR_MEMBERS(C_FR)
    }
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(a->tr[0]); ++i) {
            TLB_MEMBERS(C_TR)
        }
        for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(a->tc[0]); ++i) {
            TLB_MEMBERS(C_TC)
        }
    }
    PAL_MEMBERS(C_PAL)
}

unsigned compare_runs(const struct compare_run *ref, const struct compare_run *got,
                      compare_diff_fn fn, void *arg)
{
    struct differ d = {fn, arg, 0};
    diff(&d, "exit_present", 0, ref->exit != NULL, got->exit != NULL);
    if (ref->exit && got->exit) {
        diff(&d, "exit.reason", 0, ref->exit->reason, got->exit->reason);
        diff(&d, "exit.ip", 0, ref->exit->ip, got->exit->ip);
        diff(&d, "exit.slot", 0, ref->exit->slot, got->exit->slot);
        diff(&d, "exit.info", 0, ref->exit->info, got->exit->info);
    }
    const struct vcpu *a = ref->v, *b = got->v;
    compare_state(&d, &a->s, &b->s);
#define C_VCPU(m) diff(&d, #m, 0, (uint64_t)a->m, (uint64_t)b->m);
#define C_VCPU_ARRAY(m)                                                                            \
    for (size_t i = 0; i < IA64VMM_ARRAY_SIZE(a->m); ++i)                                          \
        diff(&d, #m, i, (uint64_t)a->m[i], (uint64_t)b->m[i]);
    VCPU_COMPARED(C_VCPU)
    VCPU_COMPARED_ARRAYS(C_VCPU_ARRAY)

    diff(&d, "ram_form", 0, ref->ram != NULL, got->ram != NULL);
    if (ref->ram && got->ram) {
        diff(&d, "ram_bytes", 0, ref->ram_bytes, got->ram_bytes);
        /* Byte-exact; whole equal pages are skipped with one memcmp. */
        if (ref->ram_bytes == got->ram_bytes)
            for (uint64_t page = 0; page < ref->ram_bytes; page += 4096) {
                uint64_t end = ref->ram_bytes - page < 4096 ? ref->ram_bytes : page + 4096;
                if (!memcmp(ref->ram + page, got->ram + page, end - page))
                    continue;
                for (uint64_t i = page; i < end; ++i)
                    diff(&d, "ram", i, ref->ram[i], got->ram[i]);
            }
    } else if (!ref->ram && !got->ram) {
        diff(&d, "ram_pages", 0, ref->ram_pages, got->ram_pages);
        for (size_t i = 0; i < ref->ram_pages && i < got->ram_pages; ++i)
            diff(&d, "ram_page", i, ref->ram_hash[i], got->ram_hash[i]);
    }

    diff(&d, "device_count", 0, ref->device_count, got->device_count);
    for (size_t i = 0; i < ref->device_count && i < got->device_count; ++i) {
        const struct compare_device_record *x = &ref->device[i], *y = &got->device[i];
        diff(&d, "device[].address", i, x->address, y->address);
        diff(&d, "device[].value", i, x->value, y->value);
        diff(&d, "device[].size", i, x->size, y->size);
        diff(&d, "device[].write", i, x->write, y->write);
    }

    diff(&d, "store_log_present", 0, ref->stores != NULL, got->stores != NULL);
    if (ref->stores && got->stores) {
        diff(&d, "store_count", 0, ref->store_count, got->store_count);
        for (size_t i = 0; i < ref->store_count && i < got->store_count; ++i) {
            const struct compare_store_record *x = &ref->stores[i], *y = &got->stores[i];
            uint64_t mask = x->width >= 8 ? UINT64_MAX : (UINT64_C(1) << (8 * x->width)) - 1;
            diff(&d, "store[].gva", i, x->gva, y->gva);
            diff(&d, "store[].width", i, x->width, y->width);
            diff(&d, "store[].value", i, x->value & mask, y->value & mask);
        }
    }
    return d.count;
}
