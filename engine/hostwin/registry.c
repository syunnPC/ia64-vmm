/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Instance registry and code-page event consumer.
 * hostwin_create/hostwin_destroy run with all executors
 * stopped; the event consumer runs on the thread that triggered the
 * tracking, holds each instance's lock and reaches its CPU through the
 * backend's synchronous flush. hostwin_owns is read by the hosted SIGSEGV
 * handler: the registry changes only while no executor runs, so plain
 * reads are safe there.
 */
#include "engine/hostwin/internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

static struct hostwin_vcpu *instances[HW_MAX_INSTANCES];
static unsigned instance_count;

/* Weak default for tests without a window backend; a host
 * layer that has one defines hostwin_host_backend (hostwin.h). */
__attribute__((weak)) const struct hostwin_backend *hostwin_host_backend(void **ctx)
{
    *ctx = NULL;
    return NULL;
}

struct hostwin_vcpu *hostwin_create(const struct hostwin_config *cfg,
                                    const struct hostwin_backend *backend, void *ctx)
{
    /* w >= 12 keeps whole pages inside a slot; w <= 53 is the bare-metal
     * maximum; the slot array is fixed at HOSTWIN_MAX_SLOTS. */
    if (!cfg || !backend || cfg->w < 12 || cfg->w > 53 || !cfg->slots ||
        cfg->slots > HOSTWIN_MAX_SLOTS || !backend->reserve || !backend->release || !backend->map ||
        !backend->unmap || !backend->protect_ro || !backend->flush) {
        LOGE("hostwin: invalid configuration");
        return NULL;
    }
    if (instance_count == HW_MAX_INSTANCES) {
        LOGE("hostwin: instance registry full");
        return NULL;
    }
    size_t pages = (sizeof(struct hostwin_vcpu) + HW_PAGE - 1) / HW_PAGE;
    struct hostwin_vcpu *hw = hal_alloc_pages(pages, HAL_PAGE_ZERO);
    if (!hw) {
        LOGE("hostwin: instance allocation failed");
        return NULL;
    }
    memset(hw, 0, sizeof(*hw));
    hw->backend = backend;
    hw->ctx = ctx;
    hw->w = cfg->w;
    hw->slots = cfg->slots;
    hw->self_pages = pages;
    /* The first hostwin_enter takes the snapshot (full path). */
    hw->check = cfg->check;
    hw->force = true;
    if (cfg->check) {
        hw->image_pages = (sizeof(*hw->image) + HW_PAGE - 1) / HW_PAGE;
        hw->image = hal_alloc_pages(hw->image_pages, HAL_PAGE_ZERO);
        if (!hw->image) {
            LOGE("hostwin: check image allocation failed");
            hal_free_pages(hw, pages);
            return NULL;
        }
    }
    if (!hw_shadows_init(hw)) {
        LOGE("hostwin: shadow pool allocation failed");
        if (hw->image)
            hal_free_pages(hw->image, hw->image_pages);
        hal_free_pages(hw, pages);
        return NULL;
    }
    if (!backend->reserve(ctx, hw->w, hw->slots, &hw->base, &hw->poison)) {
        LOGE("hostwin: window reservation failed (w=%u slots=%u)", hw->w, hw->slots);
        hw_shadows_fini(hw);
        if (hw->image)
            hal_free_pages(hw->image, hw->image_pages);
        hal_free_pages(hw, pages);
        return NULL;
    }
    instances[instance_count++] = hw;
    return hw;
}

const char *hostwin_stop_detail(const struct hostwin_vcpu *hw)
{
    return hw->stop_detail;
}

void hostwin_destroy(struct hostwin_vcpu *hw)
{
    if (!hw)
        return;
    unsigned i = 0;
    while (i < instance_count && instances[i] != hw)
        ++i;
    if (i == instance_count)
        hal_panic("hostwin_destroy: unknown instance");
    instances[i] = instances[--instance_count];
    instances[instance_count] = NULL;
    /* Releasing the whole reservation removes every shadow at once. */
    hw->backend->release(hw->ctx, hw->base, hw->w, hw->slots);
    hw_shadows_fini(hw);
    if (hw->image)
        hal_free_pages(hw->image, hw->image_pages);
    hal_free_pages(hw, hw->self_pages);
}

unsigned hostwin_bits(const struct hostwin_vcpu *hw)
{
    return hw->w;
}
uintptr_t hostwin_poison_va(const struct hostwin_vcpu *hw)
{
    return hw->poison;
}

void hostwin_get_stats(const struct hostwin_vcpu *hw, struct hostwin_stats *out)
{
    hw_lock(hw);
    *out = hw->stats;
    out->live_mappings = hw->live_count;
    hw_unlock(hw);
    out->context_unchanged = __atomic_load_n(&hw->unchanged, __ATOMIC_RELAXED);
}

#ifdef IA64VMM_HOSTED
void hostwin_test_shadows(const struct hostwin_vcpu *hw, hostwin_test_shadow_fn fn, void *arg)
{
    hw_lock(hw);
    for (const struct hw_shadow *s = hw->live; s; s = s->live_next) {
        const struct hw_slot *slot = &hw->slot[s->slot];
        uint64_t gva = (slot->tag << hw->w) + (s->host_page - hw_slot_base(hw, s->slot));
        fn(gva, s->pa_page, s->writable, arg);
    }
    hw_unlock(hw);
}
#endif

bool hostwin_owns(uintptr_t host_va)
{
    for (unsigned i = 0; i < instance_count; ++i) {
        const struct hostwin_vcpu *hw = instances[i];
        uintptr_t end = hw_slot_base(hw, hw->slots);
        if ((host_va >= hw->base && host_va < end) ||
            (host_va >= hw->poison && host_va - hw->poison < HW_PAGE))
            return true;
    }
    return false;
}

/* TRACKED write-protects every alias in every instance before
 * prepare() reads the snapshot; UNTRACKED needs no action (the next write
 * fault re-evaluates code_page_has_code). Never calls engine/cache.
 * Runs under code_lock on any thread, after the tracked bit was published;
 * each instance's lock orders it against its owner's fills. */
void hostwin_code_page_event(gpa_t page, enum code_track_event event)
{
    for (unsigned i = 0; i < instance_count; ++i) {
        struct hostwin_vcpu *hw = instances[i];
        hw_lock(hw);
        ++hw->stats.code_events;
        if (event == CODE_PAGE_TRACKED)
            hw_shadows_protect_pa(hw, page & HW_PAGE_MASK);
        hw_unlock(hw);
    }
}

void hw_panic(const struct hostwin_vcpu *hw, const char *what, uintptr_t host_page)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < instance_count; ++i)
        total += instances[i]->live_count;
    char message[320];
    /* Hosted, each isolated shadow splits the reservation: at most
     * 2 * live + 1 VMAs per instance. */
    vmm_snprintf(message, sizeof(message),
                 "hostwin: backend %s failed at host page %llx: instance live shadows %llu "
                 "(cap %u, VMA bound %llu), all instances %u with %llu live shadows",
                 what, (unsigned long long)host_page, (unsigned long long)(hw ? hw->live_count : 0),
                 HOSTWIN_MAX_SHADOWS, (unsigned long long)(hw ? 2 * hw->live_count + 1 : 0),
                 instance_count, (unsigned long long)total);
    hal_panic(message);
}
