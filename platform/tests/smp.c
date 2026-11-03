/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ia64/interrupt.h"
#include "core/smp.h"
#include "core/vm.h"
#include "platform/irq_internal.h"
#include "platform/platform.h"
#include "platform/iosapic.h"
#include "tests/hal/hal_linux.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/pci.h"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#include "tests/host/smp/watchdog.h"
#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define STORE(p, x) __atomic_store_n(p, x, __ATOMIC_RELEASE)
static struct vcpu *cpus[2];
static unsigned wakes[3], stop_seen[3];
static bool count_delivery;
static uint64_t deliveries;
bool __real_platform_irq_inject(unsigned, unsigned, unsigned);
bool __wrap_platform_irq_inject(unsigned cpu, unsigned vector, unsigned mode)
{
    if (!count_delivery)
        return __real_platform_irq_inject(cpu, vector, mode);
    CHECK(cpu == 0 && vector == 0x75 && mode == IA64_IRQ_INT);
    __atomic_fetch_add(&deliveries, 1, __ATOMIC_RELAXED);
    /* Widen the actual deliver -> remote IRR publication interval. The
     * unlocked variant must duplicate/lose service at this boundary. */
    sched_yield();
    return true;
}
void __real_hal_cpu_request(unsigned, unsigned);
void __wrap_hal_cpu_request(unsigned cpu, unsigned flags)
{
    if (cpu < 3)
        __atomic_fetch_add(&wakes[cpu], 1, __ATOMIC_RELAXED);
    if (cpu < 3 && (flags & HAL_CPU_STOP))
        STORE(&stop_seen[cpu], 1);
    __real_hal_cpu_request(cpu, flags);
}
uint32_t __real_hal_port_in(uint16_t, unsigned);
uint32_t __wrap_hal_port_in(uint16_t port, unsigned size)
{
    return port == 0x80 || port == 0x81 ? port + (size << 16) : __real_hal_port_in(port, size);
}
static void create_cpus(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        struct vcpu_config cfg = {.index = i};
        cpus[i] = vcpu_create(&cfg);
        CHECK(cpus[i]);
        CHECK(platform_irq_bind(cpus[i], i, (uint64_t)i << 24,
                                hal_vcpu_cpu(i) == UINT32_MAX ? i : hal_vcpu_cpu(i)));
        cpus[i]->s.psr |= IA64_PSR_I;
    }
}
static void destroy_cpus(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        platform_irq_unbind(i);
        vcpu_destroy(cpus[i]);
    }
}
static void reg(struct ia64_iosapic *s, struct vcpu *v, unsigned index, uint32_t value)
{
    platform_iosapic_ops.write(s, v, 0, index, 4);
    platform_iosapic_ops.write(s, v, 0x10, value, 4);
}

static struct ia64_iosapic sapic;
static unsigned ready, go;
static void *iosapic_thread(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    while (!LOAD(&go))
        __asm__ volatile("pause");
    for (unsigned n = 0; n < 250000; ++n) {
        switch ((n + id) % 3) {
        case 0:
            platform_iosapic_ops.write(&sapic, cpus[0], 0x10, 0x75 | IA64_IOSAPIC_LEVEL, 4);
            break;
        case 1:
            platform_iosapic_ops.write(&sapic, cpus[0], 0x40, 0x75, 4);
            break;
        case 2:
            CHECK(platform_iosapic_set_pin(&sapic, 16, true));
            break;
        }
        CHECK(platform_iosapic_ops.read(&sapic, cpus[0], 0x10, 4) & IA64_IOSAPIC_REMOTE_IRR);
        if (!((n + 1) % 1024))
            watchdog_progress();
    }
    return NULL;
}
static void iosapic(void)
{
    create_cpus();
    count_delivery = true;
    CHECK(platform_iosapic_init(&sapic, 0, 24));
    reg(&sapic, cpus[0], 0x30, 0x75 | IA64_IOSAPIC_LEVEL);
    CHECK(platform_iosapic_set_pin(&sapic, 16, true));
    pthread_t threads[4];
    uint64_t expected = 1;
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned n = 0; n < 250000; ++n)
            expected += (n + i) % 3 == 1;
        CHECK(!pthread_create(&threads[i], NULL, iosapic_thread, (void *)(uintptr_t)i));
    }
    while (LOAD(&ready) != 4)
        __asm__ volatile("pause");
    STORE(&go, 1);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(!pthread_join(threads[i], NULL));
    printf(
        "U-IOSAPIC: 4 threads, 1000000 operations, deliveries=%llu expected=%llu remote_IRR=%u\n",
        (unsigned long long)deliveries, (unsigned long long)expected,
        !!(sapic.rte[16] & IA64_IOSAPIC_REMOTE_IRR));
    CHECK(deliveries == expected && !sapic.rejected && sapic.input[16]);
    CHECK(sapic.rte[16] & IA64_IOSAPIC_REMOTE_IRR);
    count_delivery = false;
    destroy_cpus();
}

static struct platform_state state;
static const struct gm_region *service;
static pthread_barrier_t pairs;
static void pair_barrier(void)
{
    int r = pthread_barrier_wait(&pairs);
    CHECK(!r || r == PTHREAD_BARRIER_SERIAL_THREAD);
}
static void *latch_thread(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg, port = 0x80 + id;
    for (unsigned i = 0; i < 10000; ++i) {
        service->ops->write(service->opaque, cpus[id], IA64VMM_SVC_PORT_SELECT, (1u << 16) | port,
                            8);
        pair_barrier(); /* Both selectors complete before either data access. */
        CHECK(service->ops->read(service->opaque, cpus[id], IA64VMM_SVC_PORT_DATA, 8) ==
              (1u << 16) + port);
        pair_barrier();
        if (!id && !((i + 1) % 1024))
            watchdog_progress();
    }
    return NULL;
}
static void latch(void)
{
    create_cpus();
    CHECK(platform_devices_init(&state));
    CHECK(platform_allow_ports(&state, 0x80, 2));
    service = gm_lookup(IA64VMM_SERVICE_BASE);
    CHECK(service && service->ops);
    CHECK(!pthread_barrier_init(&pairs, NULL, 2));
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!pthread_create(&threads[i], NULL, latch_thread, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!pthread_join(threads[i], NULL));
    CHECK(!pthread_barrier_destroy(&pairs));
    platform_devices_cleanup(&state);
    destroy_cpus();
    puts("per-vCPU latch: 20000 interleaved select/data pairs PASS");
}

static unsigned vector, isa_vector, line_ops, command_writes;
static bool masked, isa_masked;
static struct pci_device *device;
static bool lifecycle;
static unsigned host_command, peer_command, racing, transaction;
static bool resolve(void *p, uint16_t bdf, unsigned pin, uint32_t *gsi, bool *low, bool *level)
{
    (void)p;
    CHECK(!bdf && pin == 1);
    *gsi = 37;
    *low = *level = true;
    return true;
}
static void owner(void)
{
    CHECK(hal_cpu_index() == (lifecycle ? 0 : hal_vcpu_cpu(0)));
    __atomic_fetch_add(&line_ops, 1, __ATOMIC_RELAXED);
}
static bool route(void *p, uint32_t gsi, unsigned cpu, unsigned v, bool low)
{
    (void)p;
    owner();
    CHECK(gsi == 37 && cpu == 1 && low);
    vector = v;
    masked = true;
    return true;
}
static bool route_isa(void *p, unsigned irq, unsigned cpu, unsigned v, uint32_t *gsi)
{
    (void)p;
    owner();
    CHECK(irq == 14 && cpu == 1);
    *gsi = 34;
    isa_vector = v;
    isa_masked = true;
    return true;
}
static void mask(void *p, uint32_t gsi, bool m)
{
    (void)p;
    owner();
    CHECK(gsi == 37 || gsi == 34);
    if (gsi == 37)
        masked = m;
    else
        isa_masked = m;
}
static void unmask(void *p, uint32_t gsi, const unsigned *waiting)
{
    mask(p, gsi, LOAD(waiting));
}
static void command(void *p, uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    (void)p;
    if (off != 4 || size != 2 || bdf > 8)
        return; /* Inventory BAR probes. */
    CHECK(value <= 0x407);
    if (LOAD(&racing)) {
        CHECK(hal_cpu_index() == (bdf ? 0 : 2)); /* The poll never writes config. */
        CHECK(!__atomic_exchange_n(&transaction, 1, __ATOMIC_ACQ_REL));
        sched_yield(); /* Model CF8 address/data as two separate accesses. */
        CHECK(LOAD(&transaction) == 1);
        STORE(&transaction, 0);
    }
    __atomic_store_n(bdf ? &peer_command : &host_command, value, __ATOMIC_RELEASE);
    __atomic_fetch_add(&command_writes, 1, __ATOMIC_RELAXED);
}
static uint32_t config_read(void *p, uint16_t bdf, unsigned off, unsigned size)
{
    (void)p;
    (void)size;
    if (bdf != 0 && bdf != 8 && bdf != 16)
        return UINT32_MAX;
    if (!off)
        return 0x12348086;
    if (off == 8)
        return 0x01060100; /* Independent media at 00:02.0. */
    if (off == 4)
        return bdf ? LOAD(&peer_command) : LOAD(&host_command);
    if (off == 0x3d)
        return bdf ? 0 : 1;
    return 0;
}
static const struct platform_intx_ops backend = {resolve, route, mask, unmask, route_isa};
static const struct pci_host_ops host = {.read = config_read, .write = command};
static void inventory(void)
{
    platform_pci_set_host(&host, NULL);
    platform_pci_root_buses(0, 0);
    CHECK(platform_pci_inventory("vmm_media=00:02.0"));
    device = (struct pci_device *)platform_pci_device(0);
    CHECK(device && !device->bdf && device->owner == PCI_OWNER_GUEST);
    command_writes = 0;
}
static void guest_config(void *p)
{
    (void)p;
    CHECK(hal_cpu_index() == 2);
    reg(&sapic, cpus[1], 0x31, 1u << 24);
    reg(&sapic, cpus[1], 0x30, 0x75 | IA64_IOSAPIC_LEVEL | (1u << 13));
    reg(&sapic, cpus[1], 0x2d, 1u << 24);
    reg(&sapic, cpus[1], 0x2c, 0x82);
    platform_pci_write(4, 0, 2);
    platform_irq_poll(cpus[1]);
}
static void owner_poll(void *p)
{
    (void)p;
    platform_irq_poll(cpus[0]);
}
static void host_interrupt(void *p)
{
    (void)p;
    CHECK(platform_intx_interrupt(vector));
    CHECK(platform_intx_interrupt(isa_vector));
}
static void guest_drain(void *p)
{
    (void)p;
    platform_irq_poll(cpus[1]);
    CHECK(ia64_ivr_read(cpus[1]) == 0x82);
    ia64_eoi(cpus[1]);
    CHECK(ia64_ivr_read(cpus[1]) == 0x75);
    ia64_eoi(cpus[1]);
    CHECK(ia64_ivr_read(cpus[1]) == 15);
}
static void guest_disable(void *p)
{
    (void)p;
    platform_pci_write(4, 0x400, 2);
    reg(&sapic, cpus[1], 0x2c, 0x82 | IA64_IOSAPIC_MASK);
    platform_irq_poll(cpus[1]);
}
static void run(unsigned cpu, hal_cpu_work_fn fn)
{
    CHECK(hal_cpu_start(cpu, fn, NULL));
    while (hal_cpu_busy(cpu))
        sched_yield();
    watchdog_progress();
}
static void ownership(void)
{
    CHECK(!hal_linux_configure(true, 2, 0, NULL, 0));
    create_cpus();
    CHECK(platform_iosapic_init(&sapic, 0, 24));
    platform_intx_set_host(&backend, NULL);
    inventory();
    CHECK(platform_intx_prepare(device, &host, NULL, &sapic));
    CHECK(platform_intx_prepare_isa(14, &sapic, false));
    unsigned before = line_ops;
    for (unsigned i = 0; i < 3; ++i)
        STORE(&wakes[i], 0);
    run(2, guest_config);
    CHECK(line_ops == before && masked && isa_masked && command_writes == 2 &&
          !LOAD(&host_command));
    CHECK(LOAD(&wakes[1]) && !LOAD(&wakes[2]));
    run(1, owner_poll);
    CHECK(!masked && !isa_masked && command_writes == 2);
    for (unsigned i = 0; i < 3; ++i)
        STORE(&wakes[i], 0);
    run(1, host_interrupt);
    CHECK(LOAD(&wakes[1]) && !LOAD(&wakes[2]));
    before = line_ops;
    run(1, owner_poll);
    run(2, guest_drain);
    CHECK(line_ops == before && masked);
    CHECK(sapic.rte[16] & IA64_IOSAPIC_REMOTE_IRR);
    run(1, owner_poll);
    CHECK(!masked && !(sapic.rte[16] & IA64_IOSAPIC_REMOTE_IRR));
    before = line_ops;
    run(2, guest_disable);
    CHECK(line_ops == before);
    run(1, owner_poll);
    CHECK(masked && isa_masked);
    lifecycle = true;
    platform_intx_cleanup();
    platform_intx_set_host(NULL, NULL);
    destroy_cpus();
    printf(
        "ownership: %u host operations runtime on host CPU1 (vCPU0), inline teardown CPU0, synchronous PCI, deferred RTE/EOI, INTx+ISA to vCPU1 PASS\n",
        line_ops);
}

static unsigned race_done, race_started;
static void race_poll(void *p)
{
    (void)p;
    STORE(&race_started, 1);
    while (!LOAD(&race_done))
        platform_intx_poll(cpus[0]);
}
static void race_write(void *p)
{
    (void)p;
    for (unsigned i = 0; i < 10000; ++i) {
        platform_pci_write(4, 6, 2);
        CHECK(LOAD(&host_command) == 6);
        platform_pci_write(4, 0, 2);
        CHECK(!LOAD(&host_command));
        if (!((i + 1) % 1024))
            watchdog_progress();
    }
    STORE(&race_done, 1);
}
static void pci_race(void)
{
    CHECK(!hal_linux_configure(true, 2, 0, NULL, 0));
    create_cpus();
    CHECK(platform_iosapic_init(&sapic, 0, 24));
    platform_intx_set_host(&backend, NULL);
    inventory();
    CHECK(platform_intx_prepare(device, &host, NULL, &sapic));
    STORE(&racing, 1);
    CHECK(hal_cpu_start(1, race_poll, NULL));
    while (!LOAD(&race_started))
        sched_yield();
    CHECK(hal_cpu_start(2, race_write, NULL));
    unsigned peer_writes = 0;
    while (!LOAD(&race_done)) {
        platform_pci_write((8u << 12) + 4, 0x400 | (peer_writes++ & 7), 2);
        CHECK(LOAD(&peer_command) == (0x400 | ((peer_writes - 1) & 7)));
    }
    while (hal_cpu_busy(1) || hal_cpu_busy(2))
        sched_yield();
    CHECK(peer_writes);
    STORE(&racing, 0);
    CHECK(!LOAD(&host_command));
    lifecycle = true;
    platform_intx_cleanup();
    destroy_cpus();
    puts(
        "PCI race: 20000 synchronous command writes versus owner poll and a second BDF writer through platform_pci_write PASS");
}

static unsigned teardown_mode, entered;
struct vcpu_exit __wrap_engine_run(struct vcpu *v, uint64_t budget)
{
    (void)budget;
    if (!v->cfg.index) {
        STORE(&entered, 1);
        if (teardown_mode == 2 || teardown_mode == 4)
            while (!LOAD(&vm_current()->stop))
                sched_yield();
        return (struct vcpu_exit){.reason = VCPU_EXIT_HALT};
    }
    while (!(teardown_mode == 4 ? LOAD(&entered) : LOAD(&cpus[0]->parked)))
        sched_yield();
    return (struct vcpu_exit){.reason = VCPU_EXIT_UNIMPLEMENTED,
                              .detail = "teardown fixture terminal"};
}
static void hold_stopped(void *ctx, struct vm_cpu *c)
{
    (void)ctx;
    /* Keep vCPU0 visibly running until management issues HAL_CPU_STOP.
     * Reproduce the finish-before-STOPPED interval. */
    if (!c->index)
        while (!LOAD(&stop_seen[1]))
            sched_yield();
}
static void teardown(unsigned mode)
{
    teardown_mode = mode;
    unsigned count = mode <= 2 ? 1 : 2;
    CHECK(!hal_linux_configure(true, count, 0, NULL, 0));
    struct vm *vm = vm_create(count, &state, false);
    CHECK(vm);
    create_cpus();
    CHECK(platform_iosapic_init(&sapic, 0, 24));
    platform_intx_set_host(&backend, NULL);
    inventory();
    CHECK(platform_intx_prepare(device, &host, NULL, &sapic));
    CHECK(platform_intx_prepare_isa(14, &sapic, false));
    for (unsigned i = 0; i < count; ++i) {
        vm->cpu[i].v = cpus[i];
        vm->cpu[i].host_cpu = i + 1;
    }
    static const struct vm_exec_ops ops = {.stopped = hold_stopped};
    struct vm_exec exec = {.ops = &ops, .start = hal_counter(), .hz = hal_counter_hz()};
    CHECK(vm_start(vm, &exec));
    watchdog_progress();
    while (!(mode == 1 || mode == 3 ? LOAD(&cpus[0]->parked) : LOAD(&entered)))
        sched_yield();
    watchdog_progress();
    if (count == 2)
        CHECK(platform_irq_inject(1, 0xf0, IA64_IRQ_INT));
    int code = vm_wait(vm, count == 1 ? hal_counter() : UINT64_MAX);
    CHECK(code == (count == 1 ? VMM_EXIT_TIMEOUT : VMM_EXIT_UNIMPLEMENTED));
    watchdog_progress();
    for (unsigned i = 0; i < count; ++i)
        CHECK(LOAD(&vm->cpu[i].state) == VM_CPU_STOPPED && !hal_cpu_busy(i + 1));
    CHECK(!hal_cpu_start(1, owner_poll, NULL)); /* Permanent stop latch. */
    unsigned before = line_ops;
    lifecycle = true;
    platform_intx_cleanup();
    CHECK(line_ops == before + 2 && masked && isa_masked && (LOAD(&host_command) & 0x400));
    destroy_cpus();
    vm_destroy(vm);
    printf("teardown mode=%u: real vm_wait/stop-all, retired CPU1, inline CPU0 cleanup PASS\n",
           mode);
}

/* Four roles meet at each host assertion. The owner must complete exactly
 * one delivery and acknowledge before the next assertion, while the other
 * three roles race RTE mask writes, IOSAPIC EOI and local EOI. */
static unsigned cycle_done, cycle_arrived;
static pthread_barrier_t cycles;
static void cycle_barrier(void)
{
    int r = pthread_barrier_wait(&cycles);
    CHECK(!r || r == PTHREAD_BARRIER_SERIAL_THREAD);
}
static void intx_cycles(void *arg)
{
    (void)arg;
    for (unsigned n = 0; n < 250000; ++n) {
        STORE(&cycle_done, 0);
        STORE(&cycle_arrived, 0);
        cycle_barrier();
        CHECK(platform_intx_interrupt(vector));
        while (LOAD(&deliveries) == n)
            platform_intx_poll(cpus[0]);
        STORE(&cycle_arrived, 1);
        for (;;) {
            platform_intx_poll(cpus[0]);
            platform_iosapic_lock(&sapic);
            bool idle = !(sapic.rte[16] & IA64_IOSAPIC_REMOTE_IRR) && sapic.input[16];
            platform_iosapic_unlock(&sapic);
            if (idle)
                break;
        }
        STORE(&cycle_done, 1);
        cycle_barrier();
        platform_intx_poll(cpus[0]);
        CHECK(LOAD(&deliveries) == n + 1);
        if (!((n + 1) % 1024))
            watchdog_progress();
    }
}
static void *intx_peer(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    for (unsigned n = 0; n < 250000; ++n) {
        cycle_barrier();
        if (!id) {
            platform_iosapic_ops.write(&sapic, cpus[1], 0x10,
                                       0x75 | IA64_IOSAPIC_LEVEL | (1u << 13) | IA64_IOSAPIC_MASK,
                                       4);
            platform_iosapic_ops.write(&sapic, cpus[1], 0x10,
                                       0x75 | IA64_IOSAPIC_LEVEL | (1u << 13), 4);
        } else {
            while (!LOAD(&cycle_arrived))
                sched_yield();
            while (!LOAD(&cycle_done)) {
                if (id == 1)
                    platform_iosapic_ops.write(&sapic, cpus[1], 0x40, 0x75, 4);
                else
                    platform_intx_local_eoi(cpus[0], 0x75);
            }
        }
        cycle_barrier();
    }
    return NULL;
}
static void iosapic_intx(void)
{
    CHECK(!hal_linux_configure(true, 2, 0, NULL, 0));
    create_cpus();
    CHECK(platform_iosapic_init(&sapic, 0, 24));
    platform_intx_set_host(&backend, NULL);
    inventory();
    CHECK(platform_intx_prepare(device, &host, NULL, &sapic));
    run(2, guest_config);
    reg(&sapic, cpus[0], 0x31, 0); /* Count injections instead of touching guest state. */
    reg(&sapic, cpus[0], 0x30, 0x75 | IA64_IOSAPIC_LEVEL | (1u << 13));
    count_delivery = true;
    CHECK(!pthread_barrier_init(&cycles, NULL, 4));
    pthread_t peers[3];
    for (unsigned i = 0; i < 3; ++i)
        CHECK(!pthread_create(&peers[i], NULL, intx_peer, (void *)(uintptr_t)i));
    run(1, intx_cycles);
    for (unsigned i = 0; i < 3; ++i)
        CHECK(!pthread_join(peers[i], NULL));
    CHECK(!pthread_barrier_destroy(&cycles));
    CHECK(deliveries == 250000 && !sapic.rejected && sapic.input[16]);
    CHECK(!(sapic.rte[16] & IA64_IOSAPIC_REMOTE_IRR));
    count_delivery = false;
    lifecycle = true;
    platform_intx_cleanup();
    destroy_cpus();
    puts(
        "U-IOSAPIC INTx: four roles, 1000000 role iterations, 250000 exact IRQ/delivery/ack cycles PASS");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    watchdog_start(argv[1], 120);
    double load[3] = {0};
    getloadavg(load, 3);
    printf("load_before=%.2f/%.2f/%.2f\n", load[0], load[1], load[2]);
    fflush(stdout);
    uint64_t start = hal_counter();
    if (!strcmp(argv[1], "iosapic"))
        iosapic();
    else if (!strcmp(argv[1], "latch"))
        latch();
    else if (!strcmp(argv[1], "ownership"))
        ownership();
    else if (!strcmp(argv[1], "pci-race"))
        pci_race();
    else if (!strcmp(argv[1], "iosapic-intx"))
        iosapic_intx();
    else if (!strncmp(argv[1], "teardown-", 9))
        teardown(atoi(argv[1] + 9));
    else
        CHECK(false);
    alarm(0);
    getloadavg(load, 3);
    printf("%s elapsed=%.6f s load_after=%.2f/%.2f/%.2f\n", argv[1],
           (double)(hal_counter() - start) / hal_counter_hz(), load[0], load[1], load[2]);
    return 0;
}
