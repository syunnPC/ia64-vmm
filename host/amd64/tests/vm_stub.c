/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit AMD64_HOST_ONLY=1 smoke harness while jobs A/C are incomplete.
 * Exercises the host lifecycle without executing a guest. */
#include "ia64vmm/bootinfo.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "core/vm.h"

void vmm_main(const struct boot_info *bi)
{
    LOGW("AMD64_HOST_ONLY smoke harness: platform/engine intentionally absent");
    LOGI("IA-64 VMM on %s (%u host CPU)", bi->host_name, bi->host_cpu_count);
    for (unsigned i = 0; i < bi->mem_range_count; ++i) {
        const struct boot_mem_range *m = &bi->mem[i];
        LOGI("host mem %016llx size %016llx type=%u hva=%p", (unsigned long long)m->base,
             (unsigned long long)m->size, (unsigned)m->type, (void *)m->hva);
        if (m->type != BOOT_MEM_USABLE)
            continue;
        uint64_t start = IA64VMM_ALIGN_UP(m->base, IA64VMM_PAGE_SIZE);
        uint64_t end = IA64VMM_ALIGN_DOWN(m->base + m->size, IA64VMM_PAGE_SIZE);
        if (end <= start)
            continue;
        struct gm_region r = {.base = start,
                              .size = end - start,
                              .kind = GM_RAM,
                              .hva = m->hva + start - m->base,
                              .name = "ram"};
        if (!gm_add_region(&r))
            hal_panic("host smoke guest map failed");
    }
    if (bi->fb.hva) {
        struct gm_region r = {.base = bi->fb.base,
                              .size = bi->fb.size,
                              .hva = bi->fb.hva,
                              .kind = GM_DIRECT_MMIO,
                              .name = "gop-fb"};
        if (!gm_add_region(&r))
            hal_panic("host smoke framebuffer map failed");
    }
    for (unsigned i = 0; i < bi->module_count; ++i)
        LOGI("module %s hpa=%016llx size=%llu", bi->modules[i].name,
             (unsigned long long)bi->modules[i].hpa, (unsigned long long)bi->modules[i].size);
    for (unsigned i = 0; i < gm_region_count(); ++i) {
        const struct gm_region *r = gm_region_at(i);
        LOGI("guest region %s base=%016llx size=%016llx kind=%u", r->name,
             (unsigned long long)r->base, (unsigned long long)r->size, (unsigned)r->kind);
    }
    hal_shutdown(HAL_SHUTDOWN_POWER_OFF, VMM_EXIT_SETUP);
}
