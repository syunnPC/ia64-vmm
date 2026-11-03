/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/cache/background.h"
#include "engine/cache/cache.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"
#include "engine/tier0/tier0.h" /* Generated page dependencies. */

/* Bundle bytes belong to one executor. Remote notifications change only
 * validity and dependency links under code_lock. */
#define CACHE_PAGES 64u
struct code_page {
    struct code_dependency dependency;
    gpa_t base;
    uint64_t valid[4];
    struct ia64_bundle bundle[256];
    struct decoded_cache *owner;
};
/* Two ways keep the same 64-page allocation cap while avoiding alternating
 * decode misses for colliding code pages. Entries are never freed. */
struct decoded_cache {
    struct code_page *pages[CACHE_PAGES];
    unsigned recent[CACHE_PAGES / 2];
    struct code_cache_stats stats;
    struct vcpu *owner;
    struct decoded_cache *next, **previous;
};
static struct decoded_cache *caches;
static struct code_cache_stats destroyed_stats;
uint64_t code_cache_reserve_bytes(void)
{
    uint64_t bytes = 0;
    for (struct decoded_cache *c = caches; c; c = c->next) {
        for (unsigned i = 0; i < CACHE_PAGES; ++i)
            if (!c->pages[i])
                bytes += ((sizeof(struct code_page) + 4095) / 4096) * 4096;
        if (c->owner->cfg.engine == ENGINE_LOCKSTEP && !c->owner->check_workspace)
            bytes += tier0_check_reserve_bytes();
    }
    return bytes;
}
static uint64_t valid_word(const struct code_page *p, unsigned word)
{
    return __atomic_load_n(&p->valid[word], __ATOMIC_ACQUIRE);
}
static bool valid_any(const struct code_page *p)
{
    return valid_word(p, 0) | valid_word(p, 1) | valid_word(p, 2) | valid_word(p, 3);
}
bool code_cache_vcpu_init(struct vcpu *v)
{
    struct decoded_cache *old = v->decoded;
    if (old && old->owner == v)
        return true;
    bool take = !code_lock_held();
    if (take)
        code_lock();
    /* Diagnostic state copies may overwrite the private pointer. Find the
     * original context rather than taking ownership of the copy's cache. */
    for (struct decoded_cache *at = caches; at; at = at->next)
        if (at->owner == v) {
            v->decoded = at;
            if (take)
                code_unlock();
            return true;
        }
    struct decoded_cache *c = hal_alloc_pages((sizeof(*c) + 4095) / 4096, HAL_PAGE_ZERO);
    if (!c) {
        if (take)
            code_unlock();
        return false;
    }
    c->owner = v;
    c->next = caches;
    c->previous = &caches;
    if (caches)
        caches->previous = &c->next;
    caches = c;
    v->decoded = c;
    if (take)
        code_unlock();
    return true;
}
static void invalidate_decoded(struct code_dependency *dependency)
{
    struct code_page *p = (struct code_page *)dependency;
    if (!code_lock_held())
        hal_panic("decoded invalidation without code_lock");
    if (valid_any(p))
        __atomic_add_fetch(&p->owner->stats.invalidated_pages, 1, __ATOMIC_RELAXED);
    for (unsigned word = 0; word < 4; ++word) {
        uint64_t bits = valid_word(p, word) & ~dependency->invalidated[word];
        __atomic_store_n(&p->valid[word], bits, __ATOMIC_RELEASE);
        dependency->mask[word] = bits;
    }
    if (valid_any(p))
        code_dependency_attach(p->base, dependency);
}
void code_cache_vcpu_destroy(struct vcpu *v)
{
    code_lock();
    struct decoded_cache *c = caches;
    while (c && c->owner != v)
        c = c->next;
    if (!c) {
        v->decoded = NULL;
        code_unlock();
        return;
    }
    *c->previous = c->next;
    if (c->next)
        c->next->previous = c->previous;
    destroyed_stats.hits += c->stats.hits;
    destroyed_stats.decodes += c->stats.decodes;
    destroyed_stats.invalidated_pages += c->stats.invalidated_pages;
    for (unsigned i = 0; i < CACHE_PAGES; ++i) {
        struct code_page *p = c->pages[i];
        if (!p)
            continue;
        code_dependency_remove(&p->dependency);
        hal_free_pages(p, (sizeof(*p) + 4095) / 4096);
    }
    v->decoded = NULL;
    code_unlock();
    hal_free_pages(c, (sizeof(*c) + 4095) / 4096);
}

void ia64_decode_bundle(uint64_t low, uint64_t high, gpa_t gpa, struct ia64_bundle *bundle)
{
    /* Each instruction is completely assigned by the decoder below.
     * Clear only the header (including padding), not those three outputs
     * just before overwriting them. The MLX fixup can leave its flag alone. */
    memset(bundle, 0, offsetof(struct ia64_bundle, insn));
    bundle->low = low;
    bundle->high = high;
    bundle->template_code = ia64_bundle_template_code(low);
    const IA64TemplateInfo *t = ia64_template_info(bundle->template_code);
    uint64_t slots[3];
    for (unsigned i = 0; i < 3; ++i) {
        slots[i] = ia64_bundle_slot(low, high, i);
        bundle->insn[i] = ia64_decode_insn(t->units[i], slots[i], gpa, i);
    }
    ia64_apply_mlx_long_fixup(bundle->template_code, slots, 1, &bundle->insn[1],
                              &bundle->long_instruction);
    for (unsigned i = 0; i < 3; ++i) {
        Ia64Instruction *insn = &bundle->insn[i];
        bool stop = t->stop_after[bundle->long_instruction && i == 1 ? 2 : i];
        switch (insn->opcode) {
        case IA64_OP_CLRRRB:
        case IA64_OP_CLRRRB_PR:
        case IA64_OP_BSW0:
        case IA64_OP_BSW1:
        case IA64_OP_COVER:
        case IA64_OP_ITC_I:
        case IA64_OP_ITC_D:
        case IA64_OP_RFI:
            insn->placement_illegal = !stop;
            break;
        default:
            break;
        }
    }
}
static unsigned page_hash(gpa_t base)
{
    return (((base >> 12) ^ (base >> 20)) & (CACHE_PAGES / 2 - 1)) * 2;
}

static struct ia64_result get_bundle_locked(struct vcpu *v, gpa_t gpa,
                                            const struct ia64_bundle **bundle, bool *changed)
{
    if ((gpa & 15) || (gpa >> (IA64_IMPL_PA_MSB + 1)))
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "invalid instruction address");
    /* Callers validate a valid entry: the interpreter's code_page_enter
     * compares this bundle once per stay, and Tier0 publication compares
     * every bundle a region was built from (validity.c). */
    const struct gm_region *r;
    if (tier0_is_shadow(v)) {
        /* A lockstep replay copy (the real step changed this bundle) decodes
         * from its own overlay view and never fills the shared cache. */
        r = ia64_physical_region(v, gpa, 16);
        if (!r)
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unmapped replay fetch");
        struct ia64_bundle *replay = tier0_shadow_bundle(v);
        uint64_t low, high;
        const void *raw = r->hva + (size_t)(gpa - r->base);
        if ((uintptr_t)raw & 15)
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unaligned bundle backing");
        if (!ia64_atomic_read16((void *)raw, &low, &high))
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, gpa, "host cmpxchg16b unavailable");
        ia64_decode_bundle(low, high, gpa, replay);
        *bundle = replay;
        return ia64_complete();
    }
    if (!code_cache_vcpu_init(v))
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "decoded cache context allocation failed");
    struct decoded_cache *c = v->decoded;
    struct code_page **pages = c->pages;
    unsigned *recent = c->recent;
    gpa_t base = gpa & ~UINT64_C(4095);
    unsigned hash = page_hash(base), at = (gpa & 4095) >> 4;
    struct code_page *p = pages[hash];
    if (!p || p->base != base) {
        if (pages[hash + 1] && pages[hash + 1]->base == base)
            ++hash;
        else if (p && pages[hash + 1])
            hash += recent[hash / 2] ^ 1;
        else if (p)
            ++hash;
        p = pages[hash];
    }
    if (p && p->base == base && ((valid_word(p, at / 64) >> (at % 64)) & 1)) {
        __atomic_store_n(&c->stats.hits, __atomic_load_n(&c->stats.hits, __ATOMIC_RELAXED) + 1,
                         __ATOMIC_RELAXED);
        recent[hash / 2] = hash & 1;
        *bundle = &p->bundle[at];
        return ia64_complete();
    }
    r = ia64_physical_region(v, gpa, 16);
    if (!r)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unmapped instruction fetch");
    if (r->kind == GM_EMULATED || r->kind == GM_DIRECT_MMIO)
        return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, gpa, "instruction fetch from MMIO");
    if (!p) {
        p = hal_alloc_pages((sizeof(*p) + 4095) / 4096, HAL_PAGE_ZERO);
        if (!p)
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "decoded cache allocation failed");
        pages[hash] = p;
        p->dependency.invalidate = invalidate_decoded;
        p->owner = c;
    }
    if (p->base != base) {
        code_dependency_remove(&p->dependency);
        for (unsigned word = 0; word < 4; ++word)
            __atomic_store_n(&p->valid[word], 0, __ATOMIC_RELEASE);
        memset(p->dependency.mask, 0, sizeof(p->dependency.mask));
        p->base = base;
    }
    uint64_t low = 0, high = 0;
    void *raw = r->hva + (size_t)(gpa - r->base);
    if ((uintptr_t)raw & 15)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unaligned bundle backing");
    uint64_t generation;
    /* NONE retains the original atomic live fetch, without snapshots. */
    struct ia64_result source = code_source_read(v, gpa, &low, &high, &generation);
    if (source.kind != IA64_STEP_COMPLETE)
        return source;
    ia64_decode_bundle(low, high, gpa, &p->bundle[at]);
#ifdef IA64VMM_HOSTED
    code_validity_test_hook(base, CODE_TEST_DECODE);
#endif
    if (!code_source_publish(v, gpa, generation)) {
        *changed = true;
        return ia64_exit(VCPU_EXIT_BUDGET, gpa, "source changed before decode publication");
    }
    __atomic_fetch_or(&p->valid[at / 64], UINT64_C(1) << (at % 64), __ATOMIC_RELEASE);
    code_dependency_extend(&p->dependency, at);
    if (!p->dependency.page)
        code_dependency_attach(base, &p->dependency);
    __atomic_store_n(&c->stats.decodes, __atomic_load_n(&c->stats.decodes, __ATOMIC_RELAXED) + 1,
                     __ATOMIC_RELAXED);
    recent[hash / 2] = hash & 1;
    *bundle = &p->bundle[at];
    return ia64_complete();
}

static struct ia64_result get_bundle(struct vcpu *v, gpa_t gpa, const struct ia64_bundle **bundle,
                                     bool *changed)
{
    bool shadow = v->check_context && tier0_is_shadow(v);
    if (!shadow && v->decoded && ((struct decoded_cache *)v->decoded)->owner == v && !(gpa & 15)) {
        struct decoded_cache *c = v->decoded;
        gpa_t base = gpa & ~UINT64_C(4095);
        unsigned hash = page_hash(base), at = (gpa & 4095) >> 4;
        for (unsigned way = 0; way < 2; ++way) {
            struct code_page *p = c->pages[hash + way];
            if (p && p->base == base && ((valid_word(p, at / 64) >> (at % 64)) & 1)) {
                __atomic_store_n(&c->stats.hits,
                                 __atomic_load_n(&c->stats.hits, __ATOMIC_RELAXED) + 1,
                                 __ATOMIC_RELAXED);
                c->recent[hash / 2] = way;
                *bundle = &p->bundle[at];
                return ia64_complete();
            }
        }
    }
    bool take = !code_lock_held();
    if (take)
        code_lock();
    struct ia64_result result = get_bundle_locked(v, gpa, bundle, changed);
    if (take)
        code_unlock();
    return result;
}

static __attribute__((noinline)) struct ia64_result
get_bundle_checked(struct vcpu *v, gpa_t gpa, const struct ia64_bundle **bundle, bool *changed)
{
    if (!tier0_check_code(v, gpa))
        return ia64_exit(VCPU_EXIT_DEBUG, gpa, "lockstep source capture failed");
    /* Single-vCPU checks still capture the page before any instruction
     * effect, including when the decoded bundle is already warm. */
    if (!tier0_is_shadow(v))
        (void)ia64_physical_region(v, gpa, 16);
    struct ia64_result result = get_bundle(v, gpa, bundle, changed);
    if (result.kind == IA64_STEP_COMPLETE && !tier0_check_bundle(v, gpa, *bundle))
        return ia64_exit(VCPU_EXIT_DEBUG, gpa, "lockstep selected source capture failed");
    return result;
}

struct ia64_result code_get_bundle(struct vcpu *v, gpa_t gpa, const struct ia64_bundle **bundle)
{
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        bool changed = false;
        struct ia64_result result = v->check_context ? get_bundle_checked(v, gpa, bundle, &changed)
                                                     : get_bundle(v, gpa, bundle, &changed);
        if (!changed)
            return result;
    }
    return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "source changed repeatedly during decoding");
}

static void invalidate_locked(gpa_t gpa, uint64_t len)
{
    if (!len)
        return;
    code_validity_invalidate(gpa, len);
    if (code_validity_active())
        return;
    tier0_invalidate(gpa, len);
    gpa_t last = len - 1 > UINT64_MAX - gpa ? UINT64_MAX : gpa + len - 1;
    gpa_t first_page = gpa & ~UINT64_C(4095), last_page = last & ~UINT64_C(4095);
    /* Bounded work even for a huge notification; never iterate over the
     * entire guest address space or wrap at UINT64_MAX. */
    for (struct decoded_cache *c = caches; c; c = c->next)
        for (unsigned i = 0; i < CACHE_PAGES; ++i) {
            struct code_page *p = c->pages[i];
            if (p && p->base >= first_page && p->base <= last_page &&
                (p->valid[0] | p->valid[1] | p->valid[2] | p->valid[3])) {
                uint64_t mask[4];
                code_bundle_mask(p->base, gpa, len, mask);
                bool changed = false;
                for (unsigned word = 0; word < 4; ++word)
                    changed |= (p->valid[word] & mask[word]) != 0;
                if (changed) {
                    code_dependency_remove(&p->dependency);
                    for (unsigned word = 0; word < 4; ++word) {
                        uint64_t bits = valid_word(p, word) & ~mask[word];
                        __atomic_store_n(&p->valid[word], bits, __ATOMIC_RELEASE);
                        p->dependency.mask[word] = bits;
                    }
                    if (p->valid[0] | p->valid[1] | p->valid[2] | p->valid[3])
                        code_dependency_attach(p->base, &p->dependency);
                    __atomic_add_fetch(&c->stats.invalidated_pages, 1, __ATOMIC_RELAXED);
                }
            }
        }
}
static void invalidate(gpa_t gpa, uint64_t len)
{
    if (!len)
        return;
    if (code_validity_active()) {
        gpa_t last = len - 1 > UINT64_MAX - gpa ? UINT64_MAX : gpa + len - 1;
        gpa_t first = gpa & ~UINT64_C(4095), end = last & ~UINT64_C(4095);
        /* Huge cache-flush notifications go straight to the bounded record
         * walk; ordinary stores need at most two bitmap loads. */
        if ((end - first) / 4096 < 16) {
            bool tracked = false;
            for (gpa_t at = first;; at += 4096) {
                if (code_page_tracked_fast(at)) {
                    tracked = true;
                    break;
                }
                if (at == end)
                    break;
            }
            if (!tracked)
                return;
        }
    }
    bool take = !code_lock_held();
    if (take)
        code_lock();
    invalidate_locked(gpa, len);
    if (take)
        code_unlock();
}
void code_notify_vmm_write(gpa_t gpa, uint64_t len)
{
    if (!tier0_shadow_active())
        invalidate(gpa, len);
}
void code_notify_cpu_write(gpa_t gpa, uint64_t len)
{
    if (!tier0_shadow_active())
        invalidate(gpa, len);
}
bool code_cpu_write_begin(gpa_t gpa, uint64_t len)
{
    if (!len || tier0_shadow_active())
        return false;
    if (!code_validity_active()) {
        code_notify_cpu_write(gpa, len);
        return false;
    }
    gpa_t last = len - 1 > UINT64_MAX - gpa ? UINT64_MAX : gpa + len - 1;
    gpa_t page = gpa & ~UINT64_C(4095), last_page = last & ~UINT64_C(4095);
    while (!code_page_tracked_fast(page)) {
        if (page == last_page)
            return false;
        page += 4096;
    }
    bool take = !code_lock_held();
    if (take)
        code_lock();
    /* Keep preparation from republishing pre-store bytes after invalidation
     * (the untracked race remains, without an extra fence). */
    code_notify_cpu_write(gpa, len);
#ifdef IA64VMM_HOSTED
    code_validity_test_hook(gpa & ~UINT64_C(4095), CODE_TEST_STORE);
#endif
    return take;
}
void code_cpu_write_end(bool acquired)
{
    if (acquired)
        code_unlock();
}
bool code_vmm_write_begin(gpa_t gpa, uint64_t len)
{
    return code_cpu_write_begin(gpa, len);
}

void code_cache_get_stats(struct code_cache_stats *out)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    *out = destroyed_stats;
    for (struct decoded_cache *c = caches; c; c = c->next) {
        out->hits += __atomic_load_n(&c->stats.hits, __ATOMIC_RELAXED);
        out->decodes += __atomic_load_n(&c->stats.decodes, __ATOMIC_RELAXED);
        out->invalidated_pages += __atomic_load_n(&c->stats.invalidated_pages, __ATOMIC_RELAXED);
    }
    if (take)
        code_unlock();
}
void code_cache_flush(void)
{
    code_background_quiesce();
    bool take = !code_lock_held();
    if (take)
        code_lock();
    tier0_flush();
    for (struct decoded_cache *c = caches; c; c = c->next)
        for (unsigned i = 0; i < CACHE_PAGES; ++i) {
            struct code_page *p = c->pages[i];
            if (!p)
                continue;
            code_dependency_remove(&p->dependency);
            for (unsigned word = 0; word < 4; ++word)
                __atomic_store_n(&p->valid[word], 0, __ATOMIC_RELEASE);
            memset(p->dependency.mask, 0, sizeof(p->dependency.mask));
        }
    code_validity_flush();
    if (take)
        code_unlock();
    code_background_resume();
}
bool code_page_has_code(gpa_t page)
{
    if (code_validity_active())
        return code_page_tracked_fast(page);
    bool take = !code_lock_held();
    if (take)
        code_lock();
    unsigned hash = page_hash(page);
    bool found = false;
    for (struct decoded_cache *c = caches; c && !found; c = c->next)
        for (unsigned way = 0; way < 2; ++way) {
            const struct code_page *p = c->pages[hash + way];
            if (p && p->base == page && valid_any(p)) {
                found = true;
                break;
            }
        }
    found = found || tier0_page_has_code(page) || code_page_has_dependencies(page);
    if (take)
        code_unlock();
    return found;
}
