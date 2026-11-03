/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Amd64/Interrupt/Amd64IoApic.cpp
 */
/* IOAPIC routing from the copied host ACPI tables. */
#include "host/amd64/amd64.h"
#include "host/amd64/smp.h"
#include "host/amd64/iommu.h"
#include "ia64vmm/interrupts.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "platform/acpi-irq.h"
#if !defined(IA64VMM_AMD64_HOST_ONLY)
struct ioapic {
    volatile uint32_t *mmio;
    unsigned id, version, base, count, lock;
};
static struct ioapic apics[16];
static unsigned napics;
static struct acpi_irq *routing;
static const uint8_t *host_madt;
static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint64_t lock(struct ioapic *a)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags)::"memory");
    while (__atomic_exchange_n(&a->lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return flags;
}
static void unlock(struct ioapic *a, uint64_t flags)
{
    __atomic_store_n(&a->lock, 0, __ATOMIC_RELEASE);
    if (flags & 512)
        __asm__ volatile("sti" ::: "memory");
}
static uint32_t read(struct ioapic *a, unsigned reg)
{
    a->mmio[0] = reg;
    return a->mmio[4];
}
static void write(struct ioapic *a, unsigned reg, uint32_t data)
{
    a->mmio[0] = reg;
    a->mmio[4] = data;
    (void)a->mmio[4];
}
static struct ioapic *controller(unsigned gsi)
{
    for (unsigned i = 0; i < napics; ++i)
        if (gsi >= apics[i].base && gsi - apics[i].base < apics[i].count)
            return &apics[i];
    return NULL;
}
static bool resolve(void *context, uint16_t bdf, unsigned pin, uint32_t *gsi, bool *low,
                    bool *level)
{
    (void)context;
    return routing && acpi_irq_resolve(routing, bdf, pin, gsi, low, level);
}
static bool route_pin(struct ioapic *a, unsigned pin, unsigned cpu, unsigned vector, bool low,
                      bool level)
{
    if (!a || cpu >= amd64_cpu_count || vector < VMM_IRQ_FIRST_VECTOR ||
        vector > VMM_IRQ_LAST_VECTOR)
        return false;
    unsigned reg = 0x10 + 2 * pin;
    uint64_t flags = lock(a);
    uint32_t old = read(a, reg) | (1u << 16);
    write(a, reg, old);
    if ((old & ((1u << 15) | (1u << 14))) == ((1u << 15) | (1u << 14))) {
        if (a->version >= 0x20)
            a->mmio[0x40 / 4] = old & 255;
        else {
            write(a, reg, old & ~(1u << 15));
            write(a, reg, old);
        }
        if (read(a, reg) & (1u << 14)) {
            unlock(a, flags);
            return false;
        }
    }
    unlock(a, flags);
    uint64_t rte = (1u << 16);
    if (amd64_iommu_irq_enabled()) {
        if (!amd64_iommu_ioapic(a->id, pin, cpu, vector, low, level, &rte))
            return false;
    } else {
        if (amd64_cpus[cpu].apic_id >= 255)
            return false;
        rte |= ((uint64_t)amd64_cpus[cpu].apic_id << 56) | vector | (low ? 1u << 13 : 0) |
               (level ? 1u << 15 : 0);
    }
    flags = lock(a);
    write(a, reg + 1, rte >> 32);
    write(a, reg, (uint32_t)rte);
    bool ready = !(level && (read(a, reg) & (1u << 14)));
    unlock(a, flags);
    return ready;
}
static bool route(void *context, uint32_t gsi, unsigned cpu, unsigned vector, bool low)
{
    (void)context;
    struct ioapic *a = controller(gsi);
    return a && route_pin(a, gsi - a->base, cpu, vector, low, true);
}
/* ISA IRQs keep their bus default (edge, active high) unless the host
 * MADT overrides them; a level override belongs to an INTx/SCI-style source
 * that this edge path cannot acknowledge, so it is refused. */
static bool route_isa(void *context, unsigned irq, unsigned cpu, unsigned vector, uint32_t *gsi)
{
    (void)context;
    bool low, level;
    uint32_t line;
    if (!acpi_isa_irq(host_madt, irq, &line, &low, &level) || level)
        return false;
    struct ioapic *a = controller(line);
    if (!a || !route_pin(a, line - a->base, cpu, vector, low, false))
        return false;
    *gsi = line;
    return true;
}
static void mask(void *context, uint32_t gsi, bool masked)
{
    (void)context;
    struct ioapic *a = controller(gsi);
    if (!a)
        return;
    uint64_t flags = lock(a);
    unsigned reg = 0x10 + 2 * (gsi - a->base);
    uint32_t value = read(a, reg);
    write(a, reg, masked ? value | (1u << 16) : value & ~(1u << 16));
    unlock(a, flags);
}
static void unmask_if_idle(void *context, uint32_t gsi, const unsigned *waiting)
{
    (void)context;
    struct ioapic *a = controller(gsi);
    if (!a)
        return;
    uint64_t flags = lock(a);
    unsigned reg = 0x10 + 2 * (gsi - a->base);
    uint32_t value = read(a, reg);
    /* All assigned INTx/ISA lines target vCPU 0's host CPU. Guest
     * RTE/config/EOI work is deferred to that executor; setup/teardown dispatch
     * there too. Test after CLI on this same destination so an arrival
     * cannot publish waiting between the test and the MMIO write. */
    write(a, reg,
          __atomic_load_n(waiting, __ATOMIC_ACQUIRE) ? value | (1u << 16) : value & ~(1u << 16));
    unlock(a, flags);
}
void amd64_intx_init(const struct boot_info *bi, const struct pci_host_ops *host, void *context)
{
    platform_irq_measure_cmdline(bi->cmdline);
    static const struct platform_intx_ops ops = {resolve, route, mask, unmask_if_idle, route_isa};
    const uint8_t *madt = amd64_acpi_table(bi, "APIC"), *dsdt = amd64_acpi_dsdt(bi);
    if (!madt || !dsdt || le(madt + 4, 4) < 44)
        return;
    unsigned length = le(madt + 4, 4);
    for (unsigned at = 44; at + 2 <= length;) {
        const uint8_t *p = madt + at;
        unsigned n = p[1];
        if (n < 2 || n > length - at)
            return;
        if (p[0] == 1 && n >= 12) {
            uint64_t pa = le(p + 4, 4);
            unsigned base = le(p + 8, 4);
            if (napics == 16 || !pa || (pa & 4095))
                return;
            struct ioapic *a = &apics[napics];
            a->mmio = (volatile uint32_t *)amd64_map_mmio(pa, 4096);
            if (!a->mmio)
                return;
            unsigned version = read(a, 1), count = ((version >> 16) & 255) + 1;
            if (version == UINT32_MAX || (version & 255) < 0x11 || count > 120 ||
                base > UINT32_MAX - count)
                return;
            for (unsigned i = 0; i < napics; ++i)
                if (p[2] == apics[i].id ||
                    (base < apics[i].base + apics[i].count && apics[i].base < base + count))
                    return;
            a->id = p[2];
            a->version = version & 255;
            a->base = base;
            a->count = count;
            ++napics;
            LOGI("host IOAPIC base=%llx GSIs=%u..%u", (unsigned long long)pa, base,
                 base + count - 1);
        }
        at += n;
    }
    if (!napics)
        return;
    /* ISA lines need only the MADT; PCI INTx additionally needs host AML,
     * and resolve() reports every PCI pin unresolved while routing is NULL. */
    host_madt = madt;
    platform_intx_set_host(&ops, NULL);
    routing = hal_alloc_pages((sizeof(*routing) + 4095) / 4096, HAL_PAGE_ZERO);
    if (!routing)
        return;
    aml_init(&routing->aml);
    bool loaded = aml_load(&routing->aml, dsdt, le(dsdt + 4, 4));
    for (unsigned i = 0; loaded && i < 256; ++i) {
        const uint8_t *ssdt = amd64_acpi_table_index(bi, "SSDT", i);
        if (!ssdt)
            break;
        loaded = aml_load(&routing->aml, ssdt, le(ssdt + 4, 4));
    }
    if (!loaded || !acpi_irq_prepare(routing, host, context)) {
        LOGW("host AML routing subset could not evaluate host tables");
        hal_free_pages(routing, (sizeof(*routing) + 4095) / 4096);
        routing = NULL;
        return;
    }
}
#endif
