/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/memory.h"
#include "engine/interp/interp.h"
#include "ia64vmm/platform.h"

/* Standalone instruction/oracle fixtures have no platform bus apertures. */
__attribute__((weak)) const struct gm_region *platform_unclaimed_region(gpa_t address,
                                                                        unsigned size)
{
    (void)address;
    (void)size;
    return NULL;
}

/* Opt-in observation of completed display MMIO (vbios_trace_guest), with no additional
 * device transaction. Standalone instruction fixtures have no observer. */
__attribute__((weak)) void platform_display_memory_access(struct vcpu *v, uint64_t address,
                                                          unsigned size, bool write, uint64_t value)
{
    (void)v;
    (void)address;
    (void)size;
    (void)write;
    (void)value;
}

const struct gm_region *ia64_physical_region(struct vcpu *v, gpa_t gpa, unsigned size)
{
    if (gpa >> (IA64_IMPL_PA_MSB + 1))
        return NULL;
    if (v->memory_hook)
        return v->memory_hook(v, gpa, size);
    uint64_t page = gpa & ~UINT64_C(4095);
    const struct gm_region *r = v->physical_region;
    /* A region spans an address range, not one guest page. gm_add_region()
     * rejects wrapping ranges, so unsigned subtraction also rejects an
     * address below base. Hooks and the full access width remain checked. */
    if (!r || gpa - r->base >= r->size) {
        r = gm_lookup(gpa);
        v->physical_region = r;
        v->physical_page = page;
    }
    if (!r)
        return platform_unclaimed_region(gpa, size);
    return size <= r->size - (gpa - r->base) ? r : NULL;
}

static unsigned device_rmw_lock;
void ia64_device_rmw_begin(void)
{
    unsigned expected = 0;
    while (!__atomic_compare_exchange_n(&device_rmw_lock, &expected, 1, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
        while (__atomic_load_n(&device_rmw_lock, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
        expected = 0;
    }
}
void ia64_device_rmw_end(void)
{
    __atomic_store_n(&device_rmw_lock, 0, __ATOMIC_RELEASE);
}

/* Lockstep record storage stays on the checking path. Ordinary RAM references
 * keep the original width-specific access without a replay-log stack frame. */
static inline __attribute__((always_inline)) uint64_t ram_read(const void *p, unsigned size,
                                                               bool acquire)
{
#define RAM_LOAD(bytes, type)                                                                      \
    case bytes:                                                                                    \
        return acquire ? __atomic_load_n((const type *)p, __ATOMIC_ACQUIRE)                        \
                       : __atomic_load_n((const type *)p, __ATOMIC_RELAXED)
    switch (size) {
        RAM_LOAD(1, uint8_t);
        RAM_LOAD(2, uint16_t);
        RAM_LOAD(4, uint32_t);
    default:
        return acquire ? __atomic_load_n((const uint64_t *)p, __ATOMIC_ACQUIRE)
                       : __atomic_load_n((const uint64_t *)p, __ATOMIC_RELAXED);
    }
#undef RAM_LOAD
}
static inline __attribute__((always_inline)) void ram_write(gpa_t gpa, void *p, unsigned size,
                                                            uint64_t value, bool release)
{
    bool acquired = code_cpu_write_begin(gpa, size);
#define RAM_STORE(bytes, type)                                                                     \
    case bytes:                                                                                    \
        if (release)                                                                               \
            __atomic_store_n((type *)p, value, __ATOMIC_RELEASE);                                  \
        else                                                                                       \
            __atomic_store_n((type *)p, value, __ATOMIC_RELAXED);                                  \
        break
    switch (size) {
        RAM_STORE(1, uint8_t);
        RAM_STORE(2, uint16_t);
        RAM_STORE(4, uint32_t);
        RAM_STORE(8, uint64_t);
    }
#undef RAM_STORE
    if (acquired)
        code_cpu_write_end(true);
}
static __attribute__((noinline)) void checked_ram_read(struct vcpu *v, gpa_t gpa, unsigned size,
                                                       uint64_t *value, bool acquire, const void *p)
{
    struct lockstep_access a;
    *value = lockstep_access_begin(v, &a, LOCKSTEP_LOAD, gpa, size) ? a.read[0]
                                                                    : ram_read(p, size, acquire);
    lockstep_access_end(v, &a, *value, 0, 0, 0, false);
}
static __attribute__((noinline)) void checked_ram_write(struct vcpu *v, gpa_t gpa, unsigned size,
                                                        uint64_t value, bool release, void *p)
{
    struct lockstep_access a;
    if (!lockstep_access_begin(v, &a, LOCKSTEP_STORE, gpa, size))
        ram_write(gpa, p, size, value, release);
    lockstep_access_end(v, &a, 0, 0, value, 0, true);
}

struct ia64_result ia64_physical_read_ordered(struct vcpu *v, gpa_t gpa, unsigned size,
                                              uint64_t *value, bool acquire)
{
    if (size != 1 && size != 2 && size != 4 && size != 8)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "invalid physical access size");
    const struct gm_region *r = ia64_physical_region(v, gpa, size);
    if (!r)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unmapped physical read");
    /* Every device access ends v's code-validity stay. */
    if (r->kind != GM_RAM && r->kind != GM_FIRMWARE)
        v->code_entry_generation = 0;
    if (r->kind == GM_EMULATED) {
        if (!r->ops->read)
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "MMIO read unavailable");
        *value = r->ops->read(r->opaque, v, gpa - r->base, size) & ia64_low_mask(size * 8);
    } else {
        const volatile uint8_t *p = r->hva + (size_t)(gpa - r->base);
        /* Direct MMIO must preserve the bus transaction width. Hosted and
         * bare-metal execution both use little-endian AMD64. */
        if (r->kind == GM_DIRECT_MMIO) {
            if ((uintptr_t)p & (size - 1))
                return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unaligned direct MMIO backing");
            switch (size) {
            case 1:
                *value = *p;
                break;
            case 2:
                *value = *(const volatile uint16_t *)p;
                break;
            case 4:
                *value = *(const volatile uint32_t *)p;
                break;
            default:
                *value = *(const volatile uint64_t *)p;
                break;
            }
            platform_display_memory_access(v, gpa, size, false, *value);
        } else {
            /* SDM Rev.2.3 Vol.2 §4.5: an aligned datum is one indivisible
             * host reference. Byte loops cannot satisfy this for SMP/DMA. */
            if ((uintptr_t)p & (size - 1))
                return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unaligned atomic RAM backing");
            if (v->access_hook)
                checked_ram_read(v, gpa, size, value, acquire, (const void *)p);
            else
                *value = ram_read((const void *)p, size, acquire);
        }
    }
    if (acquire)
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return ia64_complete();
}
struct ia64_result ia64_physical_write_ordered(struct vcpu *v, gpa_t gpa, unsigned size,
                                               uint64_t value, bool release)
{
    if (size != 1 && size != 2 && size != 4 && size != 8)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "invalid physical access size");
    const struct gm_region *r = ia64_physical_region(v, gpa, size);
    if (!r)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unmapped physical write");
    if (r->kind != GM_RAM && r->kind != GM_FIRMWARE)
        v->code_entry_generation = 0;
    /* A release store becomes visible after every earlier access, including
     * write-coalesced data (SDM Vol.2 §4.4.5: any release or fence flushes
     * the coalescing buffers first). Guest stores to the GOP framebuffer use
     * the host's write-combining mapping (PAT[7], host/amd64/memory.c), and
     * an ordinary store does not drain WC buffers; SFENCE does (Intel SDM
     * Vol.3A §11.2.2, §14.3). The other ordering points drain them already:
     * mf, mf.a, fwb, fc and the semaphores execute a LOCK-prefixed
     * instruction or MFENCE, and accesses through the host's UC device
     * mappings evict WC buffers (§14.3). */
    if (release)
        __asm__ volatile("sfence" ::: "memory");
    if (r->kind == GM_EMULATED) {
        if (!r->ops->write)
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "MMIO write unavailable");
        r->ops->write(r->opaque, v, gpa - r->base, value & ia64_low_mask(size * 8), size);
    } else {
        volatile uint8_t *p = r->hva + (size_t)(gpa - r->base);
        if ((uintptr_t)p & (size - 1))
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unaligned atomic backing");
        if (r->kind == GM_DIRECT_MMIO) {
            code_notify_cpu_write(gpa, size);
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
            platform_display_memory_access(v, gpa, size, true, value & ia64_low_mask(size * 8));
        } else {
            if (v->access_hook)
                checked_ram_write(v, gpa, size, value, release, (void *)p);
            else
                ram_write(gpa, (void *)p, size, value, release);
        }
    }
    return ia64_complete();
}
struct ia64_result ia64_physical_read(struct vcpu *v, gpa_t gpa, unsigned size, uint64_t *value)
{
    return ia64_physical_read_ordered(v, gpa, size, value, false);
}
struct ia64_result ia64_physical_write(struct vcpu *v, gpa_t gpa, unsigned size, uint64_t value)
{
    return ia64_physical_write_ordered(v, gpa, size, value, false);
}
