/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/engine.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

static struct gm_region regions[GM_MAX_REGIONS];
static unsigned region_count;

__attribute__((weak)) void code_notify_vmm_write(gpa_t gpa, uint64_t len)
{
    (void)gpa;
    (void)len;
}

__attribute__((weak)) void code_notify_cpu_write(gpa_t gpa, uint64_t len)
{
    (void)gpa;
    (void)len;
}

/* CPU-only fixtures have no code validator; the engine overrides these. */
__attribute__((weak)) bool code_cpu_write_begin(gpa_t gpa, uint64_t len)
{
    code_notify_cpu_write(gpa, len);
    return false;
}
__attribute__((weak)) void code_cpu_write_end(bool acquired)
{
    (void)acquired;
}
/* Keep engine-free gm_write fixtures on the VMM notification contract. */
__attribute__((weak)) bool code_vmm_write_begin(gpa_t gpa, uint64_t len)
{
    code_notify_vmm_write(gpa, len);
    return false;
}

bool gm_add_region(const struct gm_region *r)
{
    if (!r || !r->size || r->size > UINT64_MAX - r->base || (unsigned)r->kind > GM_EMULATED ||
        (r->kind == GM_EMULATED ? !r->ops : !r->hva)) {
        LOGE("invalid guest region");
        return false;
    }
    if (region_count == GM_MAX_REGIONS) {
        LOGE("guest region table exhausted");
        return false;
    }
    unsigned at = 0;
    while (at < region_count && regions[at].base < r->base)
        ++at;
    if ((at && regions[at - 1].size > r->base - regions[at - 1].base) ||
        (at < region_count && r->size > regions[at].base - r->base)) {
        LOGE("overlapping guest region %s at %llx", r->name ? r->name : "?",
             (unsigned long long)r->base);
        return false;
    }
    memmove(&regions[at + 1], &regions[at], (region_count - at) * sizeof(*regions));
    regions[at] = *r;
    ++region_count;
    return true;
}

const struct gm_region *gm_lookup(gpa_t gpa)
{
    unsigned lo = 0, hi = region_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (regions[mid].base <= gpa)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return NULL;
    const struct gm_region *r = &regions[lo - 1];
    return gpa - r->base < r->size ? r : NULL;
}

void *gm_direct_ptr(gpa_t gpa, uint64_t len)
{
    const struct gm_region *r = gm_lookup(gpa);
    if (!r || r->kind == GM_EMULATED || len > SIZE_MAX || len > r->size - (gpa - r->base))
        return NULL;
    return r->hva + (size_t)(gpa - r->base);
}

bool gm_write(gpa_t gpa, const void *src, uint64_t len)
{
    void *dst = gm_direct_ptr(gpa, len);
    if (!dst || (len && !src))
        return false;
    bool acquired = code_vmm_write_begin(gpa, len);
    if (len)
        memcpy(dst, src, (size_t)len);
    code_cpu_write_end(acquired);
    return true;
}

bool gm_read(gpa_t gpa, void *dst, uint64_t len)
{
    const void *src = gm_direct_ptr(gpa, len);
    if (!src || (len && !dst))
        return false;
    if (len)
        memcpy(dst, src, (size_t)len);
    return true;
}

unsigned gm_region_count(void)
{
    return region_count;
}
const struct gm_region *gm_region_at(unsigned index)
{
    return index < region_count ? &regions[index] : NULL;
}
