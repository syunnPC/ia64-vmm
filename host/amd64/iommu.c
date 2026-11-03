/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/amd64.h"
#include "host/amd64/iommu.h"
#include "host/amd64/vtd.h"
#include "host/amd64/amdvi.h"
#include "host/amd64/smp.h"
#include "ia64vmm/interrupts.h"
#include "platform/lock.h"
#include "platform/platform.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#if !defined(IA64VMM_AMD64_HOST_ONLY)
static bool protection_enabled = true;
#endif

static IA64VMM_NORETURN void handoff_error(const char *reason)
{
    char message[256];
    vmm_snprintf(message, sizeof(message), "%s; disable VT-d/AMD-Vi (IOMMU) in UEFI setup", reason);
    hal_panic(message);
}

static uint64_t read_reg(void *p, unsigned off, unsigned width)
{
    volatile uint8_t *r = p;
    return width == 4 ? *(volatile uint32_t *)(r + off) : *(volatile uint64_t *)(r + off);
}
static void write_reg(void *p, unsigned off, uint64_t value, unsigned width)
{
    volatile uint8_t *r = p;
    if (width == 4)
        *(volatile uint32_t *)(r + off) = value;
    else
        *(volatile uint64_t *)(r + off) = value;
    __asm__ volatile("mfence" ::: "memory");
}
static uint32_t dma_config(void *context, uint16_t bdf, unsigned off, unsigned width)
{
    (void)context;
    return platform_pci_host_read(bdf, off, width);
}
static void dma_config_write(void *context, uint16_t bdf, unsigned off, uint32_t value,
                             unsigned width)
{
    (void)context;
    platform_pci_host_write(bdf, off, value, width);
}
static uint8_t *map_registers(const struct boot_info *bi, uint64_t base, unsigned size)
{
    size = (unsigned)IA64VMM_ALIGN_UP(size, 4096u);
    if (size > UINT64_MAX - base)
        handoff_error("IOMMU register extent overflow");
    for (unsigned j = 0; j < bi->mem_range_count; ++j) {
        const struct boot_mem_range *m = &bi->mem[j];
        if (m->size > UINT64_MAX - m->base ||
            (base < m->base + m->size && m->base < base + size && m->type != BOOT_MEM_RESERVED))
            handoff_error("IOMMU register aperture overlaps RAM");
    }
    uint8_t *p = amd64_map_mmio(base, size);
    if (!p)
        handoff_error("cannot map IOMMU register aperture");
    return p;
}
static uint8_t *map_vtd(const struct boot_info *bi, uint64_t base, unsigned size)
{
    uint8_t *p = map_registers(bi, base, size);
    uint64_t ecap = read_reg(p, 0x10, 8);
    if (ecap == UINT64_MAX)
        handoff_error("VT-d capability unreadable");
    unsigned extent = (unsigned)((ecap >> 8) & 1023) * 16 + 16;
    return extent > size ? map_registers(bi, base, extent) : p;
}
static uint8_t *map_amdvi(const struct boot_info *bi, uint64_t base, unsigned size)
{
    uint8_t *p = map_registers(bi, base, size);
    uint64_t efr = read_reg(p, 0x30, 8);
    if (efr == UINT64_MAX)
        handoff_error("AMD-Vi capability unreadable");
    return efr & (UINT64_C(1) << 9) ? map_registers(bi, base, 0x80000) : p;
}
void amd64_iommu_handoff(const struct boot_info *bi)
{
#if !defined(IA64VMM_AMD64_HOST_ONLY)
    const char *value;
    size_t size;
    protection_enabled = true;
    if (platform_cmdline_value(bi->cmdline, "iommu", &value, &size)) {
        if (size == 3 && !strncmp(value, "off", size))
            protection_enabled = false;
        else if (size != 4 || strncmp(value, "auto", size))
            hal_panic("invalid iommu setting; use iommu=auto or iommu=off");
    }
#endif
    static const struct amd64_iommu_ops ops = {read_reg, write_reg};
    static const struct amd64_iommu_dma_ops pci_ops = {.config = dma_config,
                                                       .config_write = dma_config_write};
    const char *names[] = {"DMAR", "IVRS"};
    if (amd64_acpi_table(bi, names[0]) && amd64_acpi_table(bi, names[1]))
        handoff_error("mixed VT-d/AMD-Vi remappers unsupported");
    unsigned total = 0;
    for (unsigned t = 0; t < 2; ++t) {
        const uint8_t *p = amd64_acpi_table(bi, names[t]);
        if (!p)
            continue;
        uint32_t len = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) |
                       ((uint32_t)p[7] << 24);
        struct amd64_iommu_table table;
        if (!amd64_iommu_parse(p, len, &table))
            handoff_error("malformed/unsupported IOMMU table or unit capacity exceeded");
        LOGI("IOMMU %s units=%u/%u", names[t], table.count, AMD64_IOMMU_MAX_UNITS);
        for (unsigned i = 0; i < table.count; ++i) {
            const struct amd64_iommu_unit *u = &table.units[i];
            if (u->segment)
                handoff_error("IOMMU on unsupported nonzero PCI segment");
            if (u->kind == AMD64_AMDVI) {
                /* Check/map the declared aperture before locking PCI decode. */
                map_registers(bi, u->base, u->size);
                if (!amd64_amdvi_prepare(u, &pci_ops, NULL))
                    handoff_error("AMD-Vi PCI capability/MMIO aperture could not be enabled");
            }
            uint8_t *r = u->kind == AMD64_VTD ? map_vtd(bi, u->base, u->size)
                                              : map_amdvi(bi, u->base, u->size);
            uint64_t status =
                read_reg(r, u->kind == AMD64_VTD ? 0x1c : 0x2020, u->kind == AMD64_VTD ? 4 : 8);
            uint64_t control = u->kind == AMD64_VTD ? status : read_reg(r, 0x18, 8);
            LOGI("IOMMU %s unit=%u base=%llx status=%llx control/state=%llx translation=%u",
                 names[t], i, (unsigned long long)u->base, (unsigned long long)status,
                 (unsigned long long)control,
                 (unsigned)((control >> (u->kind == AMD64_VTD ? 31 : 0)) & 1));
            if (!amd64_iommu_disable(u->kind, &ops, r))
                handoff_error(
                    "IOMMU handoff failed: unreadable/stuck command or active firmware VT-d QI/interrupt remapping");
            LOGI("IOMMU %s unit=%u translation off (identity DMA, no DMA protection)", names[t], i);
            ++total;
        }
    }
    if (!total)
        LOGI("IOMMU: no DMAR/IVRS units reported");
}

#if !defined(IA64VMM_AMD64_HOST_ONLY)
static uint8_t *dma_registers[AMD64_IOMMU_MAX_UNITS];
static struct amd64_vtd vtd_protection;
static struct amd64_amdvi amdvi_protection;
static unsigned irq_lock;
bool amd64_iommu_irq_enabled(void)
{
    return vtd_protection.irq_ready || amdvi_protection.irq_ready;
}
static bool irq_result(bool ok)
{
    bool amd = amdvi_protection.irq_ready || amdvi_protection.irq_failed;
    bool failed = amd ? amdvi_protection.irq_failed : vtd_protection.irq_failed;
    platform_unlock(&irq_lock);
    if (failed) {
        LOGE("%s interrupt remapping failed: %s", amd ? "AMD-Vi" : "VT-d",
             amd ? amdvi_protection.error : vtd_protection.error);
        hal_panic("interrupt remapping failed; routes retained");
    }
    return ok;
}
static bool irq_message(uint16_t bdf, unsigned cpu, unsigned first, unsigned count,
                        uint64_t *address, uint32_t *data)
{
    if (cpu >= amd64_cpu_count)
        return false;
    platform_lock(&irq_lock);
    if (amdvi_protection.irq_ready)
        return irq_result(amd64_amdvi_irq_msi(&amdvi_protection, 0, bdf, first, count,
                                              amd64_cpus[cpu].apic_id, address, data));
    return irq_result(amd64_vtd_irq_msi(&vtd_protection, 0, bdf, first, count,
                                        amd64_cpus[cpu].apic_id, address, data));
}
static void irq_revoke(unsigned first, unsigned count)
{
    platform_lock(&irq_lock);
    bool ok = amdvi_protection.irq_ready ? amd64_amdvi_irq_clear(&amdvi_protection, first, count)
                                         : amd64_vtd_irq_clear(&vtd_protection, first, count);
    if (!irq_result(ok))
        hal_panic("invalid interrupt revocation; vectors retained");
}
bool amd64_iommu_ioapic(unsigned id, unsigned pin, unsigned cpu, unsigned vector, bool low,
                        bool level, uint64_t *rte)
{
    if (cpu >= amd64_cpu_count)
        return false;
    platform_lock(&irq_lock);
    if (amdvi_protection.irq_ready)
        return irq_result(amd64_amdvi_irq_ioapic(&amdvi_protection, id, pin, vector,
                                                 amd64_cpus[cpu].apic_id, low, level, rte));
    return irq_result(amd64_vtd_irq_ioapic(&vtd_protection, id, pin, vector,
                                           amd64_cpus[cpu].apic_id, low, level, rte));
}
static void *dma_allocate(void *context, unsigned pages)
{
    (void)context;
    return hal_alloc_pages(pages, 0);
}
static bool dma_physical(void *context, const void *p, uint64_t *pa)
{
    (void)context;
    return amd64_memory_physical((uintptr_t)p, pa);
}
static void *dma_pointer(void *context, uint64_t pa)
{
    (void)context;
    return (void *)(uintptr_t)(amd64_hhdm + pa);
}
static void dma_flush(void *context, const void *p, size_t size)
{
    (void)context;
    uint32_t a, b, c, d;
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    unsigned line = ((b >> 8) & 255) * 8;
    if (!(d & (1u << 19)) || !line || (line & (line - 1)) || line > 4096)
        hal_panic("IOMMU publication requires CLFLUSH");
    for (uintptr_t at = (uintptr_t)p & ~(uintptr_t)(line - 1); at < (uintptr_t)p + size; at += line)
        __asm__ volatile("clflush (%0)" ::"r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}
static uint64_t dma_read(void *context, unsigned unit, unsigned off, unsigned width)
{
    (void)context;
    return read_reg(dma_registers[unit], off, width);
}
static void dma_write(void *context, unsigned unit, unsigned off, uint64_t value, unsigned width)
{
    (void)context;
    write_reg(dma_registers[unit], off, value, width);
}
void amd64_iommu_protect(const struct boot_info *bi)
{
    if (!protection_enabled) {
        LOGW(
            "IOMMU disabled by configuration: identity DMA without memory protection or interrupt remapping");
        return;
    }
    const uint8_t *table = amd64_acpi_table(bi, "DMAR");
    bool amd = false;
    if (!table) {
        table = amd64_acpi_table(bi, "IVRS");
        amd = true;
    }
    if (!table) {
        LOGW(
            "IOMMU: no remapping units; identity DMA without memory protection or interrupt remapping");
        return;
    }
    uint32_t length = (uint32_t)table[4] | ((uint32_t)table[5] << 8) | ((uint32_t)table[6] << 16) |
                      ((uint32_t)table[7] << 24);
    struct amd64_iommu_table locations;
    if (!amd64_iommu_parse(table, length, &locations))
        hal_panic("invalid IOMMU table before protection");
    if (!locations.count) {
        LOGW(
            "IOMMU: no remapping units; identity DMA without memory protection or interrupt remapping");
        return;
    }
    for (unsigned i = 0; i < locations.count; ++i)
        dma_registers[i] = amd ? map_amdvi(bi, locations.units[i].base, locations.units[i].size)
                               : map_vtd(bi, locations.units[i].base, locations.units[i].size);
    static const struct amd64_iommu_dma_ops ops = {
        dma_allocate, dma_physical, dma_pointer, dma_flush,
        dma_config,   dma_read,     dma_write,   dma_config_write,
    };
    const struct pci_device *devices[PCI_MAX_DEVICES];
    unsigned count = platform_pci_count();
    if (count > PCI_MAX_DEVICES)
        hal_panic("IOMMU PCI requester capacity exceeded");
    for (unsigned i = 0; i < count; ++i)
        devices[i] = platform_pci_device(i);
    const char *name = amd ? "AMD-Vi" : "VT-d";
    bool built =
        amd ? amd64_amdvi_build(table, length, bi, devices, count, &ops, NULL, &amdvi_protection)
            : amd64_vtd_build(table, length, bi, devices, count, &ops, NULL, &vtd_protection);
    bool enabled =
        built && (amd ? amd64_amdvi_enable(&amdvi_protection) : amd64_vtd_enable(&vtd_protection));
    if (!enabled) {
        char reason[224];
        vmm_snprintf(reason, sizeof(reason),
                     "%s DMA protection refused: %s; set iommu=off in Configure VMM Options", name,
                     amd ? amdvi_protection.error : vtd_protection.error);
        hal_panic(reason);
    }
    for (unsigned i = 0; i < locations.count; ++i) {
        if (amd) {
            const struct amd64_amdvi_unit *u = &amdvi_protection.units[i];
            LOGI(
                "AMD-Vi unit=%u base=%llx table=%llx levels=%u width=%u event=%llx translation enabled",
                i, (unsigned long long)u->base, (unsigned long long)u->table, u->levels, u->width,
                (unsigned long long)u->event);
        } else {
            const struct amd64_vtd_unit *u = &vtd_protection.units[i];
            LOGI(
                "VT-d unit=%u base=%llx root=%llx levels=%u width=%u coherent=%u translation enabled",
                i, (unsigned long long)u->base, (unsigned long long)u->root, u->levels, u->width,
                (unsigned)(u->ecap & 1));
        }
    }
    LOGI(
        "%s: guest DMA confined to guest RAM/firmware buffers; trusted VMM media has a separate private domain",
        name);
    if (amd) {
        bool supported = true;
        bool extended = amd64_apic_extended();
        for (unsigned i = 0; i < amdvi_protection.count; ++i)
            supported &= !extended || (amdvi_protection.units[i].efr & 0x84) == 0x84;
        if (supported) {
            if (!amd64_amdvi_irq_init(&amdvi_protection, extended)) {
                LOGE("AMD-Vi interrupt setup refused: %s", amdvi_protection.error);
                hal_panic(
                    "AMD-Vi interrupt remapping failed; set IOMMU DMA protection to Disabled (iommu=off)");
            }
            static const struct platform_host_irq_ops irq_ops = {
                .message = irq_message,
                .revoke = irq_revoke,
                .retire = amd64_irq_retire,
                .remapped = true,
            };
            platform_irq_set_host(&irq_ops);
            LOGI("AMD-Vi: source-validated interrupt remapping enabled");
        }
    } else {
        bool extended = amd64_apic_extended();
        bool supported = (table[37] & 1) && !(extended && (table[37] & 2));
        for (unsigned i = 0; i < vtd_protection.count; ++i) {
            uint64_t ecap = vtd_protection.units[i].ecap;
            supported &= (ecap & 10) == 10 && (!extended || (ecap & 16));
        }
        if (supported) {
            if (!amd64_vtd_irq_init(&vtd_protection, table, length, extended)) {
                LOGE("VT-d interrupt setup refused: %s", vtd_protection.error);
                hal_panic(
                    "VT-d interrupt remapping failed; set IOMMU DMA protection to Disabled (iommu=off)");
            }
            static const struct platform_host_irq_ops irq_ops = {
                .message = irq_message,
                .revoke = irq_revoke,
                .retire = amd64_irq_retire,
                .remapped = true,
            };
            platform_irq_set_host(&irq_ops);
            LOGI("VT-d: source-validated interrupt remapping enabled");
        }
    }
    if (!amd64_iommu_irq_enabled())
        LOGW("%s: direct device interrupts are not isolated", name);
    LOGW("%s: pre-activation DMA and root-complex peer routing are not isolated", name);
}
#else
void amd64_iommu_protect(const struct boot_info *bi)
{
    (void)bi;
}
#endif
