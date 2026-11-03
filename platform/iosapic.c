/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Ia64/Interrupt/Ia64IoSapic.{cpp,hpp}
 */
/* Register layout and level-acknowledge sequencing. The model implements
 * the receiving side of that interface: cr.eoi does not clear remote IRR. */
#include "platform/iosapic.h"
#include "platform/irq_internal.h"
#include "ia64vmm/string.h"

static bool asserted(const struct ia64_iosapic *s, unsigned pin)
{
    return s->input[pin] != !!(s->rte[pin] & (1u << 13));
}
static void deliver(struct ia64_iosapic *s, unsigned pin)
{
    uint64_t rte = s->rte[pin];
    bool level = rte & IA64_IOSAPIC_LEVEL;
    if ((rte & IA64_IOSAPIC_MASK) ||
        (level ? !asserted(s, pin) || (rte & IA64_IOSAPIC_REMOTE_IRR) : !s->edge[pin]))
        return;
    int cpu = platform_irq_target((rte >> 56) & 255, (rte >> 48) & 255);
    unsigned mode = (rte >> 8) & 7;
    /* IOSAPIC lowest-priority delivery uses XTP. Only explicit physical
     * destinations are exposed by this platform; dm=1 selects that target. */
    if (mode == 1)
        mode = IA64_IRQ_INT;
    if (cpu < 0) {
        ++s->rejected;
        return;
    }
    platform_intx_delivery(s, s->base + pin, cpu, rte & 255);
    if (!platform_irq_inject(cpu, rte & 255, mode)) {
        ++s->rejected;
        return;
    }
    if (level)
        __atomic_store_n(&s->rte[pin], rte | IA64_IOSAPIC_REMOTE_IRR, __ATOMIC_RELAXED);
    else
        s->edge[pin] = false;
}
bool platform_iosapic_init(struct ia64_iosapic *s, unsigned base, unsigned count)
{
    if (!count || count > IA64_IOSAPIC_MAX_PINS || base > UINT32_MAX - count)
        return false;
    memset(s, 0, sizeof(*s));
    s->base = base;
    s->count = count;
    for (unsigned i = 0; i < count; ++i)
        s->rte[i] = IA64_IOSAPIC_MASK;
    return true;
}
bool platform_iosapic_set_pin_locked(struct ia64_iosapic *s, unsigned gsi, bool high)
{
    if (gsi < s->base || gsi - s->base >= s->count)
        return false;
    unsigned pin = gsi - s->base;
    bool old = asserted(s, pin);
    s->input[pin] = high;
    if (!old && asserted(s, pin) && !(s->rte[pin] & IA64_IOSAPIC_LEVEL)) {
        /* Masked edges are discarded; level assertions survive masking. */
        if (!(s->rte[pin] & IA64_IOSAPIC_MASK))
            s->edge[pin] = true;
    }
    deliver(s, pin);
    return true;
}
bool platform_iosapic_set_pin(struct ia64_iosapic *s, unsigned gsi, bool high)
{
    platform_iosapic_lock(s);
    bool result = platform_iosapic_set_pin_locked(s, gsi, high);
    platform_iosapic_unlock(s);
    return result;
}
static uint64_t iosapic_read_unlocked(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    (void)v;
    struct ia64_iosapic *s = opaque;
    if (size != 4)
        return 0;
    if (off == 0)
        return s->select;
    if (off != 0x10)
        return 0;
    if (s->select == 0)
        return s->id << 24;
    if (s->select == 1)
        return 0x21u | ((s->count - 1) << 16);
    if (s->select == 2)
        return s->id << 24;
    unsigned pin = (s->select - 0x10u) / 2;
    if (s->select < 0x10 || pin >= s->count)
        return 0;
    return (uint32_t)(s->rte[pin] >> ((s->select & 1) * 32));
}
static uint64_t iosapic_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    platform_iosapic_lock(opaque);
    uint64_t result = iosapic_read_unlocked(opaque, v, off, size);
    platform_iosapic_unlock(opaque);
    return result;
}
static void iosapic_write_unlocked(void *opaque, struct vcpu *v, uint64_t off, uint64_t value,
                                   unsigned size)
{
    (void)v;
    struct ia64_iosapic *s = opaque;
    if (size != 4) {
        ++s->rejected;
        return;
    }
    if (off == 0) {
        s->select = value & 0xff;
        return;
    }
    if (off == 0x40) {
        for (unsigned pin = 0; pin < s->count; ++pin)
            if ((s->rte[pin] & 255) == (value & 255) && (s->rte[pin] & IA64_IOSAPIC_REMOTE_IRR)) {
                platform_intx_iosapic_eoi(s, s->base + pin);
                __atomic_store_n(&s->rte[pin], s->rte[pin] & ~(uint64_t)IA64_IOSAPIC_REMOTE_IRR,
                                 __ATOMIC_RELAXED);
                deliver(s, pin);
            }
        return;
    }
    if (off != 0x10) {
        ++s->rejected;
        return;
    }
    if (s->select == 0) {
        s->id = (value >> 24) & 255;
        return;
    }
    unsigned pin = (s->select - 0x10u) / 2;
    if (s->select < 0x10 || pin >= s->count) {
        ++s->rejected;
        return;
    }
    if (s->select & 1)
        __atomic_store_n(&s->rte[pin], (s->rte[pin] & UINT32_MAX) | ((value & 0xffff0000u) << 32),
                         __ATOMIC_RELAXED);
    else {
        uint64_t remote = s->rte[pin] & IA64_IOSAPIC_REMOTE_IRR;
        /* Remote IRR is read-only, but switching to edge drops it, as the
         * ownership handover sequence requires. Delivery status=0. */
        __atomic_store_n(&s->rte[pin],
                         (s->rte[pin] & ~UINT64_C(0xffffffff)) | (value & 0x1afffu) |
                             ((value & IA64_IOSAPIC_LEVEL) ? remote : 0),
                         __ATOMIC_RELAXED);
    }
    deliver(s, pin);
    platform_intx_iosapic_changed(s, s->base + pin);
}
static void iosapic_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    platform_iosapic_lock(opaque);
    iosapic_write_unlocked(opaque, v, off, value, size);
    platform_iosapic_unlock(opaque);
}
const struct gm_mmio_ops platform_iosapic_ops = {iosapic_read, iosapic_write};
