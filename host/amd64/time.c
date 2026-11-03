/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Amd64/Time/Amd64TscClock.cpp
 */
#include "host/amd64/amd64.h"
#include "host/amd64/calibration.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"

static uint64_t little(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static uint64_t timestamp(void *unused)
{
    (void)unused;
    return hal_counter();
}
static uint32_t pit_counter(void *unused)
{
    (void)unused;
    hal_port_out(0x43, 0x80, 1);
    uint32_t lo = hal_port_in(0x42, 1), hi = hal_port_in(0x42, 1);
    return (0x10000u - ((hi << 8) | lo)) & 0xffff;
}
static uint32_t port_counter(void *ctx)
{
    return hal_port_in((uintptr_t)ctx, 4);
}
static uint32_t mmio_counter(void *ctx)
{
    return *(volatile uint32_t *)ctx;
}
uint64_t amd64_calibrate_tsc(const struct boot_info *bi, const char **source)
{
    /* Mode-2 PIT reference. Restore speaker/gate even if it is
     * absent or gated; never wait for OUT2 without observing count progress. */
    uint8_t old = hal_port_in(0x61, 1);
    hal_port_out(0x61, old & ~3u, 1);
    hal_port_out(0x43, 0xb4, 1);
    hal_port_out(0x42, 0, 1);
    hal_port_out(0x42, 0, 1);
    hal_port_out(0x61, (old & ~3u) | 1, 1);
    struct amd64_clock_reference r = {pit_counter, timestamp, NULL};
    uint64_t hz = amd64_measure_reference(&r, 0xffff, 1193182);
    hal_port_out(0x61, old, 1);
    if (hz) {
        *source = "PIT channel 2 calibration";
        return hz;
    }
    LOGW("PIT absent/gated or invalid calibration; trying ACPI PM timer / HPET");
    const uint8_t *fadt = amd64_acpi_table(bi, "FACP");
    if (fadt && little(fadt + 4, 4) >= 116) {
        unsigned len = little(fadt + 4, 4);
        uint64_t address = little(fadt + 76, 4);
        unsigned space = 1;
        bool valid = fadt[91] == 4 && address && address <= 0xfffc;
        if (len >= 220 && little(fadt + 212, 8)) {
            address = little(fadt + 212, 8);
            space = fadt[208];
            valid = fadt[209] >= 24 && !fadt[210] && space <= 1 &&
                    (space == 0 ? !(address & 3) : address <= 0xfffc);
        }
        if (valid) {
            r.counter = space ? port_counter : mmio_counter;
            r.context = space ? (void *)(uintptr_t)address : amd64_map_mmio(address, 4);
            if (r.context)
                hz = amd64_measure_reference(
                    &r, little(fadt + 112, 4) & (1u << 8) ? UINT32_MAX : 0xffffff, 3579545);
            if (hz) {
                *source = "ACPI PM timer calibration";
                return hz;
            }
            LOGW("ACPI PM timer unavailable/gated or invalid calibration");
        }
    }
    const uint8_t *hpet = amd64_acpi_table(bi, "HPET");
    if (hpet && little(hpet + 4, 4) >= 56 && !hpet[40] && !hpet[42]) {
        uint64_t address = little(hpet + 44, 8);
        if (!address || (address & 7))
            return 0;
        volatile uint64_t *regs = (volatile uint64_t *)amd64_map_mmio(address, 1024);
        if (!regs)
            return 0;
        uint64_t caps = regs[0], period = caps >> 32;
        if (!period || period > 100000000)
            return 0; /* HPET 1.0a §2.3.4 */
        uint64_t config = regs[2];
        /* Enable only the main counter; preserve legacy routing/timer state. */
        regs[2] = config | 1;
        r.counter = mmio_counter;
        r.context = (void *)(regs + 0x1e);
        hz = amd64_measure_reference(&r, UINT32_MAX, UINT64_C(1000000000000000) / period);
        regs[2] = config;
        if (hz) {
            *source = "HPET calibration";
            return hz;
        }
        LOGW("HPET unavailable/gated or invalid calibration");
    }
    return 0;
}
