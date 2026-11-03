/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "ia64vmm/hal.h"
#include "ia64vmm/pci.h"
#include "platform/platform.h"
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            abort();                                                                               \
    } while (0)
static bool resolve(void *p, uint16_t bdf, unsigned pin, uint32_t *gsi, bool *low, bool *level)
{
    (void)p;
    (void)pin;
    *gsi = 32 + bdf;
    *low = *level = true;
    return true;
}
static bool route(void *p, uint32_t gsi, unsigned cpu, unsigned vector, bool low)
{
    (void)p;
    (void)gsi;
    (void)cpu;
    (void)vector;
    (void)low;
    return true;
}
static void mask(void *p, uint32_t gsi, bool masked)
{
    (void)p;
    (void)gsi;
    (void)masked;
}
static void command(void *p, uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    (void)p;
    (void)bdf;
    (void)off;
    (void)value;
    (void)size;
}
int main(void)
{
    cpu_set_t allowed, pin;
    CHECK(!sched_getaffinity(0, sizeof(allowed), &allowed));
    int cpu = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &allowed))
            cpu = i;
    CPU_ZERO(&pin);
    CPU_SET(cpu, &pin);
    CHECK(!sched_setaffinity(0, sizeof(pin), &pin));
    struct vcpu_config cfg = {0};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v);
    CHECK(platform_irq_bind(v, 0, 0, 0));
    struct ia64_iosapic sapic;
    CHECK(platform_iosapic_init(&sapic, 0, 24));
    static const struct platform_intx_ops ops = {.resolve = resolve, .route = route, .mask = mask};
    static const struct pci_host_ops host = {.write = command};
    struct pci_device devices[2] = {{.bdf = 0, .irq_pin = 1, .command = 0x400},
                                    {.bdf = 1, .irq_pin = 1, .command = 0x400}};
    platform_intx_set_host(&ops, NULL);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(platform_intx_prepare(&devices[i], &host, NULL, &sapic));
    double load[3];
    getloadavg(load, 3);
    const unsigned n = 2000000;
    for (unsigned i = 0; i < 10000; ++i)
        platform_irq_poll(v);
    uint64_t start = hal_counter();
    for (unsigned i = 0; i < n; ++i)
        platform_irq_poll(v);
    uint64_t poll = hal_counter() - start;
    start = hal_counter();
    for (unsigned i = 0; i < n; ++i)
        platform_intx_local_eoi(v, 0xef);
    uint64_t eoi = hal_counter() - start;
    printf("cpu=%d iterations=%u poll_ns=%.3f eoi_ns=%.3f load=%.2f/%.2f/%.2f\n", cpu, n,
           (double)poll / n, (double)eoi / n, load[0], load[1], load[2]);
    platform_intx_cleanup();
    platform_irq_unbind(0);
    vcpu_destroy(v);
    return 0;
}
