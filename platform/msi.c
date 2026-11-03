/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Kernel/Device/PciMessageSignaled.cpp,
 * Source/Platform/Amd64/Interrupt/Amd64DeviceVectors.cpp
 */
/* MSI enable/program/enable order and host-message encoding. MSI-X mediation
 * extends the same ownership rule (PCI 3.0 §6.8). */
#include "ia64vmm/interrupts.h"
#include "platform/platform.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define MSIX_MAX_ENTRIES 2048u
#define IRQ_ACTIVE (1u << 31)
#define IRQ_MASKED (1u << 30)
#define IRQ_MSIX (1u << 29)
struct msi_entry {
    uint64_t address;
    uint32_t data, control;
    unsigned host_vector;
};
struct pci_irq_state {
    unsigned lock;
    struct pci_device *device;
    struct pci_irq_state *next;
    const struct pci_host_ops *host;
    void *context;
    unsigned entries, table_bar, pba_bar, msi_vectors;
    uint64_t table, pba, table_end, pba_end;
    bool logged, msix_valid;
    struct msi_entry msi[32];
    struct msi_entry *msix;
    uint64_t pending[32];
    uint32_t msi_pending;
    struct irq_counters counters;
};
struct vector_binding {
    struct pci_irq_state *state;
    unsigned route, msix_index;
    struct irq_counters counters;
};
static struct vector_binding vectors[256];
static unsigned irq_readers;
static struct pci_irq_state *active_states;
extern void code_epoch_bump(enum code_device_class) __attribute__((weak));

static uint32_t cfg(struct pci_device *d, unsigned off, unsigned n)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < n; ++i)
        value |= (uint32_t)d->irq_config[off + i] << (8 * i);
    return value;
}
static uint64_t mmio_read(uint8_t *p, unsigned size)
{
    __asm__ volatile("mfence" ::: "memory");
    uint64_t value;
    switch (size) {
    case 1:
        value = *(volatile uint8_t *)p;
        break;
    case 2:
        value = *(volatile uint16_t *)p;
        break;
    case 4:
        value = *(volatile uint32_t *)p;
        break;
    default:
        value = *(volatile uint64_t *)p;
        break;
    }
    __asm__ volatile("mfence" ::: "memory");
    return value;
}
static void mmio_write(uint8_t *p, uint64_t value, unsigned size)
{
    __asm__ volatile("mfence" ::: "memory");
    switch (size) {
    case 1:
        *(volatile uint8_t *)p = value;
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
    __asm__ volatile("mfence" ::: "memory");
}
bool platform_msi_decode(uint64_t address, uint32_t data, struct ia64_irq_message *m)
{
    if ((address & ~UINT64_C(0xffff0)) != UINT64_C(0xfee00000) || (data & ~0xc7ffu))
        return false;
    int cpu = platform_irq_target((address >> 12) & 255, (address >> 4) & 255);
    unsigned mode = (data >> 8) & 7, vector = data & 255;
    if (mode == IA64_IRQ_NMI)
        vector = 2;
    else if (mode == IA64_IRQ_EXTINT)
        vector = 0;
    else if (mode != IA64_IRQ_INT)
        return false;
    /* Management vectors have no guest device route. Numeric guest IDs
     * never turn into host APIC destinations without the placement map. */
    if (cpu < 0 || (vector != 0 && vector != 2 && (vector < 16 || vector > 255)) ||
        vector == 0xe0 || vector == 0xe1 || vector == 0xfe || vector == 0xff)
        return false;
    *m = (struct ia64_irq_message){cpu, vector, mode, !!(data & (1u << 15))};
    return true;
}
static void reject(struct pci_irq_state *s, struct msi_entry *e)
{
    __atomic_fetch_add(&s->counters.rejected, 1, __ATOMIC_RELAXED);
    if (e->host_vector)
        __atomic_fetch_add(&vectors[e->host_vector].counters.rejected, 1, __ATOMIC_RELAXED);
    LOGW("PCI %04x rejected IRQ address=%llx data=%x (target/vector/backend)", s->device->bdf,
         (unsigned long long)e->address, e->data);
}
static unsigned reserve(struct pci_irq_state *s, unsigned n)
{
    unsigned first = platform_irq_vector_reserve(s, n);
    if (first) {
        for (unsigned i = 0; i < n; ++i) {
            vectors[first + i].msix_index = UINT32_MAX;
            __atomic_store_n(&vectors[first + i].state, s, __ATOMIC_RELEASE);
        }
        return first;
    }
    LOGW("PCI %04x host IRQ vector pool exhausted", s->device->bdf);
    return 0;
}
static bool message(struct pci_irq_state *s, struct msi_entry *e, unsigned count, uint64_t *address,
                    uint32_t *data)
{
    struct ia64_irq_message m;
    if (!platform_msi_decode(e->address, e->data, &m)) {
        reject(s, e);
        return false;
    }
    unsigned cpu = platform_irq_host_cpu(m.cpu);
    if (cpu != UINT32_MAX &&
        platform_irq_host_message(s->device->bdf, cpu, e->host_vector, count, address, data))
        return true;
    platform_irq_host_revoke(e->host_vector, count);
    reject(s, e);
    return false;
}
static bool publish(struct pci_irq_state *s, struct msi_entry *e)
{
    struct ia64_irq_message m;
    if (!platform_msi_decode(e->address, e->data, &m)) {
        reject(s, e);
        return false;
    }
    struct vector_binding *b = &vectors[e->host_vector];
    unsigned route = IRQ_ACTIVE | (m.cpu << 16) | (m.mode << 8) | m.vector;
    if (e->control & 1)
        route |= IRQ_MASKED;
    if ((uintptr_t)e >= (uintptr_t)s->msix && s->msix &&
        (uintptr_t)e < (uintptr_t)(s->msix + s->entries)) {
        route |= IRQ_MSIX;
        if (!(cfg(s->device, s->device->msix_offset + 2, 2) & 0x8000) ||
            (cfg(s->device, s->device->msix_offset + 2, 2) & 0x4000))
            route |= IRQ_MASKED;
    }
    __atomic_store_n(&b->route, route, __ATOMIC_RELEASE);
    if (!s->logged && !platform_irq_remapped()) {
        LOGW("PCI %04x direct host MSI message, no interrupt remapping", s->device->bdf);
        s->logged = true;
    }
    return true;
}
static void disable(struct msi_entry *e)
{
    if (e->host_vector) {
        platform_irq_host_revoke(e->host_vector, 1);
        platform_irq_host_retire(e->host_vector, 1);
        __atomic_store_n(&vectors[e->host_vector].route, 0, __ATOMIC_RELEASE);
    }
}
static void disable_msi(struct pci_irq_state *s)
{
    if (!s->msi_vectors)
        return;
    platform_irq_host_revoke(s->msi[0].host_vector, s->msi_vectors);
    platform_irq_host_retire(s->msi[0].host_vector, s->msi_vectors);
    for (unsigned i = 0; i < s->msi_vectors; ++i)
        __atomic_store_n(&vectors[s->msi[i].host_vector].route, 0, __ATOMIC_RELEASE);
}
bool platform_msi_prepare(struct pci_device *d, const struct pci_host_ops *host, void *context)
{
    if (d->interrupts || (!d->msi_offset && !d->msix_offset))
        return true;
    struct pci_irq_state *s = hal_alloc_pages((sizeof(*s) + 4095) / 4096, HAL_PAGE_ZERO);
    if (!s)
        return false;
    s->next = active_states;
    active_states = s;
    d->interrupts = s;
    s->device = d;
    s->host = host;
    s->context = context;
    if (!d->msix_offset)
        return true;
    unsigned p = d->msix_offset;
    s->entries = (cfg(d, p + 2, 2) & 0x7ff) + 1;
    uint32_t table = cfg(d, p + 4, 4), pba = cfg(d, p + 8, 4);
    s->table_bar = table & 7;
    s->pba_bar = pba & 7;
    s->table = table & ~7u;
    s->pba = pba & ~7u;
    uint64_t table_bytes = (uint64_t)s->entries * 16;
    uint64_t pba_bytes = ((s->entries + 63) / 64) * 8;
    if (s->table_bar >= 6 || s->pba_bar >= 6 || d->bars[s->table_bar].io ||
        d->bars[s->pba_bar].io || s->table > d->bars[s->table_bar].size ||
        table_bytes > d->bars[s->table_bar].size - s->table || s->pba > d->bars[s->pba_bar].size ||
        pba_bytes > d->bars[s->pba_bar].size - s->pba ||
        (s->table_bar == s->pba_bar && s->table < s->pba + pba_bytes &&
         s->pba < s->table + table_bytes)) {
        LOGE("PCI %04x malformed MSI-X table/PBA BAR descriptors", d->bdf);
        return true;
    }
    s->table_end = s->table + table_bytes;
    s->pba_end = s->pba + pba_bytes;
    s->msix = hal_alloc_pages((s->entries * sizeof(*s->msix) + 4095) / 4096, HAL_PAGE_ZERO);
    if (!s->msix)
        return false;
    for (unsigned i = 0; i < s->entries; ++i)
        s->msix[i].control = 1;
    s->msix_valid = true;
    return true;
}
static void inject_pending(struct pci_irq_state *s, struct msi_entry *e)
{
    unsigned route = __atomic_load_n(&vectors[e->host_vector].route, __ATOMIC_ACQUIRE);
    if ((route & (IRQ_ACTIVE | IRQ_MASKED)) != IRQ_ACTIVE)
        return;
    if (platform_irq_inject((route >> 16) & 255, route & 255, (route >> 8) & 7)) {
        __atomic_fetch_add(&s->counters.injected, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&vectors[e->host_vector].counters.injected, 1, __ATOMIC_RELAXED);
    }
}
static void update_msix(struct pci_irq_state *s, unsigned i)
{
    struct pci_device *d = s->device;
    struct msi_entry *e = &s->msix[i];
    uint8_t *table = d->bars[s->table_bar].hva;
    if (!table)
        return;
    table += s->table + i * 16;
    /* Mask, program complete host tuple, publish route, then unmask. */
    mmio_write(table + 12, 1, 4);
    disable(e);
    if (!e->host_vector && (cfg(d, d->msix_offset + 2, 2) & 0x8000) && !(e->control & 1)) {
        e->host_vector = reserve(s, 1);
        if (e->host_vector)
            vectors[e->host_vector].msix_index = i;
    }
    uint64_t address;
    uint32_t data;
    if (!e->host_vector || !message(s, e, 1, &address, &data) || !publish(s, e))
        return;
    mmio_write(table, (uint32_t)address, 4);
    mmio_write(table + 4, address >> 32, 4);
    mmio_write(table + 8, data, 4);
    mmio_write(table + 12, e->control & 1, 4);
    if (!(e->control & 1) && !(cfg(d, d->msix_offset + 2, 2) & 0x4000) &&
        (cfg(d, d->msix_offset + 2, 2) & 0x8000) &&
        (__atomic_load_n(&s->pending[i / 64], __ATOMIC_ACQUIRE) & (UINT64_C(1) << (i % 64)))) {
        __atomic_fetch_and(&s->pending[i / 64], ~(UINT64_C(1) << (i % 64)), __ATOMIC_ACQ_REL);
        inject_pending(s, e);
    }
}
static void update_msi(struct pci_irq_state *s)
{
    struct pci_device *d = s->device;
    unsigned p = d->msi_offset;
    if (!p)
        return;
    uint16_t control = cfg(d, p + 2, 2);
    bool msix_enabled = d->msix_offset && (cfg(d, d->msix_offset + 2, 2) & 0x8000);
    /* NVMe 1.4a §7.5.2 requires MME=0 as well as MSI disabled in MSI-X
     * mode. Preserve the guest tuple for switching back to MSI. */
    s->host->write(s->context, d->bdf, p + 2, control & ~(msix_enabled ? 0x71u : 1u), 2);
    disable_msi(s);
    if (!(control & 1) || msix_enabled)
        return;
    unsigned mme = (control >> 4) & 7, mmc = (control >> 1) & 7;
    if (mme > mmc || mme > 5) {
        reject(s, &s->msi[0]);
        return;
    }
    unsigned n = 1u << mme, data_off = p + ((control & 0x80) ? 12 : 8);
    uint64_t address = cfg(d, p + 4, 4);
    if (control & 0x80)
        address |= (uint64_t)cfg(d, p + 8, 4) << 32;
    uint32_t data = cfg(d, data_off, 2);
    if (data & (n - 1)) {
        reject(s, &s->msi[0]);
        return;
    }
    for (unsigned i = 0; i < n; ++i) {
        struct ia64_irq_message m;
        if (!platform_msi_decode(address, data + i, &m)) {
            reject(s, &s->msi[0]);
            return;
        }
    }
    if (s->msi_vectors < n) {
        unsigned first = reserve(s, n);
        if (!first)
            return;
        for (unsigned i = 0; i < n; ++i) {
            s->msi[i].host_vector = first + i;
            vectors[first + i].msix_index = i;
        }
        s->msi_vectors = n;
    }
    uint64_t host_address = 0;
    uint32_t host_data = 0;
    for (unsigned i = 0; i < n; ++i) {
        struct msi_entry *e = &s->msi[i];
        e->address = address;
        e->data = data + i;
        e->control = control & 0x100 ? (cfg(d, data_off + 4, 4) >> i) & 1 : 0;
    }
    if (!message(s, &s->msi[0], n, &host_address, &host_data))
        return;
    for (unsigned i = 0; i < n; ++i)
        if (!publish(s, &s->msi[i])) {
            disable_msi(s);
            return;
        }
    s->host->write(s->context, d->bdf, p + 4, host_address, 4);
    if (control & 0x80)
        s->host->write(s->context, d->bdf, p + 8, host_address >> 32, 4);
    s->host->write(s->context, d->bdf, data_off, host_data, 2);
    if (control & 0x100)
        s->host->write(s->context, d->bdf, data_off + 4, cfg(d, data_off + 4, 4), 4);
    s->host->write(s->context, d->bdf, p + 2, control, 2);
    for (unsigned i = 0; i < n; ++i)
        if (!(s->msi[i].control & 1) &&
            (__atomic_fetch_and(&s->msi_pending, ~(1u << i), __ATOMIC_ACQ_REL) & (1u << i)))
            inject_pending(s, &s->msi[i]);
}
static void platform_msi_config_changed_unlocked(struct pci_device *d)
{
    struct pci_irq_state *s = d->interrupts;
    if (!s)
        return;
    bool msix_enabled = d->msix_offset && (cfg(d, d->msix_offset + 2, 2) & 0x8000);
    /* Disable MSI before enabling MSI-X, and MSI-X before enabling MSI. */
    if (msix_enabled)
        update_msi(s);
    if (d->msix_offset && !s->msix_valid) {
        s->host->write(s->context, d->bdf, d->msix_offset + 2,
                       (cfg(d, d->msix_offset + 2, 2) & ~0x8000u) | 0x4000u, 2);
    }
    if (d->msix_offset && s->msix_valid) {
        unsigned p = d->msix_offset;
        uint16_t control = cfg(d, p + 2, 2);
        s->host->write(s->context, d->bdf, p + 2, control | 0x4000u, 2);
        for (unsigned i = 0; i < s->entries; ++i)
            update_msix(s, i);
        s->host->write(s->context, d->bdf, p + 2, control, 2);
    }
    if (!msix_enabled)
        update_msi(s);
    platform_intx_config_changed(d);
}
void platform_msi_config_changed(struct pci_device *d)
{
    struct pci_irq_state *s = d->interrupts;
    if (!s)
        return;
    platform_lock(&s->lock);
    platform_msi_config_changed_unlocked(d);
    platform_unlock(&s->lock);
}
static void platform_msi_sync_pending_unlocked(struct pci_device *d)
{
    struct pci_irq_state *s = d->interrupts;
    unsigned p = d->msi_offset;
    if (!s || !p)
        return;
    unsigned control = cfg(d, p + 2, 2);
    if (!(control & 0x100))
        return;
    unsigned off = p + ((control & 0x80) ? 12 : 8) + 8;
    uint32_t pending = s->host->read(s->context, d->bdf, off, 4) |
                       __atomic_load_n(&s->msi_pending, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < 4; ++i)
        d->irq_config[off + i] = pending >> (8 * i);
}
void platform_msi_sync_pending(struct pci_device *d)
{
    struct pci_irq_state *s = d->interrupts;
    if (!s)
        return;
    platform_lock(&s->lock);
    platform_msi_sync_pending_unlocked(d);
    platform_unlock(&s->lock);
}
static uint64_t table_read(struct msi_entry *e, unsigned off, unsigned size)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) {
        unsigned byte = off + i;
        unsigned data = byte < 8    ? (e->address >> (byte * 8)) & 255
                        : byte < 12 ? (e->data >> ((byte - 8) * 8)) & 255
                                    : (e->control >> ((byte - 12) * 8)) & 255;
        value |= (uint64_t)data << (i * 8);
    }
    return value;
}
static uint64_t bar_read(struct pci_device *d, struct pci_bar *b, struct vcpu *v, uint64_t off,
                         unsigned size)
{
    (void)v;
    struct pci_irq_state *s = d->interrupts;
    uint64_t at = off; /* Mediated regions use BAR-base-relative offsets below. */
    if (b == &d->bars[s->table_bar] && at >= s->table && at + size <= s->table_end) {
        unsigned i = (at - s->table) / 16, byte = (at - s->table) & 15;
        if (byte + size <= 16)
            return table_read(&s->msix[i], byte, size);
    }
    if (b == &d->bars[s->pba_bar] && at >= s->pba && at + size <= s->pba_end) {
        uint64_t value = mmio_read(b->hva + at, size);
        for (unsigned i = 0; i < size; ++i) {
            unsigned byte = at - s->pba + i;
            value |=
                ((__atomic_load_n(&s->pending[byte / 8], __ATOMIC_ACQUIRE) >> ((byte & 7) * 8)) &
                 255)
                << (i * 8);
        }
        return value;
    }
    if (b == &d->bars[s->table_bar] && at < s->table_end && at + size > s->table)
        return 0;
    if (b == &d->bars[s->pba_bar] && at < s->pba_end && at + size > s->pba)
        return 0;
    return mmio_read(b->hva + at, size);
}
static void bar_write(struct pci_device *d, struct pci_bar *b, struct vcpu *v, uint64_t off,
                      uint64_t value, unsigned size)
{
    (void)v;
    struct pci_irq_state *s = d->interrupts;
    uint64_t at = off;
    if (b == &d->bars[s->table_bar] && at >= s->table && at + size <= s->table_end) {
        unsigned index = (at - s->table) / 16, byte = (at - s->table) & 15;
        if (byte + size > 16)
            return;
        struct msi_entry *e = &s->msix[index];
        for (unsigned i = 0; i < size; ++i) {
            unsigned n = byte + i;
            uint64_t data = (value >> (i * 8)) & 255;
            if (n < 8)
                e->address = (e->address & ~(UINT64_C(255) << (n * 8))) | (data << (n * 8));
            else if (n < 12) {
                unsigned shift = (n - 8) * 8;
                e->data = (e->data & ~(255u << shift)) | (data << shift);
            } else if (n == 12)
                e->control = data & 1;
        }
        update_msix(s, index);
        return;
    }
    if (b == &d->bars[s->table_bar] && at < s->table_end && at + size > s->table)
        return;
    if (b == &d->bars[s->pba_bar] && at < s->pba_end && at + size > s->pba)
        return;
    mmio_write(b->hva + at, value, size);
}
/* gm_mmio_ops receive region-relative offsets. A small page context restores
 * the BAR offset without widening or splitting co-located register accesses. */
struct bar_page {
    struct pci_device *device;
    struct pci_bar *bar;
    uint64_t offset;
};
static struct bar_page pages[PCI_MAX_DEVICES * 12];
static unsigned page_count;
static uint64_t page_read(void *opaque, struct vcpu *v, uint64_t off, unsigned n)
{
    struct bar_page *p = opaque;
    platform_pci_lock();
    platform_lock(&p->device->interrupts->lock);
    uint64_t result = bar_read(p->device, p->bar, v, p->offset + off, n);
    platform_unlock(&p->device->interrupts->lock);
    platform_pci_unlock();
    return result;
}
static void page_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned n)
{
    struct bar_page *p = opaque;
    platform_pci_lock();
    platform_lock(&p->device->interrupts->lock);
    bar_write(p->device, p->bar, v, p->offset + off, value, n);
    platform_unlock(&p->device->interrupts->lock);
    platform_pci_unlock();
}
static const struct gm_mmio_ops page_ops = {page_read, page_write};
const struct gm_mmio_ops *platform_msi_bar_region(struct pci_device *d, struct pci_bar *b,
                                                  uint64_t at, uint64_t *end, void **opaque)
{
    struct pci_irq_state *s = d->interrupts;
    if (!s || !s->msix_valid)
        return NULL;
    uint64_t ranges[4] = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    if (b == &d->bars[s->table_bar]) {
        ranges[0] = s->table & ~UINT64_C(4095);
        ranges[1] = IA64VMM_ALIGN_UP(s->table_end, 4096);
    }
    if (b == &d->bars[s->pba_bar]) {
        ranges[2] = s->pba & ~UINT64_C(4095);
        ranges[3] = IA64VMM_ALIGN_UP(s->pba_end, 4096);
    }
    uint64_t offset = at - b->base;
    bool mediated = false;
    for (unsigned i = 0; i < 4; i += 2) {
        if (offset >= ranges[i] && offset < ranges[i + 1])
            mediated = true;
        if (offset < ranges[i] && ranges[i] != UINT64_MAX && b->base + ranges[i] < *end)
            *end = b->base + ranges[i];
    }
    if (!mediated)
        return NULL;
    uint64_t next = IA64VMM_ALIGN_DOWN(at, 4096) + 4096;
    if (next < *end)
        *end = next;
    if (page_count == IA64VMM_ARRAY_SIZE(pages)) {
        *end = at;
        return NULL;
    }
    struct bar_page *p = &pages[page_count++];
    p->device = d;
    p->bar = b;
    p->offset = offset;
    *opaque = p;
    return &page_ops;
}
bool platform_msi_interrupt(unsigned vector)
{
    if (vector < VMM_IRQ_FIRST_VECTOR || vector > VMM_IRQ_LAST_VECTOR)
        return false;
    __atomic_fetch_add(&irq_readers, 1, __ATOMIC_SEQ_CST);
    struct vector_binding *b = &vectors[vector];
    struct pci_irq_state *s = __atomic_load_n(&b->state, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&b->counters.host, 1, __ATOMIC_RELAXED);
    if (!s) {
        __atomic_fetch_add(&b->counters.spurious, 1, __ATOMIC_RELAXED);
        goto done;
    }
    __atomic_fetch_add(&s->counters.host, 1, __ATOMIC_RELAXED);
    /* Completion ordering: epoch first, then release-publish the guest wake. */
    if (code_epoch_bump && s->device->storage)
        code_epoch_bump(CODE_DEVICE_STORAGE);
    unsigned route = __atomic_load_n(&b->route, __ATOMIC_ACQUIRE);
    if (!(route & IRQ_ACTIVE)) {
        __atomic_fetch_add(&s->counters.spurious, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&b->counters.spurious, 1, __ATOMIC_RELAXED);
        goto done;
    }
    if (route & IRQ_MASKED) {
        if (route & IRQ_MSIX) {
            unsigned i = b->msix_index;
            __atomic_fetch_or(&s->pending[i / 64], UINT64_C(1) << (i % 64), __ATOMIC_RELEASE);
        } else
            __atomic_fetch_or(&s->msi_pending, 1u << b->msix_index, __ATOMIC_RELEASE);
        goto done;
    }
    if (platform_irq_inject((route >> 16) & 255, route & 255, (route >> 8) & 7)) {
        __atomic_fetch_add(&s->counters.injected, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&b->counters.injected, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&s->counters.rejected, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&b->counters.rejected, 1, __ATOMIC_RELAXED);
    }
done:
    __atomic_fetch_sub(&irq_readers, 1, __ATOMIC_SEQ_CST);
    return true;
}
void platform_msi_retarget(unsigned index)
{
    /* Placement changes serialize with device setup/teardown and stop the
     * affected executor. Reprogram only devices with a route to that CPU. */
    for (struct pci_irq_state *s = active_states; s; s = s->next) {
        bool affected = false;
        for (unsigned v = VMM_IRQ_FIRST_VECTOR; v <= VMM_IRQ_LAST_VECTOR; ++v) {
            unsigned route = __atomic_load_n(&vectors[v].route, __ATOMIC_ACQUIRE);
            if (__atomic_load_n(&vectors[v].state, __ATOMIC_ACQUIRE) == s && (route & IRQ_ACTIVE) &&
                ((route >> 16) & 255) == index) {
                affected = true;
                break;
            }
        }
        if (affected) {
            platform_pci_lock();
            platform_msi_config_changed(s->device);
            platform_pci_unlock();
        }
    }
}
void platform_msi_cleanup(struct pci_device *d)
{
    struct pci_irq_state *s = d->interrupts;
    if (!s)
        return;
    if (d->msi_offset)
        s->host->write(s->context, d->bdf, d->msi_offset + 2, cfg(d, d->msi_offset + 2, 2) & ~1u,
                       2);
    if (d->msix_offset)
        s->host->write(s->context, d->bdf, d->msix_offset + 2,
                       (cfg(d, d->msix_offset + 2, 2) & ~0x8000u) | 0x4000u, 2);
    /* Executors/devices must be quiesced before releasing routes. */
    for (unsigned i = VMM_IRQ_FIRST_VECTOR; i <= VMM_IRQ_LAST_VECTOR; ++i)
        if (__atomic_load_n(&vectors[i].state, __ATOMIC_ACQUIRE) == s) {
            __atomic_store_n(&vectors[i].route, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&vectors[i].state, NULL, __ATOMIC_SEQ_CST);
        }
    while (__atomic_load_n(&irq_readers, __ATOMIC_SEQ_CST))
        __asm__ volatile("pause");
    platform_irq_vector_release(s);
    if (s->msix)
        hal_free_pages(s->msix, (s->entries * sizeof(*s->msix) + 4095) / 4096);
    struct pci_irq_state **link = &active_states;
    while (*link && *link != s)
        link = &(*link)->next;
    if (*link)
        *link = s->next;
    hal_free_pages(s, (sizeof(*s) + 4095) / 4096);
    d->interrupts = NULL;
    page_count = 0;
}
void platform_msi_restart(struct pci_device *d)
{
    struct pci_irq_state *s = d->interrupts;
    if (!s)
        return;
    if (d->msi_offset)
        s->host->write(s->context, d->bdf, d->msi_offset + 2, cfg(d, d->msi_offset + 2, 2) & ~1u,
                       2);
    if (d->msix_offset)
        s->host->write(s->context, d->bdf, d->msix_offset + 2,
                       (cfg(d, d->msix_offset + 2, 2) & ~0x8000u) | 0x4000u, 2);
    for (unsigned i = VMM_IRQ_FIRST_VECTOR; i <= VMM_IRQ_LAST_VECTOR; ++i)
        if (__atomic_load_n(&vectors[i].state, __ATOMIC_ACQUIRE) == s) {
            __atomic_store_n(&vectors[i].route, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&vectors[i].state, NULL, __ATOMIC_SEQ_CST);
        }
    while (__atomic_load_n(&irq_readers, __ATOMIC_SEQ_CST))
        __asm__ volatile("pause");
    platform_irq_vector_release(s);
    s->msi_vectors = 0;
    s->msi_pending = 0;
    memset(s->msi, 0, sizeof(s->msi));
    memset(s->pending, 0, sizeof(s->pending));
    uint8_t *table = s->msix_valid ? d->bars[s->table_bar].hva : NULL;
    for (unsigned i = 0; s->msix && i < s->entries; ++i) {
        s->msix[i] = (struct msi_entry){.control = 1};
        if (table)
            mmio_write(table + s->table + i * 16 + 12, 1, 4);
    }
}
void platform_msi_dump(void)
{
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        const struct pci_irq_state *s = d->interrupts;
        if (s)
            LOGI("IRQ PCI %04x host=%llu injections=%llu rejected=%llu spurious=%llu", d->bdf,
                 (unsigned long long)__atomic_load_n(&s->counters.host, __ATOMIC_RELAXED),
                 (unsigned long long)__atomic_load_n(&s->counters.injected, __ATOMIC_RELAXED),
                 (unsigned long long)__atomic_load_n(&s->counters.rejected, __ATOMIC_RELAXED),
                 (unsigned long long)__atomic_load_n(&s->counters.spurious, __ATOMIC_RELAXED));
        if (s && s->msix_valid && s->msix[0].host_vector) {
            uint8_t *table = d->bars[s->table_bar].hva + s->table;
            LOGI("IRQ PCI %04x MSI-X host control=%x table=%llx address=%llx data=%x mask=%x",
                 d->bdf, s->host->read(s->context, d->bdf, d->msix_offset + 2, 2),
                 (unsigned long long)(d->bars[s->table_bar].base + s->table),
                 (unsigned long long)mmio_read(table, 8), (unsigned)mmio_read(table + 8, 4),
                 (unsigned)mmio_read(table + 12, 4));
            LOGI("IRQ PCI %04x MSI-X hardware PBA=%llx", d->bdf,
                 (unsigned long long)mmio_read(d->bars[s->pba_bar].hva + s->pba, 8));
        }
    }
    for (unsigned i = VMM_IRQ_FIRST_VECTOR; i <= VMM_IRQ_LAST_VECTOR; ++i) {
        const struct vector_binding *b = &vectors[i];
        const struct pci_irq_state *s = __atomic_load_n(&b->state, __ATOMIC_ACQUIRE);
        struct irq_counters c = {__atomic_load_n(&b->counters.host, __ATOMIC_RELAXED),
                                 __atomic_load_n(&b->counters.injected, __ATOMIC_RELAXED),
                                 __atomic_load_n(&b->counters.rejected, __ATOMIC_RELAXED),
                                 __atomic_load_n(&b->counters.spurious, __ATOMIC_RELAXED)};
        if (c.host || c.rejected)
            LOGI(
                "IRQ host vector=%u PCI=%04x host=%llu injections=%llu rejected=%llu spurious=%llu",
                i, s ? s->device->bdf : 0xffff, (unsigned long long)c.host,
                (unsigned long long)c.injected, (unsigned long long)c.rejected,
                (unsigned long long)c.spurious);
    }
}
