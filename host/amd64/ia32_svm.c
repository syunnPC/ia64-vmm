/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/ia32_svm.h"
#include "host/amd64/ia32_native.h"
#include "host/amd64/ia32_native_mode.h"
#include "host/amd64/amd64.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"

/* 4 GiB of 4 KiB tokens: PML4, PDPT, four PDs and 2048 PTs. */
#define NATIVE_TABLES 2054u
#define NPT_NX (UINT64_C(1) << 63)
struct ia32_container {
    unsigned cpu;
    struct amd64_svm_ia32_requirements requirements;
    struct amd64_virtualization_cpu *owner;
    struct amd64_virtualization_tag tag;
    uint8_t *vmcb, *host, *iopm, *msrpm;
    void *tables[NATIVE_TABLES];
    unsigned table_count, flush;
    struct amd64_ia32_fp_owner *fp;
    struct amd64_svm_ia32_gprs gprs;
    struct ia32_native_state state;
    struct ia32_generations generations;
    bool imported;
};
#define CONTAINER_PAGES ((sizeof(struct ia32_container) + 4095) / 4096)
static uint64_t physical(const void *p)
{
    return (uintptr_t)p - amd64_hhdm;
}
static void *virtual(uint64_t pa)
{
    return (void *)(uintptr_t)(amd64_hhdm + pa);
}
static bool fail(struct ia32_setup_error *e, enum ia32_exit_reason reason, const char *detail)
{
    if (e)
        *e = (struct ia32_setup_error){
            .reason = reason, .host_cpu = hal_cpu_index(), .detail = detail};
    return false;
}
static bool owned(const struct ia32_container *c, struct ia32_setup_error *e)
{
    return (c && c->cpu == hal_cpu_index()) ||
           fail(e, IA32_EXIT_ENTRY_FAILURE, "native SVM owner CPU mismatch");
}
static bool probe(unsigned cpu, struct ia32_capabilities *out, struct ia32_setup_error *error)
{
    struct amd64_svm_ia32_requirements r;
    if (cpu != hal_cpu_index())
        return fail(error, IA32_EXIT_ENTRY_FAILURE, "native SVM probe CPU mismatch");
    if (!amd64_svm_ia32_requirements(&r, error))
        return false;
    *out = r.caps;
    return true;
}
static void destroy(struct ia32_container *c)
{
    if (!c || c->cpu != hal_cpu_index())
        return;
    for (unsigned i = 0; i < c->table_count; ++i)
        hal_free_pages(c->tables[i], 1);
    if (c->vmcb)
        hal_free_pages(c->vmcb, 1);
    if (c->host)
        hal_free_pages(c->host, 1);
    if (c->iopm)
        hal_free_pages(c->iopm, SVM_IOPM_SIZE / 4096);
    if (c->msrpm)
        hal_free_pages(c->msrpm, SVM_MSRPM_SIZE / 4096);
    amd64_ia32_fp_destroy(c->fp);
    if (c->owner) {
        amd64_virtualization_tag_release(c->owner, AMD64_VIRT_IA32, &c->tag);
        amd64_virtualization_release(c->owner, AMD64_VIRT_IA32);
    }
    hal_free_pages(c, CONTAINER_PAGES);
}
static void controls(struct ia32_container *c)
{
    const struct svm_vmcb_params p = {physical(c->iopm), physical(c->msrpm), physical(c->tables[0]),
                                      c->tag.id};
    amd64_svm_ia32_controls(c->vmcb, &p);
    if (c->requirements.rdpru)
        c->vmcb[VMCB_INTERCEPT_MISC2 + 1] |= 0x40;
    if (c->requirements.invlpgb)
        c->vmcb[0x14] = 0x11; /* INVLPGB and TLBSYNC */
}
static bool create(unsigned cpu, struct ia32_container **out, struct ia32_setup_error *error)
{
    struct amd64_svm_ia32_requirements r;
    *out = NULL;
    if (cpu != hal_cpu_index())
        return fail(error, IA32_EXIT_ENTRY_FAILURE, "native SVM create CPU mismatch");
    if (!amd64_svm_ia32_requirements(&r, error))
        return false;
    struct ia32_container *c = hal_alloc_pages(CONTAINER_PAGES, HAL_PAGE_ZERO);
    if (!c)
        return fail(error, IA32_EXIT_ENTRY_FAILURE, "native SVM container allocation failed");
    c->cpu = cpu;
    c->requirements = r;
    if (!amd64_virtualization_acquire(cpu, IA32_BACKEND_SVM, AMD64_VIRT_IA32, &c->owner, error) ||
        !amd64_virtualization_tag_acquire(c->owner, AMD64_VIRT_IA32, &c->tag, error) ||
        !amd64_ia32_fp_create(&c->fp, error)) {
        destroy(c);
        return false;
    }
    c->vmcb = hal_alloc_pages(1, HAL_PAGE_ZERO);
    c->host = hal_alloc_pages(1, HAL_PAGE_ZERO);
    c->iopm = hal_alloc_pages(SVM_IOPM_SIZE / 4096, HAL_PAGE_ZERO);
    c->msrpm = hal_alloc_pages(SVM_MSRPM_SIZE / 4096, HAL_PAGE_ZERO);
    c->tables[0] = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (c->tables[0])
        c->table_count = 1;
    if (!c->vmcb || !c->host || !c->iopm || !c->msrpm || !c->table_count) {
        destroy(c);
        return fail(error, IA32_EXIT_ENTRY_FAILURE, "native SVM control/table allocation failed");
    }
    memset(c->iopm, 0xff, SVM_IOPM_SIZE);
    memset(c->msrpm, 0xff, SVM_MSRPM_SIZE);
    controls(c);
    c->flush = 1;
    *out = c;
    return true;
}
static void merge(struct ia32_native_state *out, const struct ia32_native_state *n, uint64_t dirty)
{
    if (dirty & IA32_DIRTY_GPR)
        memcpy(out->gpr, n->gpr, sizeof(out->gpr));
    if (dirty & IA32_DIRTY_IP)
        out->eip = n->eip;
    if (dirty & IA32_DIRTY_FLAGS)
        out->eflag = n->eflag;
    if (dirty & IA32_DIRTY_CONTROL) {
        out->cflg = n->cflg;
        out->kr0 = n->kr0;
        out->kr1 = n->kr1;
        out->kr2 = n->kr2;
        out->cpl = n->cpl;
    }
    if (dirty & IA32_DIRTY_SEGMENTS) {
        out->es = n->es;
        out->cs = n->cs;
        out->ss = n->ss;
        out->ds = n->ds;
        out->fs = n->fs;
        out->gs = n->gs;
        out->ldtr = n->ldtr;
        out->tr = n->tr;
        out->gdtr = n->gdtr;
        out->idtr = n->idtr;
    }
    if (dirty & IA32_DIRTY_DEBUG) {
        memcpy(out->dr, n->dr, sizeof(out->dr));
        out->dr6 = n->dr6;
        out->dr7 = n->dr7;
    }
    if (dirty & IA32_DIRTY_FP)
        out->fp = n->fp;
    if (dirty & IA32_DIRTY_SHADOW) {
        out->interruptibility = n->interruptibility;
        out->pending_debug = n->pending_debug;
        out->interrupt_shadow = n->interrupt_shadow;
    }
}
static bool import(struct ia32_container *c, const struct ia32_native_state *n, uint64_t dirty,
                   struct ia32_setup_error *error)
{
    if (!owned(c, error))
        return false;
    if ((dirty & ~IA32_DIRTY_ALL) || (!c->imported && dirty != IA32_DIRTY_ALL) ||
        !c->tag.generation)
        return fail(error, IA32_EXIT_UNIMPLEMENTED,
                    "native SVM import requires complete initial state and a tag");
    struct ia32_native_state candidate = c->state;
    merge(&candidate, n, dirty);
    const char *detail;
    if (!amd64_svm_ia32_state_valid(&candidate, &detail))
        return fail(error, IA32_EXIT_UNIMPLEMENTED, detail);
    if (!amd64_ia32_fp_prepare(c->fp, &candidate.fp, error))
        return false;
    c->state = candidate;
    amd64_svm_ia32_put(c->vmcb, &c->gprs, &c->state);
    c->imported = true;
    return true;
}
static bool export(struct ia32_container *c, struct ia32_native_state *out,
                   struct ia32_setup_error *error)
{
    if (!owned(c, error))
        return false;
    if (!c->imported)
        return fail(error, IA32_EXIT_UNIMPLEMENTED, "native SVM state not imported");
    *out = c->state;
    return true;
}
/* Allocated tables persist until reset/destroy, so a registered remote leaf
 * pointer stays valid while its reverse-alias registration is held. */
static uint64_t *leaf(struct ia32_container *c, uint64_t token, bool allocate)
{
    uint64_t *table = c->tables[0];
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        uint64_t *slot = &table[(token >> shift) & 511],
                 entry = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
        if (!(entry & NPT_P)) {
            if (!allocate || c->table_count == NATIVE_TABLES)
                return NULL;
            void *next = hal_alloc_pages(1, HAL_PAGE_ZERO);
            if (!next)
                return NULL;
            c->tables[c->table_count++] = next;
            entry = physical(next) | NPT_P | NPT_W | NPT_U;
            __atomic_store_n(slot, entry, __ATOMIC_RELEASE);
        }
        table = virtual(entry & NPT_ADDR);
    }
    return &table[(token >> 12) & 511];
}
static bool map(struct ia32_container *c, const struct ia32_mapping *m,
                struct ia32_setup_error *error)
{
    if (!owned(c, error))
        return false;
    unsigned rights = m->rights;
    if (m->token.value > UINT32_MAX || ((m->token.value | m->hpa) & 4095) ||
        m->hpa >= (UINT64_C(1) << c->requirements.physical_bits) || !(rights & IA32_MAP_READ) ||
        (rights & ~7u) || (unsigned)m->memory_type > IA32_MEMORY_WC ||
        c->requirements.pat_flags[m->memory_type] == UINT64_MAX)
        return fail(error, IA32_EXIT_UNIMPLEMENTED,
                    "native SVM mapping address/rights/type is unrepresentable");
    if (memcmp(&m->generations, &c->generations, sizeof(c->generations)))
        return fail(error, IA32_EXIT_ENTRY_FAILURE, "native SVM mapping generations are stale");
    uint64_t *slot = leaf(c, m->token.value, true);
    if (!slot)
        return fail(error, IA32_EXIT_ENTRY_FAILURE, "native SVM nested table allocation failed");
    uint64_t entry = m->hpa | NPT_P | NPT_U | c->requirements.pat_flags[m->memory_type] |
                     ((rights & IA32_MAP_WRITE) ? NPT_W : 0) |
                     ((rights & IA32_MAP_EXEC) ? 0 : NPT_NX);
    __atomic_store_n(slot, entry, __ATOMIC_RELEASE);
    __atomic_store_n(&c->flush, 1, __ATOMIC_RELEASE);
    return true;
}
static bool unmap(struct ia32_container *c, ia32_token_t token, uint64_t bytes,
                  struct ia32_setup_error *error)
{
    if (!owned(c, error))
        return false;
    if (((token.value | bytes) & 4095) || token.value > UINT64_C(0x100000000) ||
        bytes > UINT64_C(0x100000000) - token.value)
        return fail(error, IA32_EXIT_UNIMPLEMENTED, "native SVM unmap range is invalid");
    if (bytes)
        amd64_ia32_clear_tables(c->tables[0], 4, 0, token.value, token.value + bytes, amd64_hhdm);
    __atomic_store_n(&c->flush, 1, __ATOMIC_RELEASE);
    return true;
}
static bool invalidate(struct ia32_container *c, const struct ia32_generations *g,
                       struct ia32_setup_error *error)
{
    if (!owned(c, error))
        return false;
    c->generations = *g;
    __atomic_store_n(&c->flush, 1, __ATOMIC_RELEASE);
    return true;
}
static struct ia32_native_exit stop(const struct ia32_container *c, enum ia32_exit_reason reason,
                                    const char *detail)
{
    struct ia32_native_exit e = {.backend = IA32_BACKEND_SVM,
                                 .reason = reason,
                                 .phase = IA32_PHASE_BEFORE,
                                 .detail = detail};
    if (c && c->imported) {
        e.position_valid = true;
        e.eip = c->state.eip;
        e.linear_ip.value = c->state.cs.base + c->state.eip.value;
        e.generations = c->generations;
    }
    return e;
}
static struct ia32_native_exit enter(struct ia32_container *c, struct ia32_native_state *out,
                                     const struct ia32_run_limit *limit,
                                     struct ia32_run_usage *usage)
{
    *usage = (struct ia32_run_usage){0};
    if (!owned(c, NULL))
        return stop(NULL, IA32_EXIT_ENTRY_FAILURE, "native SVM entry owner CPU mismatch");
    if (!c->imported || !c->tag.generation)
        return stop(c, IA32_EXIT_UNIMPLEMENTED, "native SVM entry has no imported state/tag");
    if (!limit || limit->deadline == UINT64_MAX || limit->instructions ||
        (limit->step && (!limit->svm_step_safe || c->state.interrupt_shadow)))
        return stop(c, IA32_EXIT_UNIMPLEMENTED,
                    "native SVM needs a finite deadline and verified TF-safe step without shadow");
    if (hal_counter() >= limit->deadline)
        return stop(c, IA32_EXIT_DEADLINE, "native SVM deadline expired before entry");
    struct ia32_setup_error error;
    if (!amd64_ia32_fp_prepare(c->fp, &c->state.fp, &error))
        return stop(c, error.reason, error.detail);
    amd64_svm_ia32_put(c->vmcb, &c->gprs, &c->state);
    bool private_tf = limit->step && !(c->state.eflag & 0x100);
    if (private_tf) {
        uint64_t flags = amd64_ia32_private_eflag(&c->state) | 0x100;
        /* DR6 status is sticky. Clear the private observation window,
         * then merge the guest's previous status back on export. */
        uint64_t dr6 = c->state.dr6 & ~UINT64_C(0xe00f);
        memcpy(c->vmcb + VMCB_RFLAGS, &flags, 8);
        memcpy(c->vmcb + VMCB_DR6, &dr6, 8);
    }
    uint64_t flags = amd64_svm_ia32_lock();
    if (!amd64_ia32_fp_host_admit(c->fp, &error)) {
        amd64_svm_ia32_unlock(flags);
        hal_timer_cancel();
        return stop(c, error.reason, error.detail);
    }
    uint8_t pending;
    if (!amd64_virtualization_tag_prepare(c->owner, AMD64_VIRT_IA32, &c->tag,
                                          physical(c->tables[0]), &pending)) {
        amd64_svm_ia32_unlock(flags);
        hal_timer_cancel();
        return stop(c, IA32_EXIT_ENTRY_FAILURE, "native SVM tag admission failed");
    }
    bool local = __atomic_exchange_n(&c->flush, 0, __ATOMIC_ACQ_REL) != 0;
    c->vmcb[VMCB_TLB_CONTROL] = (pending || local) ? 1 : 0;
    hal_timer_arm(limit->deadline);
    uint64_t start = hal_counter();
    if (start >= limit->deadline) {
        if (local)
            __atomic_store_n(&c->flush, 1, __ATOMIC_RELEASE);
        amd64_svm_ia32_unlock(flags);
        hal_timer_cancel();
        return stop(c, IA32_EXIT_DEADLINE, "native SVM deadline expired during admission");
    }
    struct ia32_native_state before = c->state;
    struct amd64_ia32_segment_view segment_view = amd64_ia32_segment_view(&before);
    amd64_svm_ia32_run(c->vmcb, c->host, &c->gprs, c->fp, &c->state, c->requirements.tsc_aux);
    usage->residence_ticks = hal_counter() - start;
    struct ia32_native_exit e = amd64_svm_ia32_decode(c->vmcb, c->requirements.caps.nrip,
                                                      c->requirements.caps.decode_assists);
    bool entered = e.reason != IA32_EXIT_ENTRY_FAILURE;
    amd64_virtualization_tag_entered(c->owner, AMD64_VIRT_IA32, &c->tag, entered);
    if (!entered) {
        c->state = before;
        if (pending || local)
            __atomic_store_n(&c->flush, 1, __ATOMIC_RELEASE);
    } else {
        amd64_ia32_restore_fp_selectors(&c->state, segment_view);
        if (private_tf) {
            c->state.eflag &= ~0x100u;
            uint64_t debug = c->state.dr6;
            c->state.dr6 = (debug & ~(UINT64_C(1) << 14)) | (before.dr6 & UINT64_C(0xe00f));
            if (e.reason == IA32_EXIT_EXCEPTION && e.exception.vector == 1 &&
                (debug & (1u << 14)) && !(debug & 0xa00f)) {
                e.reason = IA32_EXIT_STEP;
                e.phase = IA32_PHASE_UNKNOWN;
                e.exception = (struct ia32_event){0};
            }
        }
        e.dirty = IA32_DIRTY_ALL;
    }
    e.prior_eflag = before.eflag;
    e.prior_eflag_valid = true;
    if (e.reason == IA32_EXIT_HOST_IRQ && hal_counter() >= limit->deadline)
        e.reason = IA32_EXIT_DEADLINE;
    amd64_ia32_step_host_phase(&e, limit->step);
    e.generations = c->generations;
    *out = c->state;
    amd64_svm_ia32_unlock(flags);
    hal_timer_cancel();
    return e;
}
static bool reset(struct ia32_container *c, struct ia32_setup_error *error)
{
    if (!owned(c, error))
        return false;
    c->imported = false;
    for (unsigned i = 1; i < c->table_count; ++i)
        hal_free_pages(c->tables[i], 1);
    c->table_count = 1;
    memset(c->tables[0], 0, 4096);
    memset(&c->state, 0, sizeof(c->state));
    memset(&c->gprs, 0, sizeof(c->gprs));
    memset(&c->generations, 0, sizeof(c->generations));
    amd64_virtualization_tag_release(c->owner, AMD64_VIRT_IA32, &c->tag);
    c->tag = (struct amd64_virtualization_tag){0};
    __atomic_store_n(&c->flush, 1, __ATOMIC_RELEASE);
    if (!amd64_virtualization_tag_acquire(c->owner, AMD64_VIRT_IA32, &c->tag, error))
        return false;
    controls(c);
    return true;
}
const struct ia32_backend_ops amd64_svm_ia32_ops = {
    .probe = probe,
    .create = create,
    .import = import,
    .export = export,
    .map = map,
    .unmap = unmap,
    .enter = enter,
    .invalidate = invalidate,
    .reset = reset,
    .destroy = destroy,
};
