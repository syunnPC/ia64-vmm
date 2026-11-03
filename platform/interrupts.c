/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64vmm/interrupts.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "platform/platform.h"
#include "platform/irq_internal.h"

struct irq_route {
    struct vcpu *vcpu;
    uint64_t lid, pending[4];
    /* INIT (bit 0) and PMI vectors 0-15 (bits 16-31) for the vCPU. */
    unsigned pal, host_cpu, senders;
    uint8_t xtp;
    uint64_t injections[256], rejected;
    uint64_t latency_start, latency_count, latency_sum, latency_min, latency_max;
    uint64_t intx_start, intx_count, intx_sum, intx_max;
};
static struct irq_route routes[VMM_IRQ_MAX_CPUS];
static unsigned measured_vector, measured_intx_vector;
static unsigned measure_vector(const char *cmdline, const char *key)
{
    const char *value;
    size_t length;
    unsigned vector = 0;
    if (!platform_cmdline_value(cmdline, key, &value, &length) || !length)
        return 0;
    for (size_t i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9' || vector > 255)
            return 0;
        vector = vector * 10 + value[i] - '0';
    }
    return vector >= 16 && vector <= 255 ? vector : 0;
}
void platform_irq_measure_cmdline(const char *cmdline)
{
    measured_vector = measure_vector(cmdline, "irq_latency");
    measured_intx_vector = measure_vector(cmdline, "intx_latency");
}
void platform_irq_ivt(struct vcpu *v, unsigned vector)
{
    if (vector != measured_vector && vector != measured_intx_vector)
        return;
    unsigned cpu = platform_irq_index(v);
    if (cpu == UINT32_MAX)
        return;
    struct irq_route *r = &routes[cpu];
    if (vector == measured_intx_vector) {
        uint64_t start = __atomic_exchange_n(&r->intx_start, 0, __ATOMIC_ACQ_REL);
        if (start) {
            uint64_t ticks = hal_counter() - start;
            r->intx_sum += ticks;
            ++r->intx_count;
            if (ticks > r->intx_max)
                r->intx_max = ticks;
        }
    }
    if (vector != measured_vector)
        return;
    uint64_t start = __atomic_exchange_n(&r->latency_start, 0, __ATOMIC_ACQ_REL);
    if (!start)
        return;
    uint64_t ticks = hal_counter() - start;
    if (!r->latency_count || ticks < r->latency_min)
        r->latency_min = ticks;
    if (ticks > r->latency_max)
        r->latency_max = ticks;
    r->latency_sum += ticks;
    ++r->latency_count;
}
bool platform_irq_intx_measuring(void)
{
    return measured_intx_vector != 0;
}
void platform_irq_intx_start(unsigned cpu, unsigned vector, uint64_t ticks)
{
    if (cpu < VMM_IRQ_MAX_CPUS && vector == measured_intx_vector)
        __atomic_store_n(&routes[cpu].intx_start, ticks, __ATOMIC_RELEASE);
}
static void *vector_owners[256];
static unsigned vector_lock;
static const struct platform_host_irq_ops *host;
void platform_irq_set_host(const struct platform_host_irq_ops *ops)
{
    host = ops;
}
bool platform_irq_remapped(void)
{
    return host && host->remapped;
}
bool platform_irq_host_message(uint16_t bdf, unsigned cpu, unsigned first, unsigned count,
                               uint64_t *address, uint32_t *data)
{
    return host && host->message && count && count <= 32 && !(count & (count - 1)) &&
           first >= VMM_IRQ_FIRST_VECTOR && first <= VMM_IRQ_LAST_VECTOR &&
           count - 1 <= VMM_IRQ_LAST_VECTOR - first && !(first & (count - 1)) &&
           host->message(bdf, cpu, first, count, address, data);
}
void platform_irq_host_revoke(unsigned first, unsigned count)
{
    if (host && host->revoke)
        host->revoke(first, count);
}
void platform_irq_host_retire(unsigned first, unsigned count)
{
    if (!host || !host->retire)
        return;
    uint64_t retired[4] = {0};
    for (unsigned i = 0; i < count; ++i)
        retired[(first + i) / 64] |= UINT64_C(1) << ((first + i) % 64);
    host->retire(retired);
}
unsigned platform_irq_vector_reserve(void *owner, unsigned count)
{
    if (!owner || !count || count > 32 || (count & (count - 1)))
        return 0;
    platform_lock(&vector_lock);
    for (unsigned first = VMM_IRQ_FIRST_VECTOR; first + count - 1 <= VMM_IRQ_LAST_VECTOR;
         first += count) {
        unsigned i;
        for (i = 0; i < count && !vector_owners[first + i]; ++i) {
        }
        if (i != count)
            continue;
        for (i = 0; i < count; ++i)
            vector_owners[first + i] = owner;
        platform_unlock(&vector_lock);
        return first;
    }
    platform_unlock(&vector_lock);
    return 0;
}
void platform_irq_vector_release(void *owner)
{
    if (!owner)
        return;
    uint64_t retired[4] = {0};
    platform_lock(&vector_lock);
    for (unsigned i = VMM_IRQ_FIRST_VECTOR; i <= VMM_IRQ_LAST_VECTOR; ++i)
        if (vector_owners[i] == owner)
            retired[i / 64] |= UINT64_C(1) << (i % 64);
    platform_unlock(&vector_lock);
    if (!(retired[0] | retired[1] | retired[2] | retired[3]))
        return;
    for (unsigned i = VMM_IRQ_FIRST_VECTOR; i <= VMM_IRQ_LAST_VECTOR;) {
        if (!(retired[i / 64] & (UINT64_C(1) << (i % 64)))) {
            ++i;
            continue;
        }
        unsigned first = i++;
        while (i <= VMM_IRQ_LAST_VECTOR && (retired[i / 64] & (UINT64_C(1) << (i % 64))))
            ++i;
        platform_irq_host_revoke(first, i - first);
    }
    if (host && host->retire)
        host->retire(retired);
    platform_lock(&vector_lock);
    for (unsigned i = VMM_IRQ_FIRST_VECTOR; i <= VMM_IRQ_LAST_VECTOR; ++i)
        if (vector_owners[i] == owner)
            vector_owners[i] = NULL;
    platform_unlock(&vector_lock);
}
unsigned platform_irq_index(const struct vcpu *v)
{
    for (unsigned i = 0; i < VMM_IRQ_MAX_CPUS; ++i)
        if (__atomic_load_n(&routes[i].vcpu, __ATOMIC_ACQUIRE) == v)
            return i;
    return UINT32_MAX;
}
/* The platform-only fixture deliberately substitutes the CPU/validator. */
extern bool vcpu_accept_interrupt(struct vcpu *, unsigned) __attribute__((weak));
extern bool vcpu_accept_pal_interrupt(struct vcpu *, unsigned, unsigned) __attribute__((weak));
extern uint64_t vcpu_interrupt_lid(const struct vcpu *) __attribute__((weak));
extern void vcpu_kick(struct vcpu *) __attribute__((weak));
void platform_irq_kick(unsigned index)
{
    if (index >= VMM_IRQ_MAX_CPUS)
        return;
    struct irq_route *r = &routes[index];
    __atomic_fetch_add(&r->senders, 1, __ATOMIC_SEQ_CST);
    struct vcpu *v = __atomic_load_n(&r->vcpu, __ATOMIC_SEQ_CST);
    if (v) {
        if (vcpu_kick)
            vcpu_kick(v);
        hal_cpu_request(__atomic_load_n(&r->host_cpu, __ATOMIC_ACQUIRE), HAL_CPU_WAKE);
    }
    __atomic_fetch_sub(&r->senders, 1, __ATOMIC_SEQ_CST);
}

bool platform_irq_bind(struct vcpu *v, unsigned index, uint64_t lid, unsigned cpu)
{
    if (!v || index >= VMM_IRQ_MAX_CPUS || cpu == UINT32_MAX || (lid & ~UINT64_C(0xffff0000)))
        return false;
    int existing = platform_irq_target(lid >> 24, (lid >> 16) & 255);
    if (existing >= 0 && (unsigned)existing != index)
        return false;
    struct irq_route *r = &routes[index];
    if (__atomic_load_n(&r->vcpu, __ATOMIC_ACQUIRE))
        return false;
    r->lid = lid;
    r->host_cpu = cpu;
    r->pal = 0;
    r->xtp = 0;
    for (unsigned i = 0; i < 4; ++i)
        r->pending[i] = 0;
    for (unsigned i = 0; i < 256; ++i)
        r->injections[i] = 0;
    r->rejected = 0;
    r->latency_start = r->latency_count = r->latency_sum = r->latency_min = r->latency_max = 0;
    __atomic_store_n(&r->vcpu, v, __ATOMIC_RELEASE);
    return true;
}
void platform_irq_unbind(unsigned index)
{
    if (index < VMM_IRQ_MAX_CPUS) {
        __atomic_store_n(&routes[index].vcpu, NULL, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&routes[index].senders, __ATOMIC_SEQ_CST))
            __asm__ volatile("pause");
    }
}
bool platform_irq_lid(unsigned index, uint64_t lid)
{
    if (index >= VMM_IRQ_MAX_CPUS || (lid & ~UINT64_C(0xffff0000)) ||
        !__atomic_load_n(&routes[index].vcpu, __ATOMIC_ACQUIRE))
        return false;
    int target = platform_irq_target(lid >> 24, (lid >> 16) & 255);
    if (target >= 0 && (unsigned)target != index)
        return false;
    __atomic_store_n(&routes[index].lid, lid, __ATOMIC_RELEASE);
    return true;
}
bool platform_irq_update_lid(struct vcpu *v, uint64_t lid)
{
    for (unsigned i = 0; i < VMM_IRQ_MAX_CPUS; ++i)
        if (__atomic_load_n(&routes[i].vcpu, __ATOMIC_ACQUIRE) == v)
            return platform_irq_lid(i, lid);
    return true; /* Standalone CPU and lockstep shadow have no platform route. */
}
int platform_irq_target(unsigned id, unsigned eid)
{
    uint64_t lid = ((uint64_t)id << 24) | ((uint64_t)eid << 16);
    for (unsigned i = 0; i < VMM_IRQ_MAX_CPUS; ++i)
        if (__atomic_load_n(&routes[i].vcpu, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&routes[i].lid, __ATOMIC_ACQUIRE) == lid)
            return i;
    return -1;
}
unsigned platform_irq_host_cpu(unsigned index)
{
    return index < VMM_IRQ_MAX_CPUS && __atomic_load_n(&routes[index].vcpu, __ATOMIC_ACQUIRE)
               ? __atomic_load_n(&routes[index].host_cpu, __ATOMIC_ACQUIRE)
               : UINT32_MAX;
}
bool platform_irq_relocate(unsigned index, unsigned cpu)
{
    unsigned old = platform_irq_host_cpu(index);
    if (old == UINT32_MAX || cpu == UINT32_MAX)
        return false;
    __atomic_store_n(&routes[index].host_cpu, cpu, __ATOMIC_SEQ_CST);
    platform_msi_retarget(index);
    /* Publish placement before the sender reads it. If a sender observed
     * the old placement it wakes that CPU; this wake covers the new CPU.
     * Pending vectors stay in the route mailbox throughout relocation. */
    hal_cpu_request(old, HAL_CPU_WAKE);
    hal_cpu_request(cpu, HAL_CPU_WAKE);
    return true;
}
static bool inject(unsigned index, unsigned vector, unsigned mode, struct vcpu *v)
{
    if (platform_irq_host_cpu(index) == UINT32_MAX)
        return false;
    struct irq_route *r = &routes[index];
    if (mode == IA64_IRQ_NMI)
        vector = 2;
    else if (mode == IA64_IRQ_EXTINT)
        vector = 0;
    else if (mode == IA64_IRQ_INIT || (mode == IA64_IRQ_PMI && vector <= 15)) {
        /* PAL-based interruptions: the executor's poll
         * hands them to the vCPU, which decides delivery or a stop. */
        __atomic_fetch_or(&r->pal, mode == IA64_IRQ_INIT ? 1u : 1u << (16 + vector),
                          __ATOMIC_SEQ_CST);
        if (vcpu_kick)
            vcpu_kick(v);
        unsigned cpu = __atomic_load_n(&r->host_cpu, __ATOMIC_SEQ_CST);
        hal_cpu_request(cpu, HAL_CPU_WAKE);
        unsigned now = __atomic_load_n(&r->host_cpu, __ATOMIC_SEQ_CST);
        if (now != cpu)
            hal_cpu_request(now, HAL_CPU_WAKE);
        return true;
    } else if (mode != IA64_IRQ_INT ||
               (vector != 0 && vector != 2 && (vector < 16 || vector > 255))) {
        __atomic_fetch_add(&r->rejected, 1, __ATOMIC_RELAXED);
        return false;
    }
    if (measured_vector && vector == measured_vector)
        __atomic_store_n(&r->latency_start, hal_counter(), __ATOMIC_RELEASE);
    __atomic_fetch_or(&r->pending[vector / 64], UINT64_C(1) << (vector % 64), __ATOMIC_SEQ_CST);
    if (vcpu_kick)
        vcpu_kick(v);
    __atomic_fetch_add(&r->injections[vector], 1, __ATOMIC_RELAXED);
    unsigned cpu = __atomic_load_n(&r->host_cpu, __ATOMIC_SEQ_CST);
    hal_cpu_request(cpu, HAL_CPU_WAKE);
    /* Pairing with relocate: either relocate wakes the new CPU after this
     * publish, or this read observes its new placement. */
    unsigned now = __atomic_load_n(&r->host_cpu, __ATOMIC_SEQ_CST);
    if (now != cpu)
        hal_cpu_request(now, HAL_CPU_WAKE);
    return true;
}
bool platform_irq_inject(unsigned index, unsigned vector, unsigned mode)
{
    if (index >= VMM_IRQ_MAX_CPUS)
        return false;
    struct irq_route *r = &routes[index];
    __atomic_fetch_add(&r->senders, 1, __ATOMIC_SEQ_CST);
    struct vcpu *v = __atomic_load_n(&r->vcpu, __ATOMIC_SEQ_CST);
    bool accepted = v && inject(index, vector, mode, v);
    __atomic_fetch_sub(&r->senders, 1, __ATOMIC_SEQ_CST);
    return accepted;
}
void platform_irq_poll(struct vcpu *v)
{
    platform_intx_poll(v);
    for (unsigned n = 0; n < VMM_IRQ_MAX_CPUS; ++n) {
        struct irq_route *r = &routes[n];
        if (__atomic_load_n(&r->vcpu, __ATOMIC_ACQUIRE) != v)
            continue;
        if (vcpu_interrupt_lid && !platform_irq_lid(n, vcpu_interrupt_lid(v))) {
            LOGE("duplicate guest LID on vCPU%u", n);
            vcpu_request_exit(v, VCPU_EXIT_UNIMPLEMENTED);
        }
        unsigned pal = __atomic_exchange_n(&r->pal, 0, __ATOMIC_ACQ_REL);
        if (pal && !vcpu_accept_pal_interrupt) {
            LOGE("vCPU%u INIT/PMI without a PAL-based interruption model", n);
            vcpu_request_exit(v, VCPU_EXIT_UNIMPLEMENTED);
        } else if (pal) {
            if (pal & 1)
                vcpu_accept_pal_interrupt(v, IA64_IRQ_INIT, 0);
            for (unsigned vector = 0; vector < 16; ++vector)
                if (pal & (1u << (16 + vector)))
                    vcpu_accept_pal_interrupt(v, IA64_IRQ_PMI, vector);
        }
        if (!vcpu_accept_interrupt)
            return;
        for (unsigned i = 0; i < 4; ++i) {
            uint64_t bits = __atomic_exchange_n(&r->pending[i], 0, __ATOMIC_ACQ_REL);
            while (bits) {
                unsigned bit = __builtin_ctzll(bits);
                vcpu_accept_interrupt(v, i * 64 + bit);
                bits &= bits - 1;
            }
        }
        return;
    }
}
uint8_t platform_irq_xtp(struct vcpu *v)
{
    for (unsigned i = 0; i < VMM_IRQ_MAX_CPUS; ++i)
        if (routes[i].vcpu == v)
            return routes[i].xtp;
    return 0;
}
void platform_irq_set_xtp(struct vcpu *v, uint8_t value)
{
    for (unsigned i = 0; i < VMM_IRQ_MAX_CPUS; ++i)
        if (routes[i].vcpu == v) {
            routes[i].xtp = value;
            return;
        }
}
void platform_irq_dump(void)
{
    for (unsigned i = 0; i < VMM_IRQ_MAX_CPUS; ++i)
        if (__atomic_load_n(&routes[i].vcpu, __ATOMIC_ACQUIRE)) {
            struct irq_route *r = &routes[i];
            if (r->intx_count)
                LOGI(
                    "INTx latency vCPU%u vector=%u samples=%llu hz=%llu ticks_mean=%llu ticks_max=%llu",
                    i, measured_intx_vector, (unsigned long long)r->intx_count,
                    (unsigned long long)hal_counter_hz(),
                    (unsigned long long)(r->intx_sum / r->intx_count),
                    (unsigned long long)r->intx_max);
            if (r->latency_count)
                LOGI(
                    "IRQ latency vCPU%u vector=%u samples=%llu hz=%llu ticks_mean=%llu ticks_min=%llu ticks_max=%llu",
                    i, measured_vector, (unsigned long long)r->latency_count,
                    (unsigned long long)hal_counter_hz(),
                    (unsigned long long)(r->latency_sum / r->latency_count),
                    (unsigned long long)r->latency_min, (unsigned long long)r->latency_max);
            for (unsigned v = 0; v < 256; ++v) {
                uint64_t n = __atomic_load_n(&routes[i].injections[v], __ATOMIC_RELAXED);
                if (n)
                    LOGI("IRQ vCPU%u vector=%u injections=%llu", i, v, (unsigned long long)n);
            }
            LOGI("IRQ vCPU%u rejected=%llu", i,
                 (unsigned long long)__atomic_load_n(&routes[i].rejected, __ATOMIC_RELAXED));
        }
}
