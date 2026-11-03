/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/platform.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"

/* PCI Local Bus 3.0 §3.3.3: subtractive decode of an unclaimed memory
 * transaction ends in master abort. This is a bus completion, not RAM.
 * Keep diagnostics sparse: one bitmap page covers 64 MiB of address space.
 * RAM reservations remain absent from gm_* and EFI allocations. */
struct platform_abort_log {
    struct platform_abort_log *next;
    uint64_t block;
    uint8_t bits[2048];
};
_Static_assert(sizeof(struct platform_abort_log) <= 4096, "master-abort bitmap page");

static void abort_log_unlocked(struct platform_abort_range *range, struct vcpu *v, uint64_t off,
                               unsigned size, bool write)
{
    struct platform_state *s = range->state;
    uint64_t address = range->base + off, block = address >> 26;
    unsigned bit = (address >> 12) & 16383;
    struct platform_abort_log *log = s->abort_log;
    while (log && log->block != block)
        log = log->next;
    if (!log) {
        log = hal_alloc_pages(1, HAL_PAGE_ZERO);
        if (!log) {
            LOGE("PCI master-abort diagnostic allocation failed");
            if (v)
                vcpu_request_exit(v, VCPU_EXIT_BAD_ACCESS);
            return;
        }
        log->block = block;
        log->next = s->abort_log;
        s->abort_log = log;
    }
    if (log->bits[bit / 8] & (1u << (bit & 7)))
        return;
    log->bits[bit / 8] |= 1u << (bit & 7);
    unsigned slot = 0;
    uint64_t ip = v ? vcpu_instruction_pointer(v, &slot) : 0;
    LOGW("PCI master abort page=%llx address=%llx %s size=%u ip=%llx slot=%u",
         (unsigned long long)(address & ~UINT64_C(4095)), (unsigned long long)address,
         write ? "write" : "read", size, (unsigned long long)ip, slot);
}
static void abort_log(struct platform_abort_range *range, struct vcpu *v, uint64_t off,
                      unsigned size, bool write)
{
    platform_lock(&range->state->abort_lock);
    abort_log_unlocked(range, v, off, size, write);
    platform_unlock(&range->state->abort_lock);
}

static uint64_t abort_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    abort_log(opaque, v, off, size, false);
    return size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}
static void abort_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    (void)value;
    abort_log(opaque, v, off, size, true);
}

static struct platform_state *active;

/* A bus fallback, separate from the RAM/device region table. In particular
 * gm_direct_ptr and VMM-originated copies never acquire synthetic backing.
 * The ranges describe the gm table as it was when they were built; a later
 * gm_add_region would make them stale, so they then answer nothing (the CPU
 * reports the access as unbacked, as before). */
const struct gm_region *platform_unclaimed_region(gpa_t address, unsigned size)
{
    if (!active || !size || gm_region_count() != active->abort_regions_seen)
        return NULL;
    unsigned lo = 0, hi = active->abort_range_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (active->abort_ranges[mid].region.base <= address)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return NULL;
    const struct gm_region *r = &active->abort_ranges[lo - 1].region;
    return address - r->base < r->size && size <= r->size - (address - r->base) ? r : NULL;
}

/* A PC chipset forwards an address that is neither DRAM nor a claimed
 * device to PCI by subtractive decode, and an unclaimed transaction ends in
 * master abort (PCI 3.0 §3.3.3): every gap of the implemented physical
 * space below 2^IA64VMM_PHYS_ADDR_BITS is such a range. That includes the
 * space above guest RAM, holes between RAM ranges, the VMM's reservation
 * and host-reserved RAM, none of which is a gm region and none of which is
 * ever backed by memory here. Build after RAM, framebuffer, devices and
 * assigned BARs have registered (platform_devices_init calls this last).
 * Out-of-range PAs stay with the CPU MMU (Unimplemented Data Address). */
bool platform_abort_init(struct platform_state *s)
{
    static const struct gm_mmio_ops ops = {abort_read, abort_write};
    const uint64_t limit = UINT64_C(1) << IA64VMM_PHYS_ADDR_BITS;
    const unsigned n = gm_region_count();
    uint64_t cursor = 0;
    s->abort_range_count = 0;
    /* One gap at most before each region and one after the last: n + 1. */
    for (unsigned i = 0; i <= n && cursor < limit; ++i) {
        const struct gm_region *r = i < n ? gm_region_at(i) : NULL;
        uint64_t next = r && r->base < limit ? r->base : limit;
        if (next > cursor) {
            if (s->abort_range_count == IA64VMM_ARRAY_SIZE(s->abort_ranges))
                return false;
            struct platform_abort_range *range = &s->abort_ranges[s->abort_range_count++];
            range->base = cursor;
            range->state = s;
            range->region = (struct gm_region){
                cursor, next - cursor, GM_EMULATED, "pci-master-abort", NULL, &ops, range};
        }
        if (r && r->base + r->size > cursor)
            cursor = r->base + r->size;
    }
    s->abort_regions_seen = n;
    active = s;
    return true;
}

void platform_abort_cleanup(struct platform_state *s)
{
    if (active == s)
        active = NULL;
    while (s->abort_log) {
        struct platform_abort_log *next = s->abort_log->next;
        hal_free_pages(s->abort_log, 1);
        s->abort_log = next;
    }
}
