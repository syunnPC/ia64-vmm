/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Amd64/Interrupt/Amd64InterruptLines.cpp
 */
/* Shared sources belong to one level line. Hardware IRQs only publish;
 * guest IOSAPIC and architectural state change on the executor. */
#include "platform/iosapic.h"
#include "platform/irq_internal.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/platform.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#define INTX_LINES (IA64VMM_IOSAPIC_GSI_COUNT - 16u)
struct intx_line {
    struct ia64_iosapic *sapic;
    uint32_t host_gsi;
    unsigned vector, waiting, acknowledge, changed;
    bool low;
    uint64_t interrupts, arrival;
};
struct intx_function {
    struct pci_device *device;
    const struct pci_host_ops *host;
    void *context;
    struct intx_line *line;
    unsigned command; /* PCI writer release-publishes the enabled state. */
};
static struct intx_line lines[INTX_LINES];
static struct intx_function functions[PCI_MAX_DEVICES];
static unsigned nlines, nfunctions;
/* ISA edge lines of guest-owned legacy decodes. The host IOAPIC input
 * is edge triggered, so the IRQ path only records the edge; the executor
 * pulses the identity guest GSI, which latches it only while that RTE is
 * an unmasked edge input (masked ISA edges are lost, as on hardware). */
#define ISA_LINES 4u
struct isa_line {
    struct ia64_iosapic *sapic;
    uint32_t host_gsi;
    unsigned irq, vector, pending, changed;
    bool storage;
    uint64_t interrupts;
};
static struct isa_line isa_lines[ISA_LINES];
static unsigned nisa;
static unsigned irq_readers, work_pending;
static void publish_work(void)
{
    __atomic_store_n(&work_pending, 1, __ATOMIC_RELEASE);
    platform_irq_kick(0);
}
static const struct platform_intx_ops *backend;
static void *backend_context;
extern void code_epoch_bump(enum code_device_class) __attribute__((weak));
/* Setup happens with executors stopped. Dispatch the hardware part
 * to the future/idle vCPU 0 host CPU, outside device locks. Runtime line operations
 * reach hardware only from its poll, or its host IRQ before LAPIC EOI. */
enum line_op { LINE_ROUTE, LINE_ISA, LINE_MASK, LINE_COMMAND };
struct line_work {
    enum line_op op;
    unsigned gsi, vector;
    bool low, masked, result;
    struct intx_function *function;
};
static void line_work(void *opaque)
{
    struct line_work *w = opaque;
    if (w->op == LINE_ROUTE)
        w->result = backend->route(backend_context, w->gsi, hal_vcpu_cpu(0), w->vector, w->low);
    else if (w->op == LINE_ISA)
        w->result =
            backend->route_isa(backend_context, w->gsi, hal_vcpu_cpu(0), w->vector, &w->gsi);
    else if (w->op == LINE_COMMAND) {
        struct intx_function *f = w->function;
        f->host->write(f->context, f->device->bdf, 4, f->device->command | 0x400, 2);
        w->result = true;
    } else {
        backend->mask(backend_context, w->gsi, w->masked);
        w->result = true;
    }
}
static bool line_call(struct line_work *w)
{
    unsigned cpu = hal_vcpu_cpu(0);
    if (hal_cpu_index() == cpu)
        line_work(w);
    else {
        if (!hal_cpu_start(cpu, line_work, w))
            return false;
        while (hal_cpu_busy(cpu))
            __asm__ volatile("pause");
    }
    return w->result;
}
static unsigned guest_gsi(const struct intx_line *l)
{
    return 16 + (unsigned)(l - lines);
}
static bool enabled(const struct pci_device *d)
{
    return !(d->command & 0x400) && !(d->msi_offset && (d->irq_config[d->msi_offset + 2] & 1)) &&
           !(d->msix_offset && (d->irq_config[d->msix_offset + 3] & 0x80));
}
static bool used(const struct intx_line *l)
{
    for (unsigned i = 0; i < nfunctions; ++i)
        if (functions[i].line == l &&
            !(__atomic_load_n(&functions[i].command, __ATOMIC_ACQUIRE) & 0x400))
            return true;
    return false;
}
static void rearm(struct intx_line *l)
{
    uint64_t rte = l->sapic->rte[guest_gsi(l) - l->sapic->base];
    bool ready = (rte & (IA64_IOSAPIC_MASK | IA64_IOSAPIC_LEVEL | (1u << 13))) ==
                     (IA64_IOSAPIC_LEVEL | (1u << 13)) &&
                 platform_irq_target((rte >> 56) & 255, (rte >> 48) & 255) >= 0;
    if (!ready || !used(l))
        backend->mask(backend_context, l->host_gsi, true);
    else if (backend->unmask_if_idle)
        backend->unmask_if_idle(backend_context, l->host_gsi, &l->waiting);
    else
        backend->mask(backend_context, l->host_gsi, __atomic_load_n(&l->waiting, __ATOMIC_ACQUIRE));
}
void platform_intx_set_host(const struct platform_intx_ops *ops, void *context)
{
    backend = ops;
    backend_context = context;
}
static void isa_rearm(struct isa_line *l)
{
    uint64_t rte = l->sapic->rte[l->irq - l->sapic->base];
    bool ready = !(rte & IA64_IOSAPIC_MASK) &&
                 platform_irq_target((rte >> 56) & 255, (rte >> 48) & 255) >= 0;
    backend->mask(backend_context, l->host_gsi, !ready);
}
bool platform_intx_prepare_isa(unsigned irq, struct ia64_iosapic *sapic, bool storage)
{
    /* The guest MADT has no overrides: ISA IRQ n is GSI n. SCI keeps 9. */
    if (irq > 15 || irq == IA64VMM_SCI_GSI || irq < sapic->base ||
        irq - sapic->base >= sapic->count)
        return false;
    for (unsigned i = 0; i < nisa; ++i)
        if (isa_lines[i].irq == irq) {
            if (isa_lines[i].sapic != sapic)
                return false;
            isa_lines[i].storage |= storage;
            return true;
        }
    if (!backend || !backend->route_isa) {
        LOGW("ISA IRQ%u: no host IOAPIC edge route; the guest receives no interrupt", irq);
        return true;
    }
    if (nisa == ISA_LINES)
        return false;
    struct isa_line *l = &isa_lines[nisa];
    l->sapic = sapic;
    l->irq = irq;
    l->storage = storage;
    l->vector = platform_irq_vector_reserve(l, 1);
    if (!l->vector) {
        memset(l, 0, sizeof(*l));
        return false;
    }
    struct line_work w = {.op = LINE_ISA, .gsi = irq, .vector = l->vector};
    if (!line_call(&w)) {
        LOGW("ISA IRQ%u: host line absent or level-overridden; the guest receives no interrupt",
             irq);
        platform_irq_vector_release(l);
        memset(l, 0, sizeof(*l));
        return true;
    }
    l->host_gsi = w.gsi;
    __atomic_store_n(&nisa, nisa + 1, __ATOMIC_SEQ_CST);
    LOGI("ISA IRQ%u host GSI=%u guest GSI=%u host vector=%u (edge)", irq, l->host_gsi, irq,
         l->vector);
    return true;
}
bool platform_intx_prepare(struct pci_device *d, const struct pci_host_ops *host, void *context,
                           struct ia64_iosapic *sapic)
{
    if (!d->irq_pin)
        return true;
    uint32_t gsi;
    bool low, level;
    d->irq_line = 255;
    if (!backend || !backend->resolve(backend_context, d->bdf, d->irq_pin, &gsi, &low, &level) ||
        !level) {
        LOGW("PCI %04x INTx pin=%u unresolved host _PRT/_CRS; source remains disabled", d->bdf,
             d->irq_pin);
        return true;
    }
    struct intx_line *l = NULL;
    for (unsigned i = 0; i < nlines; ++i)
        if (lines[i].host_gsi == gsi)
            l = &lines[i];
    if (l && (l->low != low || l->sapic != sapic))
        return false;
    if (!l) {
        if (nlines == INTX_LINES)
            return false;
        l = &lines[nlines];
        l->sapic = sapic;
        l->host_gsi = gsi;
        l->low = low;
        /* Direct guest _PRT GSIs describe PCI level/active-low wiring.
         * This electrical level is independent of the guest's RTE polarity. */
        sapic->input[guest_gsi(l) - sapic->base] = true;
        l->vector = platform_irq_vector_reserve(l, 1);
        struct line_work w = {.op = LINE_ROUTE, .gsi = gsi, .vector = l->vector, .low = low};
        if (!l->vector || !line_call(&w)) {
            platform_irq_vector_release(l);
            memset(l, 0, sizeof(*l));
            return false;
        }
        __atomic_store_n(&nlines, nlines + 1, __ATOMIC_SEQ_CST);
    }
    if (nfunctions == PCI_MAX_DEVICES)
        return false;
    functions[nfunctions++] = (struct intx_function){
        .device = d, .host = host, .context = context, .line = l, .command = d->command | 0x400};
    d->irq_line = guest_gsi(l);
    LOGI("PCI %04x INTx pin=%u host GSI=%u guest GSI=%u host vector=%u", d->bdf, d->irq_pin, gsi,
         d->irq_line, l->vector);
    return true;
}
void platform_intx_config_changed(struct pci_device *d)
{
    struct intx_function *f = NULL;
    for (unsigned i = 0; i < nfunctions; ++i)
        if (functions[i].device == d)
            f = &functions[i];
    if (!f)
        return;
    /* PCI 3.0 §6.2.2: guest InterruptDisable follows the real function.
     * Enabling either message capability suppresses the legacy source. */
    unsigned command = enabled(d) ? d->command : d->command | 0x400;
    /* Called under pci_lock: CF8/CFC and the guest command are one access.
     * Only host line rearm belongs to the owner executor. */
    f->host->write(f->context, d->bdf, 4, command, 2);
    __atomic_store_n(&f->command, command, __ATOMIC_RELEASE);
    __atomic_store_n(&f->line->changed, 1, __ATOMIC_RELEASE);
    publish_work();
}
bool platform_intx_interrupt(unsigned vector)
{
    bool accepted = false;
    __atomic_fetch_add(&irq_readers, 1, __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < __atomic_load_n(&nlines, __ATOMIC_SEQ_CST); ++i) {
        struct intx_line *l = &lines[i];
        if (__atomic_load_n(&l->vector, __ATOMIC_SEQ_CST) != vector)
            continue;
        uint64_t arrival = platform_irq_intx_measuring() ? hal_counter() : 0;
        backend->mask(backend_context, l->host_gsi, true); /* Before host LAPIC EOI. */
        __atomic_fetch_add(&l->interrupts, 1, __ATOMIC_RELAXED);
        bool storage = false;
        for (unsigned j = 0; j < nfunctions; ++j)
            if (functions[j].line == l && functions[j].device->storage)
                storage = true;
        if (storage && code_epoch_bump)
            code_epoch_bump(CODE_DEVICE_STORAGE);
        __atomic_store_n(&l->arrival, arrival, __ATOMIC_RELEASE);
        __atomic_store_n(&l->waiting, 1, __ATOMIC_RELEASE);
        /* No live RTE read on the host IRQ path: the executor owns it. */
        publish_work();
        accepted = true;
        break;
    }
    for (unsigned i = 0; !accepted && i < __atomic_load_n(&nisa, __ATOMIC_SEQ_CST); ++i) {
        struct isa_line *l = &isa_lines[i];
        if (__atomic_load_n(&l->vector, __ATOMIC_SEQ_CST) != vector)
            continue;
        __atomic_fetch_add(&l->interrupts, 1, __ATOMIC_RELAXED);
        if (l->storage && code_epoch_bump)
            code_epoch_bump(CODE_DEVICE_STORAGE);
        __atomic_store_n(&l->pending, 1, __ATOMIC_RELEASE);
        publish_work();
        accepted = true;
    }
    __atomic_fetch_sub(&irq_readers, 1, __ATOMIC_SEQ_CST);
    return accepted;
}
void platform_intx_delivery(struct ia64_iosapic *s, unsigned gsi, unsigned cpu, unsigned vector)
{
    for (unsigned i = 0; i < nlines; ++i) {
        struct intx_line *l = &lines[i];
        if (l->sapic == s && guest_gsi(l) == gsi && __atomic_load_n(&l->waiting, __ATOMIC_ACQUIRE))
            platform_irq_intx_start(cpu, vector,
                                    __atomic_exchange_n(&l->arrival, 0, __ATOMIC_ACQ_REL));
    }
}
void platform_intx_iosapic_eoi(struct ia64_iosapic *s, unsigned gsi)
{
    for (unsigned i = 0; i < nlines; ++i)
        if (lines[i].sapic == s && guest_gsi(&lines[i]) == gsi &&
            __atomic_load_n(&lines[i].waiting, __ATOMIC_ACQUIRE)) {
            /* Hardware will re-evaluate a still asserted level when unmasked;
         * don't synthesize a second assertion from a cached electrical level. */
            unsigned pin = gsi - s->base;
            s->input[pin] = true;
            __atomic_store_n(&lines[i].acknowledge, 1, __ATOMIC_RELEASE);
            publish_work();
        }
}
void platform_intx_local_eoi(struct vcpu *v, unsigned vector)
{
    unsigned cpu = platform_irq_index(v);
    if (cpu == UINT32_MAX)
        return; /* Lockstep shadow has no hardware effects. */
    for (unsigned i = 0; i < nlines; ++i) {
        struct intx_line *l = &lines[i];
        /* RTE writers publish atomically for this timer-EOI fast path.
         * Recheck the complete route under the lock after a match. */
        if ((__atomic_load_n(&l->sapic->rte[guest_gsi(l) - l->sapic->base], __ATOMIC_RELAXED) &
             255) != vector)
            continue;
        platform_iosapic_lock(l->sapic);
        uint64_t rte = l->sapic->rte[guest_gsi(l) - l->sapic->base];
        if ((rte & 255) == vector && (rte & IA64_IOSAPIC_REMOTE_IRR) &&
            platform_irq_target((rte >> 56) & 255, (rte >> 48) & 255) == (int)cpu) {
            __atomic_store_n(&l->acknowledge, 1, __ATOMIC_RELEASE);
            publish_work();
        }
        platform_iosapic_unlock(l->sapic);
    }
}
void platform_intx_iosapic_changed(struct ia64_iosapic *s, unsigned gsi)
{
    for (unsigned i = 0; i < nlines; ++i)
        if (lines[i].sapic == s && guest_gsi(&lines[i]) == gsi)
            __atomic_store_n(&lines[i].changed, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < nisa; ++i)
        if (isa_lines[i].sapic == s && isa_lines[i].irq == gsi)
            __atomic_store_n(&isa_lines[i].changed, 1, __ATOMIC_RELEASE);
    publish_work();
}
void platform_intx_poll(struct vcpu *v)
{
    if (platform_irq_index(v) != 0)
        return;
    if (!__atomic_load_n(&work_pending, __ATOMIC_RELAXED))
        return;
    /* Clear before scanning: a concurrent publisher leaves another pass
     * pending even when this pass consumes its per-line flag. */
    __atomic_exchange_n(&work_pending, 0, __ATOMIC_ACQ_REL);
    for (unsigned i = 0; i < nlines; ++i) {
        struct intx_line *l = &lines[i];
        struct ia64_iosapic *s = l->sapic;
        platform_iosapic_lock(s);
        unsigned pin = guest_gsi(l) - s->base;
        if (__atomic_exchange_n(&l->acknowledge, 0, __ATOMIC_ACQ_REL)) {
            s->input[pin] = true;
            __atomic_store_n(&s->rte[pin], s->rte[pin] & ~(uint64_t)IA64_IOSAPIC_REMOTE_IRR,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&l->waiting, 0, __ATOMIC_RELEASE);
            rearm(l);
        }
        if (__atomic_exchange_n(&l->changed, 0, __ATOMIC_ACQ_REL))
            rearm(l);
        if (__atomic_load_n(&l->waiting, __ATOMIC_ACQUIRE))
            platform_iosapic_set_pin_locked(s, guest_gsi(l), false);
        platform_iosapic_unlock(s);
    }
    for (unsigned i = 0; i < nisa; ++i) {
        struct isa_line *l = &isa_lines[i];
        platform_iosapic_lock(l->sapic);
        if (__atomic_exchange_n(&l->changed, 0, __ATOMIC_ACQ_REL))
            isa_rearm(l);
        if (__atomic_exchange_n(&l->pending, 0, __ATOMIC_ACQ_REL)) {
            /* One electrical pulse per host edge; a level-programmed guest
             * RTE sees a pulse it cannot acknowledge again, never a stuck level. */
            platform_iosapic_set_pin_locked(l->sapic, l->irq, true);
            platform_iosapic_set_pin_locked(l->sapic, l->irq, false);
        }
        platform_iosapic_unlock(l->sapic);
    }
}
bool platform_intx_handoff(struct ia64vmm_handoff *h)
{
    h->irq_version = IA64VMM_HANDOFF_IRQ_VERSION;
    h->irq_route_count = h->irq_bridge_count = 0;
    for (unsigned i = 0; i < nfunctions; ++i) {
        const struct pci_device *d = functions[i].device;
        bool duplicate = false;
        for (unsigned j = 0; j < h->irq_route_count; ++j) {
            const struct ia64vmm_irq_route *r = &h->irq_routes[j];
            if ((r->bdf & ~7u) == (d->bdf & ~7u) && r->pin == d->irq_pin - 1) {
                if (r->gsi != d->irq_line)
                    return false;
                duplicate = true;
            }
        }
        if (!duplicate) {
            if (h->irq_route_count == IA64VMM_HANDOFF_MAX_IRQ_ROUTES)
                return false;
            struct ia64vmm_irq_route *r = &h->irq_routes[h->irq_route_count++];
            r->bdf = d->bdf;
            r->parent = d->parent;
            r->pin = d->irq_pin - 1;
            r->gsi = d->irq_line;
        }
    }
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (d->owner != PCI_OWNER_GUEST || d->header != 1)
            continue;
        if (h->irq_bridge_count == IA64VMM_HANDOFF_MAX_IRQ_ROUTES)
            return false;
        struct ia64vmm_irq_bridge *b = &h->irq_bridges[h->irq_bridge_count++];
        b->bdf = d->bdf;
        b->parent = d->parent;
        b->secondary = d->secondary;
    }
    return true;
}
void platform_intx_restart(void)
{
    for (unsigned i = 0; i < nfunctions; ++i)
        __atomic_store_n(&functions[i].command, functions[i].device->command | 0x400,
                         __ATOMIC_RELEASE);
    for (unsigned i = 0; i < nlines; ++i) {
        struct intx_line *l = &lines[i];
        struct line_work w = {.op = LINE_MASK, .gsi = l->host_gsi, .masked = true};
        if (!line_call(&w))
            hal_panic("INTx restart could not mask host line");
        platform_irq_host_revoke(l->vector, 1);
        platform_irq_host_retire(l->vector, 1);
        w = (struct line_work){
            .op = LINE_ROUTE, .gsi = l->host_gsi, .vector = l->vector, .low = l->low};
        if (!line_call(&w))
            hal_panic("INTx restart could not restore host route");
        __atomic_store_n(&l->waiting, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&l->acknowledge, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&l->changed, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&l->arrival, 0, __ATOMIC_RELEASE);
        l->sapic->input[guest_gsi(l) - l->sapic->base] = true;
    }
    for (unsigned i = 0; i < nisa; ++i) {
        struct isa_line *l = &isa_lines[i];
        struct line_work w = {.op = LINE_MASK, .gsi = l->host_gsi, .masked = true};
        if (!line_call(&w))
            hal_panic("ISA restart could not mask host line");
        platform_irq_host_revoke(l->vector, 1);
        platform_irq_host_retire(l->vector, 1);
        w = (struct line_work){.op = LINE_ISA, .gsi = l->irq, .vector = l->vector};
        if (!line_call(&w) || w.gsi != l->host_gsi)
            hal_panic("ISA restart could not restore host route");
        __atomic_store_n(&l->pending, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&l->changed, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&work_pending, 0, __ATOMIC_RELEASE);
}
void platform_intx_cleanup(void)
{
    unsigned count = nlines, isa = nisa;
    for (unsigned i = 0; i < nfunctions; ++i) {
        struct intx_function *f = &functions[i];
        struct line_work w = {.op = LINE_COMMAND, .function = f};
        /* vm_wait has joined every executor; its stop latch is permanent. */
        line_work(&w);
    }
    for (unsigned i = 0; i < nlines; ++i) {
        backend->mask(backend_context, lines[i].host_gsi, true);
        __atomic_store_n(&lines[i].vector, 0, __ATOMIC_SEQ_CST);
    }
    for (unsigned i = 0; i < nisa; ++i) {
        backend->mask(backend_context, isa_lines[i].host_gsi, true);
        __atomic_store_n(&isa_lines[i].vector, 0, __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(&nlines, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&nisa, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&irq_readers, __ATOMIC_SEQ_CST))
        __asm__ volatile("pause");
    for (unsigned i = 0; i < isa; ++i) {
        const struct isa_line *l = &isa_lines[i];
        LOGI("ISA IRQ%u host GSI=%u interrupts=%llu guest RTE=%016llx", l->irq, l->host_gsi,
             (unsigned long long)l->interrupts,
             (unsigned long long)(l->sapic ? l->sapic->rte[l->irq - l->sapic->base] : 0));
        platform_irq_vector_release(&isa_lines[i]);
    }
    memset(isa_lines, 0, sizeof(isa_lines));
    for (unsigned i = 0; i < count; ++i) {
        LOGI("INTx host GSI=%u guest GSI=%u interrupts=%llu", lines[i].host_gsi,
             guest_gsi(&lines[i]), (unsigned long long)lines[i].interrupts);
        platform_irq_vector_release(&lines[i]);
    }
    memset(lines, 0, sizeof(lines));
    memset(functions, 0, sizeof(functions));
    nfunctions = 0;
    __atomic_store_n(&work_pending, 0, __ATOMIC_RELAXED);
}
