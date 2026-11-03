/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Restart of the assigned functions, hosted, against a
 * fake PCIe topology whose resets behave as the specifications say:
 * Secondary Bus Reset (PCI-to-PCI Bridge 1.2 §3.2.5.17) puts everything
 * below the bridge into reset and brings it back in its power-on state,
 * bridges below with cleared bus numbers, so the devices behind them are
 * unreachable until those are restored; the link trains after release and
 * the functions answer CRS until ready (PCIe Base 5.0 §6.6.1). Function
 * Level Reset (§6.6.2) resets one function; a request to it within 100 ms,
 * or an FLR while Transactions Pending is set or in D3hot, is a violation.
 * A D3hot -> D0 transition with No_Soft_Reset clear resets the function,
 * and a request within 10 ms of it is a violation (PCI PM 1.2 §5.6.1).
 * Trst shorter than 1 ms, L1 PM Substates written while ASPM is on in Link
 * Control (PCIe §5.5.4) and a hot-plug event a reset raises with its
 * interrupt enabled are counted too. A Surprise Down error that is not
 * masked is reported (the AMD sync-flood hazard). Status registers are
 * RW1C.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "platform/platform.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/guest_mem.h"
#include "core/log_internal.h"

static unsigned failures;
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #e);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static bool clock_mocked;
static uint64_t clock_now;
#define MS (UINT64_C(1000000)) /* hal_counter units per ms: the hosted 1 GHz counter */
uint64_t __real_hal_counter(void);
uint64_t __wrap_hal_counter(void)
{
    return clock_mocked ? clock_now += 1000 : __real_hal_counter();
}
void __real_hal_idle_until(uint64_t deadline);
void __wrap_hal_idle_until(uint64_t deadline)
{
    if (clock_mocked) {
        if (deadline > clock_now)
            clock_now = deadline;
    } else
        __real_hal_idle_until(deadline);
}

struct fn {
    uint16_t bdf;
    uint8_t config[4096], defaults[4096];
    uint32_t masks[64];
    bool probing[64];
    uint64_t ready_at, link_up_at;
    uint64_t sbr_at, flr_at, d0_at, tp_until; /* 0: none */
    bool in_reset, link_dead, bar_readonly, acs_readonly;
    unsigned writes;
};
static struct fn fns[16];
static unsigned nfns, sync_flood, lost_writes, violations, hotplug_events;
static uint8_t scratch[1u << 20];
struct access {
    uint64_t time;
    uint16_t bdf, off;
    bool write;
    uint32_t value;
};
static struct access alog[8192];
static unsigned alog_count;

static struct fn *fake(uint16_t bdf)
{
    for (unsigned i = 0; i < nfns; ++i)
        if (fns[i].bdf == bdf)
            return &fns[i];
    return NULL;
}
static uint32_t get(const struct fn *f, unsigned off, unsigned size)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint32_t)f->config[off + i] << (i * 8);
    return value;
}
static void put(struct fn *f, unsigned off, uint32_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i)
        f->config[off + i] = (uint8_t)(value >> (i * 8));
}
static bool is_bridge(const struct fn *f)
{
    return (f->config[0x0e] & 0x7f) == 1;
}
/* Configuration requests reach bus 0, and a bus some reachable, ready
 * bridge forwards to with its current bus numbers. */
static bool reachable(uint16_t bdf)
{
    unsigned bus = bdf >> 8;
    if (!bus)
        return true;
    for (unsigned i = 0; i < nfns; ++i) {
        struct fn *b = &fns[i];
        if (is_bridge(b) && !b->in_reset && clock_now >= b->ready_at && b->config[0x19] &&
            b->config[0x19] <= bus && bus <= b->config[0x1a] && reachable(b->bdf))
            return true;
    }
    return false;
}
#define PCIE 0x40u
#define AER 0x100u
#define ACS 0x140u
#define PM 0x80u    /* PM capability of IDE and HDA, after the PCIe capability */
#define MSI 0x80u   /* MSI capability of NIC0 and RPB */
#define L1SS 0x110u /* NIC0, after its LTR capability */
static void timing(struct fn *f)
{
    if ((f->flr_at && clock_now < f->flr_at + 100 * MS) ||
        (f->d0_at && clock_now < f->d0_at + 10 * MS)) {
        fprintf(stderr, "%04x accessed %llu ns after FLR or D0\n", f->bdf,
                (unsigned long long)(clock_now - (f->flr_at > f->d0_at ? f->flr_at : f->d0_at)));
        ++violations;
    }
}
static uint32_t fake_read(void *opaque, uint16_t bdf, unsigned off, unsigned size)
{
    (void)opaque;
    struct fn *f = fake(bdf);
    uint32_t ones = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1, value;
    if (!f || !reachable(bdf) || off + size > 4096)
        value = ones;
    else if (f->in_reset || clock_now < f->ready_at)
        value = off == 0 && !f->in_reset ? (size == 2 ? 0x0001u : 0xffff0001u) : ones;
    else if (size == 4 && off < 256 && f->probing[off / 4])
        value = f->masks[off / 4];
    else {
        timing(f);
        value = get(f, off, size);
        if (is_bridge(f) && off == PCIE + 0x12 && size == 2)
            value = (value & ~0x2000u) | (clock_now >= f->link_up_at ? 0x2000u : 0);
        if (off == PCIE + 0x0a && size == 2 && clock_now < f->tp_until)
            value |= 0x20;
    }
    if (alog_count < sizeof(alog) / sizeof(alog[0]))
        alog[alog_count++] = (struct access){clock_now, bdf, (uint16_t)off, false, value};
    return value;
}
static void rw1c(struct fn *f, unsigned off, unsigned size, unsigned at, unsigned width,
                 uint32_t value)
{
    /* A write covering a RW1C register clears the bits written as 1. */
    if (off > at || off + size < at + width)
        return;
    uint32_t bits =
        (value >> ((at - off) * 8)) & (width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1);
    put(f, at, get(f, at, width) & ~bits, width);
}
static void fake_write(void *opaque, uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    (void)opaque;
    struct fn *f = fake(bdf);
    if (alog_count < sizeof(alog) / sizeof(alog[0]))
        alog[alog_count++] = (struct access){clock_now, bdf, (uint16_t)off, true, value};
    if (!f || !reachable(bdf) || f->in_reset || clock_now < f->ready_at) {
        ++lost_writes;
        return;
    }
    timing(f);
    ++f->writes;
    if (f->bar_readonly && off >= 0x10 && off < 0x28)
        return;
    if (f->acs_readonly && off == ACS + 6)
        return;
    bool bar = off >= 0x10 && off < (is_bridge(f) ? 0x18u : 0x28u);
    if (off < 256 && size == 4 && (value == UINT32_MAX || value == 0xfffffffeu) &&
        (bar || off == 0x30 || (off == 0x38 && is_bridge(f)))) {
        f->probing[off / 4] = true;
        return;
    }
    if (off < 256)
        f->probing[off / 4] = false;
    /* PM: D3hot -> D0; with No_Soft_Reset clear the function resets. */
    bool pm = (get(f, 6, 2) & 0x10) && get(f, PM, 1) == 1 && off == PM + 4 && size == 2;
    if (pm && (get(f, PM + 4, 2) & 3) == 3 && !(value & 3)) {
        bool keep = get(f, PM + 4, 2) & 8;
        if (!keep)
            memcpy(f->config, f->defaults, sizeof(f->config));
        put(f, PM + 4, (get(f, PM + 4, 2) & ~3u) | (value & 3), 2);
        f->d0_at = clock_now;
        return;
    }
    /* L1 PM Substates control with ASPM on in Link Control. */
    if (off >= L1SS + 8 && off < L1SS + 0x10 && get(f, L1SS, 2) == 0x1e &&
        (get(f, PCIE + 0x10, 2) & 3)) {
        fprintf(stderr, "%04x L1SS control written with ASPM on\n", f->bdf);
        ++violations;
    }
    if (is_bridge(f) && off <= 0x3e && off + size > 0x3e) {
        bool was = f->config[0x3e] & 0x40, now = (value >> ((0x3e - off) * 8)) & 0x40;
        if (now && !was)
            f->sbr_at = clock_now;
        if (was && !now && clock_now - f->sbr_at < 1 * MS) {
            fprintf(stderr, "%04x Trst %llu ns\n", f->bdf,
                    (unsigned long long)(clock_now - f->sbr_at));
            ++violations;
        }
        for (unsigned i = 0; was != now && i < nfns; ++i) {
            struct fn *g = &fns[i];
            unsigned bus = g->bdf >> 8;
            if (!bus || bus < f->config[0x19] || bus > f->config[0x1a])
                continue;
            if (now)
                g->in_reset = true;
            else if (g->in_reset) {
                g->in_reset = false;
                memcpy(g->config, g->defaults, sizeof(g->config));
                memset(g->probing, 0, sizeof(g->probing));
                g->ready_at = clock_now + 150 * MS;
            }
        }
        if (now && !was) {
            /* The link goes down: Surprise Down (AER bit 5, fatal by default). */
            if (!(get(f, AER + 8, 4) & 0x20) && (get(f, PCIE + 8, 2) & 0x4))
                ++sync_flood;
            put(f, AER + 4, get(f, AER + 4, 4) | 0x20, 4);
            put(f, PCIE + 0x0a, get(f, PCIE + 0x0a, 2) | 0x4, 2);
            f->link_up_at = UINT64_MAX;
            /* A hot-plug slot: presence and link-state change. */
            if (get(f, PCIE + 2, 2) & 0x100) {
                uint16_t control = (uint16_t)get(f, PCIE + 0x18, 2);
                if ((control & 0x20) && (control & 0x1008))
                    ++hotplug_events;
                put(f, PCIE + 0x1a, get(f, PCIE + 0x1a, 2) | 0x0108, 2);
            }
        }
        if (was && !now && !f->link_dead)
            f->link_up_at = clock_now + 30 * MS;
    }
    if (get(f, 6, 2) & 0x10 && get(f, 0x34, 1) == PCIE && off == PCIE + 8 && (value & 0x8000) &&
        (get(f, PCIE + 4, 4) & (1u << 28))) {
        if (clock_now < f->tp_until || (get(f, PM, 1) == 1 && (get(f, PM + 4, 2) & 3))) {
            fprintf(stderr, "%04x FLR with Transactions Pending or in D3hot\n", f->bdf);
            ++violations;
        }
        /* FLR leaves the link state, so Link Control, as it is (PCIe §6.6.2). */
        uint32_t link = get(f, PCIE + 0x10, 2);
        memcpy(f->config, f->defaults, sizeof(f->config));
        put(f, PCIE + 0x10, link, 2);
        f->ready_at = clock_now + 80 * MS;
        f->flr_at = clock_now;
        return;
    }
    /* Slot Control commands complete at once (Slot Status bit 4). */
    if (is_bridge(f) && (get(f, PCIE + 2, 2) & 0x100) && off == PCIE + 0x18 && size == 2)
        put(f, PCIE + 0x1a, get(f, PCIE + 0x1a, 2) | 0x10, 2);
    bool pcie_cap = (get(f, 6, 2) & 0x10) && get(f, 0x34, 1) == PCIE, aer = get(f, AER, 2) == 1;
    uint32_t old06 = get(f, 6, 2), oldsta = get(f, PCIE + 0x0a, 2), old1e = get(f, 0x1e, 2),
             olduesta = get(f, AER + 4, 4);
    uint32_t oldslot = get(f, PCIE + 0x1a, 2);
    put(f, off, value, size);
    put(f, 6, old06, 2);
    rw1c(f, off, size, 6, 2, value);
    if (pcie_cap) {
        put(f, PCIE + 0x0a, oldsta, 2);
        rw1c(f, off, size, PCIE + 0x0a, 2, value);
    }
    if (pcie_cap && is_bridge(f)) {
        put(f, PCIE + 0x1a, oldslot, 2);
        rw1c(f, off, size, PCIE + 0x1a, 2, value);
    }
    if (is_bridge(f)) {
        put(f, 0x1e, old1e, 2);
        rw1c(f, off, size, 0x1e, 2, value);
    }
    if (aer) {
        put(f, AER + 4, olduesta, 4);
        rw1c(f, off, size, AER + 4, 4, value);
    }
}
static uint8_t *fake_map(void *opaque, uint64_t base, uint64_t size)
{
    (void)opaque;
    (void)base;
    (void)size;
    return scratch;
}

#define HOST 0x0000u
#define RPA 0x0008u /* 00:01.0 -> bus 1 */
#define NIC0 0x0100u
#define NIC1 0x0101u
#define RPB 0x0010u  /* 00:02.0 -> buses 2-3 */
#define AHCI 0x0200u /* 02:00.0 */
#define SWD 0x0208u  /* 02:01.0, a bridge below RPB -> bus 3 */
#define NIC2 0x0300u
#define RPC 0x0018u /* 00:03.0 -> bus 4, the VMM's controller */
#define MEDIA 0x0400u
#define RPD 0x0020u /* 00:04.0 -> bus 5, a display and its audio */
#define GPU 0x0500u
#define HDA 0x0501u
#define XHCI 0x0028u /* 00:05.0 integrated, FLR */
#define IDE 0x0030u  /* 00:06.0 integrated, no reset method */
#define LPC 0x00f8u

static void bridge_buses(uint16_t bdf, unsigned primary, unsigned secondary, unsigned subordinate)
{
    put(fake(bdf), 0x18, primary | secondary << 8 | subordinate << 16, 4);
}
static void pcie(uint16_t bdf, unsigned type, bool flr)
{
    struct fn *f = fake(bdf);
    put(f, 6, get(f, 6, 2) | 0x10, 2);
    put(f, 0x34, PCIE, 1);
    put(f, PCIE, 0x00020010u | type << 20, 4); /* PCIe v2, next 0 */
    put(f, PCIE + 4, flr ? 1u << 28 : 0, 4);
    put(f, PCIE + 8, 0x2817, 2); /* error reporting on, MPS/MRRS */
    put(f, PCIE + 0x10, 0x0040, 2);
    if (type == 4 || type == 6) {
        put(f, PCIE + 0x0c, (1u << 20) | (3u << 4) | 3u, 4); /* DLLLARC, x3, 8 GT/s */
        f->link_up_at = 0;
    }
    put(f, PCIE + 0x28, 0x0400, 2);
}
static void build(bool broken_ids)
{
    static const struct {
        uint16_t bdf;
        uint32_t id, cls;
        uint8_t header;
    } list[] = {
        {HOST, 0x01048086, 0x060000, 0},    {RPA, 0x1c108086, 0x060400, 1},
        {NIC0, 0x10d38086, 0x020000, 0x80}, {NIC1, 0x10d38086, 0x020000, 0},
        {RPB, 0x1c128086, 0x060400, 1},     {AHCI, 0x29228086, 0x010601, 0},
        {SWD, 0x87181b21, 0x060400, 1},     {NIC2, 0x816810ec, 0x020000, 0},
        {RPC, 0x1c148086, 0x060400, 1},     {MEDIA, 0x00151b73, 0x0c0330, 0},
        {RPD, 0x1c168086, 0x060400, 1},     {GPU, 0x2f0410de, 0x030000, 0x80},
        {HDA, 0x22e810de, 0x040300, 0},     {XHCI, 0x1e318086, 0x0c0330, 0},
        {IDE, 0x70108086, 0x01018a, 0},     {LPC, 0x1e558086, 0x060100, 0},
    };
    memset(fns, 0, sizeof(fns));
    nfns = sizeof(list) / sizeof(list[0]);
    for (unsigned i = 0; i < nfns; ++i) {
        struct fn *f = &fns[i];
        f->bdf = list[i].bdf;
        put(f, 0, list[i].id, 4);
        put(f, 8, list[i].cls << 8, 4);
        put(f, 0x0e, list[i].header, 1);
        put(f, 4, 0x0006, 2);
        put(f, 0x0c, 0x10, 1);
    }
    bridge_buses(RPA, 0, 1, 1);
    bridge_buses(RPB, 0, 2, 3);
    bridge_buses(SWD, 2, 3, 3);
    bridge_buses(RPC, 0, 4, 4);
    bridge_buses(RPD, 0, 5, 5);
    put(fake(RPA), 0x20, 0xc0f0c000, 4);
    put(fake(RPB), 0x20, 0xc1f0c100, 4);
    put(fake(SWD), 0x20, 0xc1f0c180, 4);
    put(fake(RPC), 0x20, 0xc2f0c200, 4);
    put(fake(RPD), 0x20, 0xc3f0c300, 4);
    for (uint16_t b = RPA; b <= RPD; b += 8) {
        pcie(b, 4, false);
        put(fake(b), 4, 0x0007, 2);
        put(fake(b), 0x3e, 0x0002, 2);
    }
    pcie(SWD, 6, false);
    put(fake(SWD), 4, 0x0007, 2);
    /* Root ports B: AER, Surprise Down unmasked as firmware leaves it. */
    for (uint16_t b = RPA; b <= RPD; b += 8)
        put(fake(b), AER, 0x00010001, 4);
    struct fn *nic = fake(NIC0);
    put(nic, 0x10, 0xc0000000, 4);
    nic->masks[0x10 / 4] = 0xfffe0000;
    pcie(NIC0, 0, false);
    put(nic, PCIE, 0x00028010, 4); /* next: MSI at 80h */
    put(nic, MSI, 0x00800005, 4);  /* MSI 64-bit, last */
    put(nic, 0x100, 0x00010018, 4);
    put(nic, 0x104, 0x10031003, 4); /* LTR as firmware left it */
    put(fake(NIC1), 0x10, 0xc0020000, 4);
    fake(NIC1)->masks[0x10 / 4] = 0xfffe0000;
    struct fn *ahci = fake(AHCI);
    put(ahci, 0x24, 0xc1000000, 4);
    ahci->masks[0x24 / 4] = 0xfffff800;
    put(ahci, 0x92, 0x000f, 2);     /* PCS: ports enabled by the host firmware */
    put(ahci, 0xa0, 0x00000010, 4); /* SINDX: an index register */
    put(fake(NIC2), 0x10, 0xc1800000, 4);
    fake(NIC2)->masks[0x10 / 4] = 0xffff0000;
    put(fake(MEDIA), 0x10, 0xc2000000, 4);
    fake(MEDIA)->masks[0x10 / 4] = 0xffff0000;
    put(fake(GPU), 0x10, 0xc3000000, 4);
    fake(GPU)->masks[0x10 / 4] = 0xff000000;
    put(fake(GPU), 4, 0x0002, 2); /* GOP: memory decode, no bus master */
    put(fake(HDA), 0x10, 0xc3800000, 4);
    fake(HDA)->masks[0x10 / 4] = 0xffffc000;
    pcie(HDA, 0, true);
    /* PM capabilities: HDA after its PCIe capability (No_Soft_Reset set),
     * the method-less IDE function alone (No_Soft_Reset clear). */
    put(fake(HDA), PCIE + 1, PM, 1);
    put(fake(HDA), PM, 0x00030001, 4);
    put(fake(HDA), PM + 4, 0x0008, 2);
    put(fake(IDE), 6, get(fake(IDE), 6, 2) | 0x10, 2);
    put(fake(IDE), 0x34, PM, 1);
    put(fake(IDE), PM, 0x00030001, 4);
    /* NIC0: L1 PM Substates after LTR, ASPM L1 on, as the host firmware left it. */
    put(nic, 0x100, 0x11010018, 4);
    put(nic, L1SS, 0x0001001e, 4);
    put(nic, L1SS + 4, 0x1f, 4);
    put(nic, L1SS + 8, 0x0000000f, 4);
    put(nic, L1SS + 0x0c, 0x28, 4);
    put(nic, PCIE + 0x10, 0x0042, 2);
    /* RPB: a hot-plug slot with its events enabled, and an MSI capability. */
    struct fn *rpb = fake(RPB);
    put(rpb, PCIE + 2, get(rpb, PCIE + 2, 2) | 0x100, 2);
    put(rpb, PCIE + 0x18, 0x1028, 2);
    put(rpb, PCIE + 1, MSI, 1);
    put(rpb, MSI, 0x00800005, 4);
    put(fake(XHCI), 0x10, 0xc4000000, 4);
    fake(XHCI)->masks[0x10 / 4] = 0xffff0000;
    pcie(XHCI, 9, true);
    struct fn *xhci = fake(XHCI);
    put(xhci, 0x100, 0x11010018, 4);
    put(xhci, 0x104, 0x10031003, 4);
    put(xhci, L1SS, 0x0001001e, 4);
    put(xhci, L1SS + 4, 0x1f, 4);
    put(xhci, L1SS + 8, 0x0000000f, 4);
    put(xhci, L1SS + 0x0c, 0x28, 4);
    put(xhci, PCIE + 0x10, 0x0042, 2);
    /* Power-on state: BARs, command, bus numbers, windows and the
     * device-specific bytes clear; capability structures as before. */
    for (unsigned i = 0; i < nfns; ++i) {
        struct fn *f = &fns[i];
        memcpy(f->defaults, f->config, sizeof(f->config));
        for (unsigned off = 0x10; off <= 0x24; off += 4) {
            uint32_t v = get(f, off, 4);
            v &= v & 1 ? 3u : 0xfu;
            for (unsigned k = 0; k < 4; ++k)
                f->defaults[off + k] = (uint8_t)(v >> (k * 8));
        }
        memset(f->defaults + 4, 0, 2);
        memset(f->defaults + 0x30, 0, 4);
        f->defaults[0x0c] = 0;
        if (is_bridge(f))
            memset(f->defaults + 0x18, 0, 0x1c);
        memset(f->defaults + 0x92, 0, 2);
        memset(f->defaults + 0xa0, 0, 4);
        if (get(f, 6, 2) & 0x10) {
            f->defaults[PCIE + 8] = 0x10;
            f->defaults[PCIE + 9] = 0x28;
        }
        memset(f->defaults + 0x104, 0, 4);
        memset(f->defaults + L1SS + 8, 0, 8);
        if (get(f, 6, 2) & 0x10)
            f->defaults[PCIE + 0x10] = 0x40;
    }
    if (broken_ids)
        fake(AHCI)->defaults[2] ^= 1;
}

static const struct pci_host_ops ops = {fake_read, fake_write, fake_map};
static struct platform_state state;
static bool setup(bool broken_ids)
{
    build(broken_ids);
    platform_pci_set_host(&ops, NULL);
    if (!platform_pci_inventory("vmm_media=04:00.0"))
        return false;
    return platform_devices_init(&state);
}
static void teardown(void)
{
    platform_devices_cleanup(&state);
    platform_pci_set_host(NULL, NULL);
}
/* Positions in the access log (order) and their times. */
static unsigned first_access(uint16_t bdf, unsigned after)
{
    for (unsigned i = after + 1; i < alog_count; ++i)
        if (alog[i].bdf == bdf)
            return i;
    return UINT32_MAX;
}
static unsigned first_write(uint16_t bdf, unsigned off, unsigned after)
{
    for (unsigned i = after + 1; i < alog_count; ++i)
        if (alog[i].bdf == bdf && alog[i].write && alog[i].off == off)
            return i;
    return UINT32_MAX;
}
static unsigned writes_to(uint16_t bdf, unsigned off_min, unsigned off_max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < alog_count; ++i)
        if (alog[i].bdf == bdf && alog[i].write && alog[i].off >= off_min && alog[i].off <= off_max)
            ++n;
    return n;
}
static bool restored(uint16_t bdf, const uint8_t before[256])
{
    struct fn *f = fake(bdf);
    for (unsigned off = 0; off < 0x40; ++off)
        if (off != 4 && off != 5 && off != 6 && off != 7 && f->config[off] != before[off]) {
            fprintf(stderr, "%04x offset %02x: %02x, saved %02x\n", bdf, off, f->config[off],
                    before[off]);
            return false;
        }
    return true;
}

static void test_reset(void)
{
    clock_mocked = true;
    clock_now = 1000 * MS;
    CHECK(setup(false));
    /* The guest drove the NIC: Bus Master Enable and MSI on. */
    platform_pci_write((uint64_t)NIC0 << 12 | 4, 0x0007, 2);
    put(fake(NIC0), MSI + 2, 0x0081, 2);
    static uint8_t before[16][256];
    for (unsigned i = 0; i < nfns; ++i)
        memcpy(before[i], fns[i].config, 256);
    unsigned untouched[] = {fake(HOST)->writes, fake(RPC)->writes, fake(MEDIA)->writes,
                            fake(LPC)->writes, fake(GPU)->writes};
    /* The guest left the method-less IDE function's latency timer changed
     * and both it and HDA in D3hot; XHCI still has transactions pending
     * for 30 ms; the guest enabled RPB's MSI. */
    put(fake(IDE), 0x0d, 0x40, 1);
    put(fake(IDE), PM + 4, 0x0003, 2);
    put(fake(HDA), PM + 4, 0x000b, 2);
    fake(XHCI)->tp_until = clock_now + 30 * MS;
    put(fake(RPB), MSI + 2, 0x0081, 2);
    violations = hotplug_events = 0;
    alog_count = 0;
    platform_pci_quiesce();
    unsigned start = alog_count - 1;
    CHECK(!(get(fake(NIC0), 4, 2) & 4) && (get(fake(NIC0), 4, 2) & 0x400) &&
          !(get(fake(NIC0), MSI + 2, 2) & 1));
    CHECK(!(get(fake(RPB), MSI + 2, 2) & 1) && (get(fake(RPB), 4, 2) & 4));
    CHECK(platform_pci_reset_functions());
    CHECK(!sync_flood && !violations && !hotplug_events);
    /* D0 again; RPB's slot events back on, their status clear; NIC0's L1
     * PM Substates and ASPM as saved. */
    CHECK(!(get(fake(IDE), PM + 4, 2) & 3) && !(get(fake(HDA), PM + 4, 2) & 3));
    CHECK(get(fake(RPB), PCIE + 0x18, 2) == 0x1028 && !(get(fake(RPB), PCIE + 0x1a, 2) & 0x0118));
    CHECK(get(fake(NIC0), L1SS + 8, 4) == 0x0f && get(fake(NIC0), L1SS + 0x0c, 4) == 0x28 &&
          get(fake(NIC0), PCIE + 0x10, 2) == 0x0042);
    CHECK(get(fake(XHCI), L1SS + 8, 4) == 0x0f && get(fake(XHCI), PCIE + 0x10, 2) == 0x0042 &&
          get(fake(XHCI), 0x104, 4) == 0x10031003);
    /* SBR on RPA (both NIC functions) and RPB (AHCI, the bridge SWD and NIC2); not on RPC
     * (the VMM's controller) nor RPD (a display below); FLR on HDA and
     * XHCI; IDE has no method: quiesce and restore only. */
    CHECK(writes_to(RPA, 0x3e, 0x3e) >= 2 && writes_to(RPB, 0x3e, 0x3e) >= 2 &&
          !writes_to(RPD, 0x3e, 0x3e));
    CHECK(fake(HOST)->writes == untouched[0] && fake(RPC)->writes == untouched[1] &&
          fake(MEDIA)->writes == untouched[2] && fake(LPC)->writes == untouched[3] &&
          fake(GPU)->writes == untouched[4]);
    CHECK(writes_to(HDA, PCIE + 8, PCIE + 8) && writes_to(XHCI, PCIE + 8, PCIE + 8));
    CHECK(first_write(IDE, PM + 4, start) < first_write(IDE, 4, start) &&
          first_write(IDE, 4, start) < alog_count);
    /* Errors masked before the assert and restored, status cleared after. */
    unsigned assert_b = first_write(RPB, 0x3e, start);
    CHECK(first_write(RPB, AER + 8, start) < assert_b &&
          first_write(RPB, PCIE + 8, start) < assert_b);
    CHECK(get(fake(RPB), AER + 8, 4) == 0 && get(fake(RPB), PCIE + 8, 2) == 0x2817 &&
          get(fake(RPB), 0x3e, 2) == 0x0002);
    CHECK(!get(fake(RPB), AER + 4, 4) && !(get(fake(RPB), PCIE + 0x0a, 2) & 0xf));
    /* SWD's bus numbers are back before NIC2 behind it is accessed. */
    CHECK(first_write(SWD, 0x18, assert_b) < first_access(NIC2, assert_b));
    CHECK(restored(SWD, before[6]) && get(fake(SWD), 4, 2) == 0x0007);
    /* 100 ms after the link is up before a request to the functions below. */
    unsigned nic0 = first_access(NIC0, first_write(RPA, 0x3e, start));
    CHECK(nic0 < alog_count && alog[nic0].time >= fake(RPA)->link_up_at + 100 * MS);
    /* Restore: header, BARs, PCIe and LTR as saved; MSI disabled; command last
     * with Bus Master Enable clear; PCS restored, SINDX never written. */
    for (unsigned i = 0; i < nfns; ++i) {
        uint16_t bdf = fns[i].bdf;
        if (bdf == NIC0 || bdf == NIC1 || bdf == AHCI || bdf == NIC2 || bdf == HDA || bdf == XHCI ||
            bdf == IDE) {
            CHECK(restored(bdf, before[i]));
            CHECK((get(&fns[i], 4, 2) & 0x7) == 0x2);
        }
    }
    CHECK(get(fake(NIC0), PCIE + 8, 2) == 0x2817 && get(fake(NIC0), 0x104, 4) == 0x10031003);
    CHECK(get(fake(NIC0), MSI, 4) == 0x00800005 && !(get(fake(NIC0), MSI + 2, 2) & 1));
    CHECK(get(fake(AHCI), 0x92, 2) == 0x000f && !writes_to(AHCI, 0xa0, 0xa3) &&
          get(fake(AHCI), 0xa0, 4) == 0);
    uint64_t last_command = 0;
    for (unsigned i = 0; i < alog_count; ++i)
        if (alog[i].bdf == AHCI && alog[i].write && i > assert_b) {
            CHECK(!last_command);
            if (alog[i].off == 4)
                last_command = alog[i].time;
        }
    CHECK(last_command);
    CHECK(!lost_writes);
    /* The next boot's mediation state: command from the first boot without BME. */
    platform_devices_restart(&state);
    CHECK((platform_pci_read((uint64_t)NIC0 << 12 | 4, 2) & 7) == 2);
    teardown();
    clock_mocked = false;
}

/* A display the guest gave Bus Master Enable, or one that kept the
 * host's, is quiesced but not reset. */
static void display_case(bool inherited)
{
    {
        clock_mocked = true;
        clock_now = 1000 * MS;
        sync_flood = 0;
        if (inherited) {
            build(false);
            put(fake(GPU), 4, 0x0006, 2);
            platform_pci_set_host(&ops, NULL);
            CHECK(platform_pci_inventory("vmm_media=04:00.0") && platform_devices_init(&state));
        } else {
            CHECK(setup(false));
            platform_pci_write((uint64_t)GPU << 12 | 4, 0x0007, 2);
        }
        alog_count = 0;
        platform_pci_quiesce();
        CHECK(!(get(fake(GPU), 4, 2) & 4));
        unsigned gpu = fake(GPU)->writes;
        CHECK(platform_pci_reset_functions());
        CHECK(fake(GPU)->writes == gpu && !writes_to(RPD, 0x3e, 0x3e));
        platform_devices_restart(&state);
        CHECK(!(platform_pci_read((uint64_t)GPU << 12 | 4, 2) & 4) && !(get(fake(GPU), 4, 2) & 4));
        teardown();
        clock_mocked = false;
    }
}
static void test_display_guest(void)
{
    display_case(false);
}
static void test_display_inherited(void)
{
    display_case(true);
}

/* A BAR that does not take its saved value after the reset ends the run. */
static void test_bars(void)
{
    clock_mocked = true;
    clock_now = 1000 * MS;
    sync_flood = 0;
    CHECK(setup(false));
    fake(XHCI)->bar_readonly = true;
    platform_pci_quiesce();
    CHECK(!platform_pci_reset_functions());
    teardown();
    clock_mocked = false;
}

/* A link that does not come up after the reset ends the run; the bridge's
 * error reporting and control are restored. */
static void test_link(void)
{
    clock_mocked = true;
    clock_now = 1000 * MS;
    sync_flood = 0;
    CHECK(setup(false));
    fake(RPA)->link_dead = true;
    uint32_t mask = get(fake(RPA), AER + 8, 4), devctl = get(fake(RPA), PCIE + 8, 2);
    platform_pci_quiesce();
    CHECK(!platform_pci_reset_functions());
    CHECK(get(fake(RPA), AER + 8, 4) == mask && get(fake(RPA), PCIE + 8, 2) == devctl &&
          !(get(fake(RPA), 0x3e, 2) & 0x40) && !sync_flood);
    teardown();
    clock_mocked = false;
}

/* A function that comes back with other identifiers ends the run. */
static void test_failure(void)
{
    clock_mocked = true;
    clock_now = 1000 * MS;
    sync_flood = 0;
    CHECK(setup(true));
    platform_pci_quiesce();
    CHECK(!platform_pci_reset_functions());
    teardown();
    clock_mocked = false;
}

/* A reset must preserve ACS isolation and reject a lost control write. */
static void acs_case(uint16_t bdf, bool readonly)
{
    clock_mocked = true;
    clock_now = 1000 * MS;
    build(false);
    struct fn *f = fake(bdf);
    put(f, 0x100, ((ACS | 3u) << 20) | 0x00010018, 4);
    put(f, ACS, 0x0001000d, 4);
    put(f, ACS + 4, 0x001d, 2);
    put(f, ACS + 6, 0x001d, 2);
    memcpy(f->defaults + 0x100, f->config + 0x100, sizeof(f->config) - 0x100);
    memset(f->defaults + ACS + 6, 0, 2);
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=04:00.0") && platform_devices_init(&state));
    f->acs_readonly = readonly;
    platform_pci_quiesce();
    alog_count = 0;
    CHECK(platform_pci_reset_functions() == !readonly);
    CHECK(get(f, ACS + 6, 2) == (readonly ? 0u : 0x001du));
    if (readonly)
        CHECK(!(get(f, 4, 2) & 4));
    teardown();
    clock_mocked = false;
}
static void test_acs_sbr(void)
{
    acs_case(NIC0, false);
}
static void test_acs_flr(void)
{
    acs_case(XHCI, false);
}
static void test_acs_sbr_failure(void)
{
    acs_case(NIC0, true);
}
static void test_acs_flr_failure(void)
{
    acs_case(XHCI, true);
}
static void test_acs_bridge_failure(void)
{
    acs_case(SWD, true);
}
static void ari_case(bool readonly)
{
    clock_mocked = true;
    clock_now = 1000 * MS;
    build(false);
    put(fake(RPA), PCIE + 0x24, 0x20, 4);
    put(fake(RPA), PCIE + 0x28, 0x20, 2);
    fake(NIC1)->bdf = 0x189;
    struct fn *f = fake(NIC0);
    put(f, 0x0e, 0, 1);
    put(f, 0x100, (ACS << 20) | 0x00010018, 4);
    put(f, ACS, 0x0001000e, 4);
    put(f, ACS + 4, (137u << 8) | 2, 2);
    put(f, ACS + 6, 2, 2); /* ACS Function Groups Enable */
    memcpy(f->defaults + ACS, f->config + ACS, 8);
    memset(f->defaults + ACS + 6, 0, 2);
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=04:00.0") && platform_devices_init(&state));
    f->acs_readonly = readonly; /* same control offset, now an ARI capability */
    platform_pci_quiesce();
    alog_count = 0;
    CHECK(platform_pci_reset_functions() == !readonly);
    CHECK(get(f, ACS + 6, 2) == (readonly ? 0u : 2u));
    CHECK(get(fake(RPA), PCIE + 0x28, 2) == 0x20);
    if (readonly)
        CHECK(!(get(f, 4, 2) & 4));
    else {
        CHECK(get(fake(0x189), 0, 4) == 0x10d38086);
        CHECK(get(fake(0x189), 0x10, 4) == 0xc0020000);
    }
    teardown();
    clock_mocked = false;
}
static void test_ari(void)
{
    ari_case(false);
}
static void test_ari_failure(void)
{
    ari_case(true);
}

static void test_extended_bounds(void)
{
    clock_mocked = true;
    clock_now = 1000 * MS;
    build(false);
    /* A cycle and a truncated capability must stay inside ECAM. */
    put(fake(NIC0), 0x100, (0x100u << 20) | 0x00010018, 4);
    put(fake(XHCI), 0x100, (0xfffu << 20) | 0x00010018, 4);
    put(fake(XHCI), 0xffc, 0x0001000d, 4);
    /* A conventional PCI function cannot advertise PCIe ACS. */
    put(fake(IDE), 0x100, 0x0001000d, 4);
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=04:00.0") && platform_devices_init(&state));
    alog_count = 0;
    platform_pci_restart_snapshot();
    for (unsigned i = 0; i < alog_count; ++i) {
        CHECK(alog[i].off < 0x1000);
        CHECK(alog[i].bdf != IDE || alog[i].off < 0x100);
    }
    teardown();
    clock_mocked = false;
}

/* Each case registers the platform once: its own process. */
static void run(void (*test)(void))
{
    pid_t pid = fork();
    if (!pid) {
        failures = 0;
        test();
        if (failures)
            fprintf(stderr, "test-restart: case failed\n");
        _exit(failures ? 1 : 0);
    }
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
        ++failures;
}
int main(void)
{
    run(test_reset);
    run(test_display_guest);
    run(test_display_inherited);
    run(test_failure);
    run(test_bars);
    run(test_link);
    run(test_acs_sbr);
    run(test_acs_flr);
    run(test_acs_sbr_failure);
    run(test_acs_flr_failure);
    run(test_acs_bridge_failure);
    run(test_ari);
    run(test_ari_failure);
    run(test_extended_bounds);
    if (failures) {
        fprintf(stderr, "test-restart: %u failure(s)\n", failures);
        return 1;
    }
    printf("test-restart: function quiesce, SBR/FLR, restore order and failure passed\n");
    return 0;
}
