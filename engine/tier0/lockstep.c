/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/tier0/internal.h"
#include "engine/interp/interp.h"
#include "ia64/debug.h"
#include "ia64/mmu.h"
#include "ia64/smp.h"
#include "ia64/tlb.h"
#include "ia64/interrupt.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#include "ia64vmm/platform.h"

/* RSE/PAL may touch multiple pages or many words in one instruction.
 * Reference RAM is private; devices and clock samples are replayed once
 * Bounds fail explicitly rather than escaping the overlay. */
#define CHECK_PAGES (TIER0_MAX_INSNS + 4)
#define CHECK_TRANSACTIONS (TIER0_MAX_INSNS * 8)
#define CHECK_ACCESSES 4096u /* whole RSE and checked PAL buffers; overflow fails */
/* bytes: 16-byte aligned so the shadow keeps the original backing's low
 * address bits up to the 16-byte atomic forms' requirement. */
struct check_page {
    struct gm_region original, shadow;
    uint8_t bytes[4096 + 16] IA64VMM_ALIGNED(16);
};
struct check_bus {
    uint64_t address, value;
    unsigned size, requested;
    bool write;
};
struct tier0_check {
    struct vcpu shadow;
    struct vcpu *actual;
    struct ia64_bundle replay_bundle;
    struct check_page pages[CHECK_PAGES];
    struct check_bus bus[CHECK_TRANSACTIONS];
    struct lockstep_access accesses[CHECK_ACCESSES];
    unsigned naccesses, access_replay;
    bool value_replay;
    uint64_t clocks[CHECK_TRANSACTIONS];
    unsigned nclocks, clock_replay;
    unsigned npages, nbus, replay;
    bool mismatch;
    struct gm_region proxy;
    struct gm_mmio_ops proxy_ops;
    const struct gm_region *original_region;
    uint64_t before_retired;
};
#include "engine/tier0/check-bytes.h"

static void copy_bytes(void *dst, const void *src, size_t bytes)
{
    size_t words = bytes / 8;
    __asm__ volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(words)::"memory");
    memcpy(dst, src, bytes % 8);
}

#ifdef IA64VMM_HOSTED
static _Thread_local bool shadow_active;
#else
static bool shadow_active_by_cpu[256];
#define shadow_active shadow_active_by_cpu[hal_cpu_index()]
#endif
static const struct gm_region *overlay(struct vcpu *v, gpa_t gpa, unsigned size);
static uint64_t clock_sample(const struct vcpu *v, uint64_t value);
static bool access_sample(struct vcpu *v, struct lockstep_access *a, bool completed);
bool tier0_shadow_active(void)
{
    return shadow_active;
}
bool tier0_is_shadow(const struct vcpu *v)
{
    const struct tier0_check *c = v->check_context;
    return c && v == &c->shadow;
}
struct ia64_bundle *tier0_shadow_bundle(struct vcpu *v)
{
    struct tier0_check *c = v->check_context;
    return &c->replay_bundle;
}
static size_t check_npages(void)
{
    return (sizeof(struct tier0_check) + 4095) / 4096;
}
uint64_t tier0_check_reserve_bytes(void)
{
    return check_npages() * 4096;
}
struct tier0_check *tier0_check_begin(struct vcpu *v)
{
    /* mmap/munmap at every short region made framebuffer loops exceed the
     * watchdog. Only captured pages are initialized, once per region. */
    struct tier0_check *c = v->check_workspace;
    if (!c) {
        c = hal_alloc_pages(check_npages(), HAL_PAGE_ZERO);
        v->check_workspace = c;
        v->check_workspace_pages = check_npages();
    }
    if (c) {
        c->npages = c->nbus = c->replay = c->nclocks = c->clock_replay = 0;
        c->mismatch = false;
        c->naccesses = c->access_replay = 0;
        c->value_replay = ia64_smp_active();
        copy_bytes(&c->shadow, v, sizeof(*v));
        c->shadow.trace = NULL;
        c->shadow.native = NULL; /* A replay clone cannot own the real hardware container. */
        c->shadow.irq_kick = 0;  /* Asynchronous executor state is never replayed. */
        c->shadow.trace_count = c->shadow.trace_used = c->shadow.trace_next = 0;
        c->before_retired = v->retired;
        c->actual = v;
        v->memory_hook = c->shadow.memory_hook = overlay;
        v->clock_hook = c->shadow.clock_hook = clock_sample;
        v->access_hook = c->shadow.access_hook = c->value_replay ? access_sample : NULL;
        v->check_context = c->shadow.check_context = c;
    }
    return c;
}
void tier0_check_destroy(struct tier0_check *c)
{
    if (c) {
        c->actual->memory_hook = NULL;
        c->actual->clock_hook = NULL;
        c->actual->access_hook = NULL;
        c->actual->check_context = NULL;
        c->actual->physical_region = NULL;
    }
}

bool tier0_check_safe(const Ia64Instruction *i, const struct vcpu *v)
{
    /* Arbitrary platform break hooks have no replay contract. PAL's
     * checked physical accesses do, including copy and buffer results. */
    if (i->opcode == IA64_OP_BREAK)
        return !ia64_break_hook_installed() ||
               (i->unit == IA64_UNIT_M && i->slot == 0 &&
                i->operands.system.immediate == 0x100000 && ia64_pal_entry_matches(v, i->address));
    return true;
}
static struct check_page *capture(struct tier0_check *c, const struct gm_region *r, gpa_t gpa)
{
    gpa_t base = gpa & ~UINT64_C(4095);
    for (unsigned i = 0; i < c->npages; ++i)
        if ((c->pages[i].shadow.base & ~UINT64_C(4095)) == base)
            return &c->pages[i];
    if (c->npages == CHECK_PAGES)
        return NULL;
    struct check_page *p = &c->pages[c->npages++];
    p->original = *r;
    p->shadow = *r;
    uint64_t first = base > r->base ? base : r->base;
    uint64_t len = r->size - (first - r->base);
    if (len > 4096 - (first - base))
        len = 4096 - (first - base);
    p->shadow.base = first;
    p->shadow.size = len;
    p->original.base = first;
    p->original.size = len;
    p->original.hva = r->hva + (size_t)(first - r->base);
    /* Preserve backing alignment: the atomic helpers (up to the 16-byte
     * forms) reject unaligned HVA even when the guest GPA is aligned. */
    p->shadow.hva = p->bytes + ((uintptr_t)p->original.hva & 15);
    copy_bytes(p->shadow.hva, p->original.hva, len);
    return p;
}
static struct check_bus *bus_record(struct tier0_check *c, struct vcpu *v, uint64_t offset,
                                    unsigned size, bool write)
{
    if (c->nbus == CHECK_TRANSACTIONS) {
        c->mismatch = true;
        return NULL;
    }
    struct check_bus *b = &c->bus[c->nbus++];
    *b = (struct check_bus){.address = c->proxy.base + offset,
                            .size = size,
                            .write = write,
                            .requested = v->requested_exit};
    return b;
}
static uint64_t record_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    struct tier0_check *c = opaque;
    const struct gm_region *r = c->original_region;
    uint64_t value;
    if (r->kind == GM_EMULATED)
        value = r->ops->read(r->opaque, v, offset, size);
    else {
        /* Same aligned, width-specific transaction as ia64_physical_read. */
        const volatile uint8_t *p = r->hva + offset;
        switch (size) {
        case 1:
            value = *p;
            break;
        case 2:
            value = *(const volatile uint16_t *)p;
            break;
        case 4:
            value = *(const volatile uint32_t *)p;
            break;
        default:
            value = *(const volatile uint64_t *)p;
            break;
        }
    }
    struct check_bus *b = bus_record(c, v, offset, size, false);
    if (b)
        b->value = value & ia64_low_mask(size * 8);
    return value;
}
static void record_write(void *opaque, struct vcpu *v, uint64_t offset, uint64_t value,
                         unsigned size)
{
    struct tier0_check *c = opaque;
    const struct gm_region *r = c->original_region;
    if (r->kind == GM_EMULATED)
        r->ops->write(r->opaque, v, offset, value, size);
    else {
        volatile uint8_t *p = r->hva + offset;
        code_notify_cpu_write(r->base + offset, size);
        switch (size) {
        case 1:
            *p = value;
            break;
        case 2:
            *(volatile uint16_t *)p = value;
            break;
        case 4:
            *(volatile uint32_t *)p = value;
            break;
        default:
            *(volatile uint64_t *)p = value;
            break;
        }
    }
    struct check_bus *b = bus_record(c, v, offset, size, true);
    if (b)
        b->value = value & ia64_low_mask(size * 8);
}
static struct check_bus *bus_replay(struct tier0_check *c, struct vcpu *v, uint64_t offset,
                                    unsigned size, bool write, uint64_t value)
{
    if (c->replay == c->nbus) {
        c->mismatch = true;
        return NULL;
    }
    struct check_bus *b = &c->bus[c->replay++];
    if (b->address != c->proxy.base + offset || b->size != size || b->write != write ||
        (write && b->value != (value & ia64_low_mask(size * 8))))
        c->mismatch = true;
    v->requested_exit = b->requested;
    return b;
}
static uint64_t replay_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    struct check_bus *b = bus_replay(opaque, v, offset, size, false, 0);
    return b ? b->value : 0;
}
static void replay_write(void *opaque, struct vcpu *v, uint64_t offset, uint64_t value,
                         unsigned size)
{
    struct tier0_check *c = opaque;
    struct check_bus *b = bus_replay(c, v, offset, size, true, value);
    /* Only the pending per-vCPU request is replayed. Completion runs once
     * after the checked engine exits, outside its CPU/RAM transaction. */
    if (b && c->proxy.base == IA64VMM_SERVICE_BASE && size == 8 &&
        (offset == IA64VMM_SVC_PAL_STATE_SAVE || offset == IA64VMM_SVC_PAL_STATE_RESUME))
        ia64_pal_service_write(v, (unsigned)offset, value);
}
static const struct gm_mmio_ops record_ops = {record_read, record_write};
static const struct gm_mmio_ops replay_ops = {replay_read, replay_write};
static void proxy_setup(struct tier0_check *c, const struct gm_region *r, bool replay)
{
    c->proxy = *r;
    c->proxy.kind = GM_EMULATED;
    c->proxy.opaque = c;
    c->proxy_ops = replay ? replay_ops : record_ops;
    if (r->kind == GM_EMULATED) {
        if (!r->ops->read)
            c->proxy_ops.read = NULL;
        if (!r->ops->write)
            c->proxy_ops.write = NULL;
    }
    c->proxy.ops = &c->proxy_ops;
}

bool tier0_check_capture(struct tier0_execution *e, const struct gm_region *r, gpa_t gpa)
{
    return capture(e->check, r, gpa) != NULL;
}
static const struct gm_region *overlay(struct vcpu *v, gpa_t gpa, unsigned size)
{
    struct tier0_check *c = v->check_context;
    bool replay = v == &c->shadow;
    const struct gm_region *r = gm_lookup(gpa);
    if (!r || size > r->size - (gpa - r->base))
        return NULL;
    if (r->kind == GM_RAM || r->kind == GM_FIRMWARE) {
        if (c->value_replay) {
            if (replay)
                for (unsigned n = 0; n < c->npages; ++n)
                    if (gpa >= c->pages[n].shadow.base && size <= c->pages[n].shadow.size &&
                        gpa - c->pages[n].shadow.base <= c->pages[n].shadow.size - size)
                        return &c->pages[n].shadow;
            return r;
        }
        struct check_page *p = NULL;
        if (!replay)
            p = capture(c, r, gpa);
        else
            for (unsigned n = 0; n < c->npages; ++n)
                if (gpa >= c->pages[n].shadow.base &&
                    gpa - c->pages[n].shadow.base < c->pages[n].shadow.size) {
                    p = &c->pages[n];
                    break;
                }
        /* A divergent helper address must never read or write real RAM. */
        if (!p) {
            c->mismatch = true;
            return NULL;
        }
        return replay ? &p->shadow : r;
    }
    if (r->kind == GM_DIRECT_MMIO && ((uintptr_t)(r->hva + gpa - r->base) & (size - 1)))
        return r; /* Preserve the original alignment fault before I/O. */
    c->original_region = r;
    proxy_setup(c, r, replay);
    return &c->proxy;
}
static uint64_t clock_sample(const struct vcpu *v, uint64_t value)
{
    struct tier0_check *c = v->check_context;
    if (v == &c->shadow) {
        if (c->clock_replay == c->nclocks) {
            c->mismatch = true;
            return 0;
        }
        return c->clocks[c->clock_replay++];
    }
    if (c->nclocks == CHECK_TRANSACTIONS) {
        c->mismatch = true;
        return value;
    }
    c->clocks[c->nclocks++] = value;
    return value;
}
static void report(const struct tier0_execution *e, const struct vcpu *v,
                   const struct tier0_check *c)
{
    LOGE("lockstep divergence entry=%llx:%u actual=%llx:%u reference=%llx:%u",
         (unsigned long long)e->region->key.ip, e->region->key.slot, (unsigned long long)v->s.ip,
         v->s.slot, (unsigned long long)c->shadow.s.ip, c->shadow.s.slot);
    LOGE("mailbox actual=%u reference=%u bus=%u/%u clocks=%u/%u retired=%llu/%llu",
         __atomic_load_n(&v->requested_exit, __ATOMIC_ACQUIRE), c->shadow.requested_exit, c->nbus,
         c->replay, c->nclocks, c->clock_replay, (unsigned long long)v->retired,
         (unsigned long long)c->shadow.retired);
    LOGE("accesses=%u/%u mismatch=%u", c->naccesses, c->access_replay, c->mismatch);
    const uint8_t *a = (const uint8_t *)&v->s, *b = (const uint8_t *)&c->shadow.s;
    for (unsigned n = 0; n < sizeof(v->s); ++n)
        if (a[n] != b[n]) {
            LOGE("state byte %u: tier0=%02x interp=%02x", n, a[n], b[n]);
            break;
        }
    for (unsigned n = 0; n < 128; ++n)
        if (v->s.gr[n] != c->shadow.s.gr[n] || v->s.nat[n] != c->shadow.s.nat[n])
            LOGE("physical r%u tier0=%llx nat=%u interp=%llx nat=%u", n,
                 (unsigned long long)v->s.gr[n], v->s.nat[n], (unsigned long long)c->shadow.s.gr[n],
                 c->shadow.s.nat[n]);
    for (unsigned n = 0; n < e->region->count; ++n) {
        const Ia64Instruction *i = &e->region->insns[n];
        LOGE("region[%u] %llx:%u %-18s raw=%llx", n, (unsigned long long)i->address, i->slot,
             ia64_opcode_info(i->opcode)->name, (unsigned long long)i->raw);
    }
}
static bool requested_equal(const struct vcpu *v, const struct vcpu *shadow)
{
    unsigned live = __atomic_load_n(&v->requested_exit, __ATOMIC_ACQUIRE);
    return live == VCPU_EXIT_DEBUG + 1 || live == shadow->requested_exit;
}
bool tier0_check_finish(struct tier0_execution *e, const struct vcpu *v)
{
    struct tier0_check *c = e->check;
    struct vcpu *shadow = &c->shadow;
    uint64_t retired = v->retired - c->before_retired;
    unsigned attempts =
        (unsigned)retired + (e->fault || (e->stop && e->result.kind == IA64_STEP_EXIT) ? 1 : 0);
    struct ia64_result last = ia64_complete();
    shadow_active = true;
    for (unsigned n = 0; n < attempts && n < e->region->count; ++n) {
        const Ia64Instruction *i = &e->region->insns[n];
        if (c->mismatch)
            break;
        uint64_t old_psr = shadow->s.psr;
        /* Regions run with PSR.db = 1 when no pair is armed at their CPL
         * (tier0.c): the reference keeps interp_run's fetch-side check. */
        last = ia64_debug_instruction(shadow);
        if (last.kind == IA64_STEP_COMPLETE)
            last = ia64_execute(shadow, i);
        if (last.kind == IA64_STEP_FAULT || last.kind == IA64_STEP_EXIT)
            break;
        if (old_psr & IA64_PSR_IC)
            shadow->last_successful_bundle = i->address;
        ++shadow->retired;
        if (last.kind == IA64_STEP_BRANCH) {
            shadow->s.ip = last.info;
            shadow->s.slot = last.next_slot;
        } else {
            shadow->s.ip = i->address;
            shadow->s.slot = i->slot + e->region->slots[n];
            if (shadow->s.slot == 3) {
                shadow->s.slot = 0;
                shadow->s.ip += 16;
            }
        }
        shadow->requested_exit = 0;
    }
    shadow_active = false;
    /* vm_stop_all writes DEBUG+1 asynchronously. Every other mailbox
     * value is a synchronous instruction effect and must match replay. */
    bool same = !c->mismatch && same_bytes(&v->s, &shadow->s, sizeof(v->s)) &&
                v->retired == shadow->retired && v->clock_offset == shadow->clock_offset &&
                requested_equal(v, shadow) && c->nbus == c->replay &&
                c->naccesses == c->access_replay && c->nclocks == c->clock_replay &&
                v->timer_last == shadow->timer_last && v->timer_armed == shadow->timer_armed &&
                v->timer_fired == shadow->timer_fired &&
                v->last_successful_bundle == shadow->last_successful_bundle &&
                !memcmp(v->in_service, shadow->in_service, sizeof(v->in_service)) &&
                v->pal_state_address == shadow->pal_state_address &&
                v->pal_state_pending == shadow->pal_state_pending &&
                v->pal_state_valid == shadow->pal_state_valid &&
                !memcmp(v->pal_min_state, shadow->pal_min_state, sizeof(v->pal_min_state));
    if (e->fault || (e->stop && e->result.kind == IA64_STEP_EXIT))
        same &= last.kind == e->result.kind && last.fault == e->result.fault &&
                last.info == e->result.info && last.exit == e->result.exit &&
                last.fault_isr == e->result.fault_isr &&
                last.translation_fault == e->result.translation_fault;
    for (unsigned n = 0; !c->value_replay && n < c->npages; ++n) {
        const struct check_page *p = &c->pages[n];
        if (!same_bytes(p->original.hva, p->shadow.hva, p->shadow.size)) {
            same = false;
            for (unsigned j = 0; j < p->shadow.size; ++j)
                if (p->original.hva[j] != p->shadow.hva[j]) {
                    LOGE("memory %llx tier0=%02x interp=%02x",
                         (unsigned long long)(p->shadow.base + j), p->original.hva[j],
                         p->shadow.hva[j]);
                    break;
                }
        }
    }
    if (!same)
        report(e, v, c);
    /* Lockstep coverage is the diagnostic itself: counted in every build,
     * like the unchecked-instruction count (tier0.c interpreter_step). */
    else
        TIER0_ADD(tier0_local_stats(), checked_regions, 1);
    return same;
}

static bool boundary_equal(const struct tier0_check *c, const struct vcpu *v)
{
    bool same = !c->mismatch && same_bytes(&v->s, &c->shadow.s, sizeof(v->s)) &&
                v->retired == c->shadow.retired && requested_equal(v, &c->shadow) &&
                v->clock_offset == c->shadow.clock_offset &&
                v->last_successful_bundle == c->shadow.last_successful_bundle &&
                v->timer_last == c->shadow.timer_last && v->timer_armed == c->shadow.timer_armed &&
                v->timer_fired == c->shadow.timer_fired && c->nbus == c->replay &&
                c->naccesses == c->access_replay && c->nclocks == c->clock_replay &&
                !memcmp(v->in_service, c->shadow.in_service, sizeof(v->in_service)) &&
                v->pal_state_address == c->shadow.pal_state_address &&
                v->pal_state_pending == c->shadow.pal_state_pending &&
                v->pal_state_valid == c->shadow.pal_state_valid &&
                !memcmp(v->pal_min_state, c->shadow.pal_min_state, sizeof(v->pal_min_state));
    for (unsigned n = 0; !c->value_replay && n < c->npages; ++n) {
        const struct check_page *p = &c->pages[n];
        if (!same_bytes(p->original.hva, p->shadow.hva, p->shadow.size))
            same = false;
    }
    return same;
}
struct ia64_result tier0_check_fetch(struct vcpu *v, struct ia64_translation *translated)
{
    struct tier0_check *c = tier0_check_begin(v);
    if (!c)
        return ia64_exit(VCPU_EXIT_DEBUG, 0, "lockstep allocation failed");
    struct ia64_result actual = ia64_translate(v, v->s.ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                                               (v->s.psr >> 32) & 3, false, translated);
    struct ia64_translation reference;
    shadow_active = true;
    struct ia64_result expected =
        ia64_translate(&c->shadow, c->shadow.s.ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                       (c->shadow.s.psr >> 32) & 3, false, &reference);
    shadow_active = false;
    bool same = actual.kind == expected.kind && actual.fault == expected.fault &&
                actual.info == expected.info && actual.exit == expected.exit &&
                actual.fault_isr == expected.fault_isr &&
                actual.translation_fault == expected.translation_fault &&
                translated->pa == reference.pa && translated->pte == reference.pte &&
                translated->ma == reference.ma && boundary_equal(c, v);
    tier0_check_destroy(c);
    if (!same)
        return ia64_exit(VCPU_EXIT_DEBUG, 0, "fetch boundary lockstep divergence");
    TIER0_ADD(tier0_local_stats(), checked_boundaries, 1);
    return actual;
}
bool tier0_check_interrupt(struct vcpu *v, bool *delivered)
{
    struct tier0_check *c = tier0_check_begin(v);
    if (!c)
        return false;
    *delivered = ia64_external_deliver(v);
    bool expected = ia64_external_deliver(&c->shadow);
    bool same = expected == *delivered && boundary_equal(c, v);
    tier0_check_destroy(c);
    if (same)
        TIER0_ADD(tier0_local_stats(), checked_boundaries, 1);
    return same;
}
/* Debug and restored single-instruction PSR states use interpreter retirement.
 * Replay against private RAM, reusing captured I/O and clocks exactly once. */
struct vcpu_exit tier0_check_debug_boundary(struct vcpu *v, bool *checked)
{
    *checked = false;
    struct tier0_check *c = tier0_check_begin(v);
    if (!c)
        return (struct vcpu_exit){VCPU_EXIT_DEBUG, v->s.ip, v->s.slot, 0,
                                  "lockstep allocation failed"};
    struct vcpu_exit actual = interp_run(v, 1);
    /* interp_run may fault and then retire a different handler instruction.
     * With an opaque break callback installed, that handler could invoke it;
     * no replay is permitted without the callback's side-effect contract. */
    if (ia64_break_hook_installed() && v->fault.kind != IA64_FAULT_NONE) {
        tier0_check_destroy(c);
        return actual;
    }
    shadow_active = true;
    struct vcpu_exit expected = interp_run(&c->shadow, 1);
    shadow_active = false;
    bool same = actual.reason == expected.reason && actual.ip == expected.ip &&
                actual.slot == expected.slot && actual.info == expected.info &&
                boundary_equal(c, v);
    tier0_check_destroy(c);
    if (!same)
        return (struct vcpu_exit){VCPU_EXIT_DEBUG, v->s.ip, v->s.slot, 0,
                                  "interpreter boundary lockstep divergence"};
    *checked = true;
    TIER0_ADD(tier0_local_stats(), checked_regions, 1);
    return actual;
}

/* Region kinds are unchanged. Loads get recorded bytes; the CPU computes
 * STORE/RMW writes and WROTE independently before the completed callback. */
static bool access_sample(struct vcpu *v, struct lockstep_access *a, bool completed)
{
    struct tier0_check *c = v->check_context;
    if (v != &c->shadow) {
        if (completed) {
            if (c->naccesses == CHECK_ACCESSES) {
                LOGE("lockstep access log full");
                c->mismatch = true;
            } else
                c->accesses[c->naccesses++] = *a;
        }
        return false;
    }
    if (c->access_replay == c->naccesses) {
        LOGE("lockstep access %u: unexpected kind=%u gpa=%llx width=%u ip=%llx:%u",
             c->access_replay, a->kind, (unsigned long long)a->gpa, a->width,
             (unsigned long long)a->ip, a->slot);
        c->mismatch = true;
        return true;
    }
    const struct lockstep_access *record = &c->accesses[c->access_replay];
    bool same = a->kind == record->kind && a->gpa == record->gpa && a->width == record->width &&
                a->ip == record->ip && a->slot == record->slot;
    if (!completed) {
        a->read[0] = record->read[0];
        a->read[1] = record->read[1];
    } else {
        same &= a->flags == record->flags && same_bytes(a->read, record->read, sizeof(a->read)) &&
                same_bytes(a->written, record->written, sizeof(a->written));
        ++c->access_replay;
    }
    if (!same) {
        LOGE("lockstep access %u: expected kind=%u gpa=%llx width=%u ip=%llx:%u flags=%u; "
             "got kind=%u gpa=%llx width=%u ip=%llx:%u flags=%u completed=%u",
             c->access_replay - (completed ? 1 : 0), record->kind, (unsigned long long)record->gpa,
             record->width, (unsigned long long)record->ip, record->slot, record->flags, a->kind,
             (unsigned long long)a->gpa, a->width, (unsigned long long)a->ip, a->slot, a->flags,
             completed);
        c->mismatch = true;
    }
    return true;
}
bool tier0_check_code(struct vcpu *v, gpa_t gpa)
{
    struct tier0_check *c = v->check_context;
    if (!c || !c->value_replay)
        return true;
    if (v != &c->shadow) {
        const struct gm_region *r = gm_lookup(gpa);
        if (r && (r->kind == GM_RAM || r->kind == GM_FIRMWARE) && capture(c, r, gpa))
            return true;
    } else
        for (unsigned n = 0; n < c->npages; ++n)
            if (gpa >= c->pages[n].shadow.base &&
                gpa - c->pages[n].shadow.base + 16 <= c->pages[n].shadow.size)
                return true;
    c->mismatch = true;
    return false;
}
bool tier0_check_bundle(struct vcpu *v, gpa_t gpa, const struct ia64_bundle *bundle)
{
    struct tier0_check *c = v->check_context;
    if (!c || !c->value_replay || v == &c->shadow)
        return true;
    for (unsigned n = 0; n < c->npages; ++n) {
        struct gm_region *r = &c->pages[n].shadow;
        if (gpa < r->base || r->size < 16 || gpa - r->base > r->size - 16)
            continue;
        /* Data references use the logged values. Only fetches read this overlay;
         * keep the selected, owner-only decoded bytes rather than an earlier
         * RAM image that a peer can have changed before the real fetch. */
        uint64_t words[2] = {bundle->low, bundle->high};
        copy_bytes(r->hva + (size_t)(gpa - r->base), words, sizeof(words));
        return true;
    }
    c->mismatch = true;
    return false;
}
void ia64_smp_purge_hook(struct vcpu *v, uint32_t rid, uint64_t va, unsigned shift, bool all)
{
    struct tier0_check *c = v->check_context;
    if (!c || !c->value_replay || v == &c->shadow)
        return;
    struct lockstep_access a = {.kind = LOCKSTEP_PURGE,
                                .ip = v->s.ip,
                                .slot = v->s.slot,
                                .rid = all ? 0 : rid,
                                .gpa = all ? 0 : va,
                                .width = all ? 0 : shift,
                                .flags = all ? LOCKSTEP_PURGE_ALL : 0};
    (void)access_sample(v, &a, true);
}
bool ia64_smp_replay_purges(struct vcpu *v)
{
    struct tier0_check *c = v->check_context;
    if (!c || v != &c->shadow)
        return false;
    while (c->access_replay < c->naccesses) {
        const struct lockstep_access *a = &c->accesses[c->access_replay];
        if (a->kind != LOCKSTEP_PURGE)
            break;
        if (a->ip != v->s.ip || a->slot != v->s.slot) {
            c->mismatch = true;
            break;
        }
        ia64_tlb_remote_purge(v, a->rid, a->gpa, a->width, a->flags & LOCKSTEP_PURGE_ALL);
        ++c->access_replay;
    }
    return true; /* a shadow never posts or waits, also in page-capture mode */
}
