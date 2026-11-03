/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/boot_diag.h"
#include "host/amd64/boot_walk.h"
#include "host/amd64/amd64.h"
#include "host/amd64/smp.h"
#include "ia64vmm/log.h"

struct cpu_snapshot {
    uint64_t cr3, cr4, efer, pat;
    bool valid, have_pat;
};
static struct {
    struct cpu_snapshot cpu[2];
    uint64_t hhdm, base, size, target, bytes, cr3;
    struct boot_mem_range source;
    unsigned source_index, metadata;
    bool reserved, have_source;
    struct amd64_boot_walk walk;
} diag;
static struct amd64_boot_probe probe;
extern bool amd64_boot_read64(uint64_t address, uint64_t *value);
extern char amd64_boot_read64_fault[];

void amd64_boot_diag_cpu(bool after, uint64_t hhdm)
{
    struct cpu_snapshot *s = &diag.cpu[after];
    diag.hhdm = hhdm;
    __asm__ volatile("mov %%cr3,%0; mov %%cr4,%1" : "=r"(s->cr3), "=r"(s->cr4));
    s->efer = amd64_rdmsr(0xc0000080);
    uint32_t a, b, c, d;
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    s->have_pat = (d & (1u << 16)) != 0;
    if (s->have_pat)
        s->pat = amd64_rdmsr(0x277);
    s->valid = true;
}
void amd64_boot_diag_reserve(const struct boot_info *bi, uint64_t base, uint64_t size)
{
    diag.base = base;
    diag.size = size;
    diag.reserved = true;
    for (unsigned i = 0; i < bi->mem_range_count; ++i) {
        const struct boot_mem_range *m = &bi->mem[i];
        if (m->type == BOOT_MEM_USABLE && base >= m->base && base - m->base <= m->size &&
            size <= m->size - (base - m->base)) {
            diag.source = *m;
            diag.source_index = i;
            diag.have_source = true;
            break;
        }
    }
}
static bool early_bsp(void)
{
    /* IA32_APIC_BASE.BSP identifies the bootstrap processor independently
     * of GS/per-CPU storage, which does not exist during this probe. */
    return !amd64_cpus && (amd64_rdmsr(0x1b) & (UINT64_C(1) << 8));
}
static bool read_entry(void *ctx, uint64_t address, uint64_t *value)
{
    (void)ctx;
    if (!early_bsp())
        return false;
    probe.rip = (uintptr_t)amd64_boot_read64;
    probe.address = address;
    __atomic_store_n(&probe.armed, true, __ATOMIC_RELEASE);
    bool ok = amd64_boot_read64(address, value);
    __atomic_store_n(&probe.armed, false, __ATOMIC_RELEASE);
    return ok;
}
bool amd64_boot_diag_recover(uint64_t vector, uint64_t error, uint64_t *rip, uint64_t cr2)
{
    if (!__atomic_load_n(&probe.armed, __ATOMIC_ACQUIRE))
        return false;
    if (!amd64_boot_probe_matches(&probe, early_bsp(), vector, error, *rip, cr2))
        return false;
    *rip = (uintptr_t)amd64_boot_read64_fault;
    return true;
}
void amd64_boot_diag_metadata(uint64_t address, uint64_t bytes)
{
    diag.target = address;
    diag.bytes = bytes;
    diag.metadata = 1;
    uint64_t cr4;
    __asm__ volatile("mov %%cr3,%0; mov %%cr4,%1" : "=r"(diag.cr3), "=r"(cr4));
    if (cr4 & (UINT64_C(1) << 12)) {
        diag.walk.status = AMD64_BOOT_INVALID;
        return;
    }
    amd64_boot_walk(diag.cr3, diag.hhdm, address, read_entry, NULL, &diag.walk);
}
void amd64_boot_diag_written(void)
{
    diag.metadata = 2;
}
bool amd64_boot_diag_available(void)
{
    return diag.cpu[0].valid;
}

#define X(v) ((unsigned long long)(v))
void amd64_boot_diag_dump(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        const struct cpu_snapshot *s = &diag.cpu[i];
        if (!s->valid)
            continue;
        LOGI("boot %s: CR3=%016llx CR4=%016llx", i ? "post" : "pre", X(s->cr3), X(s->cr4));
        LOGI("boot %s: EFER=%016llx PAT=%016llx%s", i ? "post" : "pre", X(s->efer), X(s->pat),
             s->have_pat ? "" : " (absent)");
    }
    LOGI("boot HHDM=%016llx", X(diag.hhdm));
    if (!diag.reserved)
        return;
    LOGI("boot reserve: base=%016llx size=%016llx", X(diag.base), X(diag.size));
    if (diag.have_source)
        LOGI("boot map[%u]: base=%016llx size=%016llx type=%u", diag.source_index,
             X(diag.source.base), X(diag.source.size), (unsigned)diag.source.type);
    else
        LOGI("boot map: reservation source unavailable");
    if (!diag.metadata)
        return;
    LOGI("boot meta: VA=%016llx bytes=%llx %s", X(diag.target), X(diag.bytes),
         diag.metadata == 2 ? "written" : "write pending");
    static const char *const states[] = {"untried", "present", "not-present", "unreadable",
                                         "invalid"};
    LOGI("boot walk: CR3=%016llx %s", X(diag.cr3), states[diag.walk.status]);
    for (unsigned i = 0; i < diag.walk.count; ++i)
        LOGI("boot L%u: slot=%016llx PTE=%016llx%s", 4 - i, X(diag.walk.slot[i]),
             X(diag.walk.entry[i]),
             diag.walk.status == AMD64_BOOT_UNREADABLE && i + 1 == diag.walk.count ? " unread"
                                                                                   : "");
    if (diag.walk.status == AMD64_BOOT_PRESENT)
        LOGI("boot leaf: PA=%016llx size=%llx RW=%u", X(diag.walk.physical),
             X(diag.walk.page_bytes), (unsigned)diag.walk.writable);
}
