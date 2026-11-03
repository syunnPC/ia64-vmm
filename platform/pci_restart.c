/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Restart of the assigned functions. Management CPU,
 * every executor stopped, through the host's own configuration space.
 * Devices must stop DMA and interrupts before guest RAM is reused.
 *
 * Reset methods: Secondary Bus Reset (PCI-to-PCI Bridge 1.2 §3.2.5.17) on
 * a bridge whose whole subtree is guest-owned and resettable, otherwise
 * Function Level Reset (PCIe Base 5.0 §6.6.2) when advertised, otherwise
 * none. Timing follows PCIe Base 5.0 §6.6.1 as platform/pci_reset.c does.
 * Restore the PCI header and capabilities saved before the first guest boot.
 */
#include "platform/platform.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define HOLD_MS 2u     /* Trst 1 ms + margin */
#define SETTLE_MS 100u /* §6.6.1 */
#define D3HOT_MS 10u   /* D3hot -> D0 recovery, PCI PM 1.2 §5.6.1 */
#define SLOT_MS 1000u  /* hot-plug command completion, PCIe §6.7.3.2 */
#define LINK_MS 1000u
#define READY_MS 10000u /* as platform/pci_reset.c */
#define REBAR_MAX 6u

struct snapshot {
    bool valid;
    uint32_t config[64];
    uint16_t pcie, pm, msi, msix;             /* capability offsets, 0: absent */
    uint16_t rebar, aer, ltr, l1ss, acs, ari; /* extended capability offsets */
    unsigned rebar_count;
    uint32_t rebar_ctrl[REBAR_MAX], aer_regs[3], ltr_value, l1ss_ctrl[2];
    uint16_t acs_ctrl, ari_ctrl;
};
static struct snapshot snaps[PCI_MAX_DEVICES];

/* Device-specific bytes the host firmware programs and the guest firmware
 * does not, restored after a reset. Intel ICH/PCH SATA in AHCI mode: PCS,
 * the port enables (ICH9/ICH10 and 6/7 Series PCH datasheets, SATA 92h). */
static const struct {
    uint16_t vendor;
    uint32_t class_code;
    uint8_t off, size;
} device_bytes[] = {
    {0x8086, 0x010601, 0x92, 2},
};

static uint32_t rd(uint16_t bdf, unsigned off, unsigned size)
{
    return platform_pci_host_read(bdf, off, size);
}
static void wr(uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    platform_pci_host_write(bdf, off, value, size);
}
static uint32_t field(const struct snapshot *s, unsigned off, unsigned size)
{
    uint64_t next = off % 4 + size > 4 && off / 4 + 1 < 64 ? s->config[off / 4 + 1] : 0;
    uint64_t value = (s->config[off / 4] | next << 32) >> ((off % 4) * 8);
    return (uint32_t)(size == 4 ? value : value & ((1u << (size * 8)) - 1));
}
static uint16_t capability(const struct snapshot *s, unsigned id)
{
    if (!(field(s, 6, 2) & 0x10))
        return 0;
    bool seen[64] = {false};
    for (unsigned p = field(s, 0x34, 1) & ~3u; p >= 0x40 && p <= 0xfc && !seen[p / 4];
         p = field(s, p + 1, 1) & ~3u) {
        seen[p / 4] = true;
        if (field(s, p, 1) == id)
            return (uint16_t)p;
    }
    return 0;
}
static uint16_t extended(uint16_t bdf, const struct snapshot *s, unsigned id, unsigned bytes)
{
    if (!s->pcie)
        return 0;
    bool seen[1024] = {false};
    for (unsigned at = 0x100; at >= 0x100 && at <= 0xffc && !seen[at / 4];) {
        seen[at / 4] = true;
        uint32_t header = rd(bdf, at, 4);
        if (!header || header == UINT32_MAX)
            break;
        if ((header & 0xffff) == id)
            return at + bytes <= 0x1000 ? (uint16_t)at : 0;
        at = (header >> 20) & ~3u;
    }
    return 0;
}

static const char *name(uint16_t bdf)
{
    static char text[4][12];
    static unsigned next;
    char *t = text[next++ % 4];
    vmm_snprintf(t, 12, "%02x:%02x.%u", bdf >> 8, (bdf >> 3) & 31, bdf & 7);
    return t;
}
static uint64_t ms_since(uint64_t start)
{
    uint64_t hz = hal_counter_hz();
    return hz >= 1000 ? (hal_counter() - start) / (hz / 1000) : 0;
}
static void sleep_ms(uint64_t ms)
{
    uint64_t deadline = hal_counter() + ms * (hal_counter_hz() / 1000);
    while (hal_counter() < deadline)
        hal_idle_until(deadline);
}

/* A function a driver left in D3hot goes to D0 before it is reset or
 * restored, then rests D3HOT_MS. With No_Soft_Reset clear (PMCSR bit 3)
 * the transition resets its configuration, which the restore writes back
 * afterwards (PCI PM 1.2 §5.4.1, §5.6.1). */
static void to_d0(uint16_t bdf, const struct snapshot *s)
{
    if (!s->pm)
        return;
    uint16_t pmcsr = (uint16_t)rd(bdf, s->pm + 4, 2);
    if (!(pmcsr & 3) || pmcsr == 0xffff)
        return;
    wr(bdf, s->pm + 4, pmcsr & ~0x8003u, 2);
    sleep_ms(D3HOT_MS);
    LOGI("restart: %s D%u -> D0 (No_Soft_Reset=%u)", name(bdf), pmcsr & 3, (pmcsr >> 3) & 1);
}

static bool display(const struct pci_device *d)
{
    return d->class_code >> 16 == 0x03;
}
/* Bridges, the LPC/ISA bridge included, serve other devices. */
static bool bridge(const struct pci_device *d)
{
    return d->header == 1 || d->class_code >> 16 == 0x06;
}

/* A display no OS driver touched. Bus Master Enable inherited from
 * the host counts as touched: a driver that finds it set never writes the
 * command register, yet its device can do DMA. */
static bool untouched(const struct pci_device *d)
{
    return display(d) && !d->guest_bus_master && !(rd(d->bdf, 4, 2) & 4);
}
/* The functions the last quiesce took, by inventory index. */
static bool quiesced_at[PCI_MAX_DEVICES];
static int index_of(uint16_t bdf)
{
    for (unsigned i = 0; i < platform_pci_count(); ++i)
        if (platform_pci_device(i)->bdf == bdf)
            return (int)i;
    return -1;
}
static bool in_scope(const struct pci_device *d)
{
    int i = index_of(d->bdf);
    return i >= 0 && (unsigned)i < PCI_MAX_DEVICES && quiesced_at[i];
}
bool platform_pci_restart_untouched(const struct pci_device *d)
{
    return display(d) && !in_scope(d);
}
/* Displays are quiesced but never reset. */
static bool resettable(const struct pci_device *d)
{
    return in_scope(d) && !display(d);
}

void platform_pci_restart_snapshot(void)
{
    for (unsigned i = 0; i < platform_pci_count() && i < PCI_MAX_DEVICES; ++i) {
        const struct pci_device *d = platform_pci_device(i);
        struct snapshot *s = &snaps[i];
        memset(s, 0, sizeof(*s));
        if (d->owner != PCI_OWNER_GUEST)
            continue;
        for (unsigned off = 0; off < 256; off += 4)
            s->config[off / 4] = rd(d->bdf, off, 4);
        s->pcie = capability(s, 0x10);
        s->pm = capability(s, 0x01);
        s->msi = capability(s, 0x05);
        s->msix = capability(s, 0x11);
        if ((s->rebar = extended(d->bdf, s, 0x15, 12))) {
            s->rebar_count = (rd(d->bdf, s->rebar + 8, 4) >> 5) & 7;
            if (s->rebar_count > REBAR_MAX)
                s->rebar_count = REBAR_MAX;
            if (s->rebar_count > (0xffcu - s->rebar) / 8)
                s->rebar_count = (0xffcu - s->rebar) / 8;
            for (unsigned k = 0; k < s->rebar_count; ++k)
                s->rebar_ctrl[k] = rd(d->bdf, s->rebar + 8 + k * 8, 4);
        }
        if ((s->aer = extended(d->bdf, s, 0x01, 24))) {
            s->aer_regs[0] = rd(d->bdf, s->aer + 0x08, 4); /* Uncorrectable Error Mask */
            s->aer_regs[1] = rd(d->bdf, s->aer + 0x0c, 4); /* Uncorrectable Error Severity */
            s->aer_regs[2] = rd(d->bdf, s->aer + 0x14, 4); /* Correctable Error Mask */
        }
        if ((s->ltr = extended(d->bdf, s, 0x18, 8)))
            s->ltr_value = rd(d->bdf, s->ltr + 4, 4);
        if ((s->l1ss = extended(d->bdf, s, 0x1e, 16))) {
            s->l1ss_ctrl[0] = rd(d->bdf, s->l1ss + 8, 4);
            s->l1ss_ctrl[1] = rd(d->bdf, s->l1ss + 0x0c, 4);
        }
        if ((s->acs = extended(d->bdf, s, 0x0d, 8)))
            s->acs_ctrl = (uint16_t)rd(d->bdf, s->acs + 6, 2);
        if ((s->ari = extended(d->bdf, s, 0x0e, 8)))
            s->ari_ctrl = (uint16_t)rd(d->bdf, s->ari + 6, 2);
        s->valid = true;
    }
}

void platform_pci_quiesce(void)
{
    unsigned quiesced = 0;
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (i >= PCI_MAX_DEVICES)
            break;
        quiesced_at[i] =
            d->owner == PCI_OWNER_GUEST && d->header == 0 && !bridge(d) && !untouched(d);
        /* A guest-owned bridge keeps forwarding, but its own MSI/MSI-X
         * stops: platform_msi_restart releases its vector for reuse. */
        bool bridge_msi =
            d->owner == PCI_OWNER_GUEST && bridge(d) && (d->msi_offset || d->msix_offset);
        if (!quiesced_at[i] && !bridge_msi)
            continue;
        if (quiesced_at[i]) {
            uint16_t command = (uint16_t)rd(d->bdf, 4, 2);
            wr(d->bdf, 4, (command & ~4u) | 0x400u, 2);
        }
        if (d->msi_offset)
            wr(d->bdf, d->msi_offset + 2, rd(d->bdf, d->msi_offset + 2, 2) & ~1u, 2);
        if (d->msix_offset)
            wr(d->bdf, d->msix_offset + 2, (rd(d->bdf, d->msix_offset + 2, 2) & ~0x8000u) | 0x4000u,
               2);
        /* A read completion follows the function's earlier posted writes. */
        (void)rd(d->bdf, 4, 2);
        quiesced += quiesced_at[i];
    }
    LOGI("restart: %u function(s) quiesced (Bus Master Enable clear, INTx and MSI/MSI-X disabled)",
         quiesced);
}

/* Until the function answers with its own identifiers: FFFFh is nothing
 * yet, 0001h is Configuration Request Retry Status. */
static bool ready(uint16_t bdf, const struct snapshot *s, uint64_t start)
{
    for (;;) {
        uint32_t id = rd(bdf, 0, 4);
        if (id == s->config[0])
            return true;
        if ((id & 0xffff) != 0xffff && (id & 0xffff) != 0x0001) {
            LOGE("restart: %s answers %08x after the reset, not %08x", name(bdf), id, s->config[0]);
            return false;
        }
        if (ms_since(start) >= READY_MS) {
            LOGE("restart: %s not ready %u ms after the reset (last read %08x)", name(bdf),
                 READY_MS, id);
            return false;
        }
        sleep_ms(1);
    }
}

static unsigned restore_value(uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    if (rd(bdf, off, size) == value)
        return 0;
    wr(bdf, off, value, size);
    return 1;
}
/* PCIe capability control registers: never Initiate FLR or Retrain Link.
 * Program L1 PM Substates with ASPM off (PCIe Base 5.0 §5.5.4),
 * and LTR values before Device Control 2 enables LTR. */
static unsigned restore_pcie(uint16_t bdf, const struct snapshot *s)
{
    unsigned p = s->pcie, writes = 0;
    if (!p)
        return 0;
    if (s->l1ss) {
        uint16_t link = (uint16_t)rd(bdf, p + 0x10, 2);
        if (link & 3) {
            wr(bdf, p + 0x10, link & ~3u, 2);
            ++writes;
        }
        writes += restore_value(bdf, s->l1ss + 0x0c, s->l1ss_ctrl[1], 4);
        writes += restore_value(bdf, s->l1ss + 8, s->l1ss_ctrl[0], 4);
    }
    if (s->ltr)
        writes += restore_value(bdf, s->ltr + 4, s->ltr_value, 4);
    unsigned type = (field(s, p + 2, 2) >> 4) & 0xf, version = field(s, p + 2, 2) & 0xf;
    writes += restore_value(bdf, p + 8, field(s, p + 8, 2) & ~0x8000u, 2);
    writes += restore_value(bdf, p + 0x10, field(s, p + 0x10, 2) & ~0x0020u, 2);
    if (field(s, p + 2, 2) & 0x100)
        writes += restore_value(bdf, p + 0x18, field(s, p + 0x18, 2), 2);
    if (type == 4)
        writes += restore_value(bdf, p + 0x1c, field(s, p + 0x1c, 2), 2);
    if (version >= 2) {
        writes += restore_value(bdf, p + 0x28, field(s, p + 0x28, 2), 2);
        writes += restore_value(bdf, p + 0x30, field(s, p + 0x30, 2), 2);
        if (field(s, p + 2, 2) & 0x100)
            writes += restore_value(bdf, p + 0x38, field(s, p + 0x38, 2), 2);
    }
    return writes;
}
static unsigned restore_extended(uint16_t bdf, const struct snapshot *s)
{
    unsigned writes = 0;
    if (s->aer) {
        writes += restore_value(bdf, s->aer + 0x08, s->aer_regs[0], 4);
        writes += restore_value(bdf, s->aer + 0x0c, s->aer_regs[1], 4);
        writes += restore_value(bdf, s->aer + 0x14, s->aer_regs[2], 4);
    }
    if (s->acs)
        writes += restore_value(bdf, s->acs + 6, s->acs_ctrl, 2);
    if (s->ari)
        writes += restore_value(bdf, s->ari + 6, s->ari_ctrl, 2);
    return writes;
}
static bool acs_restored(uint16_t bdf, const struct snapshot *s)
{
    if ((!s->acs || rd(bdf, s->acs + 6, 2) == s->acs_ctrl) &&
        (!s->ari || rd(bdf, s->ari + 6, 2) == s->ari_ctrl))
        return true;
    wr(bdf, 4, rd(bdf, 4, 2) & ~4u, 2);
    LOGE("restart: %s ACS/ARI Control did not restore; Bus Master Enable clear", name(bdf));
    return false;
}
/* For an endpoint: the command register last with Bus Master Enable
 * clear; then the identifiers and BARs must match the saved values. */
static bool restore_function(const struct pci_device *d, const struct snapshot *s)
{
    uint16_t bdf = d->bdf;
    unsigned writes = 0;
    to_d0(bdf, s);
    for (unsigned k = 0; s->rebar && k < s->rebar_count; ++k)
        writes += restore_value(bdf, s->rebar + 8 + k * 8, s->rebar_ctrl[k], 4);
    for (unsigned off = 0x10; off <= 0x24; off += 4)
        writes += restore_value(bdf, off, s->config[off / 4], 4);
    writes += restore_value(bdf, 0x30, s->config[0x30 / 4], 4);
    writes += restore_value(bdf, 0x0c, field(s, 0x0c, 1), 1);
    writes += restore_value(bdf, 0x0d, field(s, 0x0d, 1), 1);
    writes += restore_value(bdf, 0x3c, field(s, 0x3c, 1), 1);
    writes += restore_pcie(bdf, s);
    if (s->pm)
        writes += restore_value(bdf, s->pm + 4, field(s, s->pm + 4, 2) & ~0x8003u, 2);
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(device_bytes); ++k)
        if (d->vendor == device_bytes[k].vendor && d->class_code == device_bytes[k].class_code)
            writes += restore_value(bdf, device_bytes[k].off,
                                    field(s, device_bytes[k].off, device_bytes[k].size),
                                    device_bytes[k].size);
    if (s->msi)
        writes += restore_value(bdf, s->msi + 2, field(s, s->msi + 2, 2) & ~1u, 2);
    if (s->msix)
        writes +=
            restore_value(bdf, s->msix + 2, (field(s, s->msix + 2, 2) & ~0x8000u) | 0x4000u, 2);
    writes += restore_extended(bdf, s);
    if (!acs_restored(bdf, s))
        return false;
    writes += restore_value(bdf, 4, (field(s, 4, 2) & ~4u) | 0x400u, 2);
    bool same = rd(bdf, 0, 4) == s->config[0];
    for (unsigned off = 0x10; off <= 0x24; off += 4)
        same = same && rd(bdf, off, 4) == s->config[off / 4];
    LOGI("restart: %s restored %u register(s)%s", name(bdf), writes,
         same ? "" : "; IDs or BARs DIFFER");
    return same;
}
static bool restore_bridge(const struct pci_device *d, const struct snapshot *s)
{
    uint16_t bdf = d->bdf;
    unsigned writes = 0;
    writes += restore_value(bdf, 0x18, s->config[0x18 / 4], 4); /* bus numbers first */
    writes += restore_value(bdf, 0x10, s->config[0x10 / 4], 4);
    writes += restore_value(bdf, 0x14, s->config[0x14 / 4], 4);
    writes += restore_value(bdf, 0x1c, field(s, 0x1c, 2), 2); /* not the RW1C secondary status */
    for (unsigned off = 0x20; off <= 0x30; off += 4)
        writes += restore_value(bdf, off, s->config[off / 4], 4);
    writes += restore_value(bdf, 0x38, s->config[0x38 / 4], 4);
    writes += restore_value(bdf, 0x0c, field(s, 0x0c, 1), 1);
    writes += restore_value(bdf, 0x0d, field(s, 0x0d, 1), 1);
    writes += restore_value(bdf, 0x3c, field(s, 0x3c, 1), 1);
    writes += restore_value(bdf, 0x3e, field(s, 0x3e, 2) & ~0x40u, 2);
    writes += restore_pcie(bdf, s);
    writes += restore_extended(bdf, s);
    if (!acs_restored(bdf, s))
        return false;
    writes += restore_value(bdf, 4, field(s, 4, 2), 2);
    bool same = rd(bdf, 0, 4) == s->config[0] && rd(bdf, 0x18, 4) == s->config[0x18 / 4];
    LOGI("restart: bridge %s restored %u register(s)%s", name(bdf), writes,
         same ? "" : "; IDs or buses DIFFER");
    return same;
}

static bool below(const struct pci_device *b, const struct pci_device *d)
{
    unsigned bus = d->bdf >> 8;
    return bus >= b->secondary && bus <= b->subordinate;
}
/* SBR resets everything below the bridge: only when every bridge below is
 * guest-owned and every function below is resettable. */
static bool sbr_allowed(const struct pci_device *b)
{
    if (b->header != 1 || b->owner != PCI_OWNER_GUEST)
        return false;
    bool any = false;
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (!below(b, d))
            continue;
        if (d->header == 1 ? d->owner != PCI_OWNER_GUEST : !resettable(d))
            return false;
        any = any || d->header != 1;
    }
    return any;
}

/* Slot Control is a hot-plug command: when the slot reports command
 * completion, wait for it and acknowledge it (PCIe Base 5.0 §6.7.3.2). */
static void slot_command(uint16_t bdf, unsigned p, uint16_t value)
{
    wr(bdf, p + 0x18, value, 2);
    if (rd(bdf, p + 0x14, 4) & (1u << 18))
        return; /* No Command Completed Support */
    uint64_t start = hal_counter();
    while (!(rd(bdf, p + 0x1a, 2) & 0x10) && ms_since(start) < SLOT_MS)
        sleep_ms(1);
    wr(bdf, p + 0x1a, 0x10, 2);
}

static bool sbr(int at, bool *done)
{
    const struct pci_device *b = platform_pci_device((unsigned)at);
    const struct snapshot *s = &snaps[at];
    uint16_t bdf = b->bdf;
    unsigned p = s->pcie, aer = s->aer;
    to_d0(bdf, s);
    uint16_t control = (uint16_t)rd(bdf, 0x3e, 2), devctl = p ? (uint16_t)rd(bdf, p + 8, 2) : 0;
    uint32_t mask = aer ? rd(bdf, aer + 8, 4) : 0;
    /* Suppress presence and link-state change events during slot reset. */
    bool slot = p && (rd(bdf, p + 2, 2) & 0x100);
    uint16_t slot_control = slot ? (uint16_t)rd(bdf, p + 0x18, 2) : 0;
    if (slot && (slot_control & 0x1008))
        slot_command(bdf, p, slot_control & ~0x1008u);
    /* The link goes down: no Surprise Down report, no SERR# (an unmasked
     * fatal error can raise a platform sync flood on AMD hosts). */
    if (aer)
        wr(bdf, aer + 8, mask | 0x20u, 4);
    if (p)
        wr(bdf, p + 8, devctl & ~0xfu, 2);
    wr(bdf, 0x3e, control & ~0x02u, 2);
    uint32_t lnkcap = p ? rd(bdf, p + 0x0c, 4) : 0;
    bool dlll = (lnkcap & (1u << 20)) && (rd(bdf, p + 0x12, 2) & 0x2000);
    wr(bdf, 0x3e, (control & ~0x02u) | 0x40u, 2);
    sleep_ms(HOLD_MS);
    wr(bdf, 0x3e, control & ~0x42u, 2);
    uint64_t release = hal_counter();
    LOGI("restart: Secondary Bus Reset on bridge %s (buses %02x-%02x)", name(bdf), b->secondary,
         b->subordinate);
    bool ok = true;
    if (dlll) {
        while (!(rd(bdf, p + 0x12, 2) & 0x2000) && ms_since(release) < LINK_MS)
            sleep_ms(1);
        if (!(rd(bdf, p + 0x12, 2) & 0x2000)) {
            LOGE("restart: bridge %s link not active %u ms after the reset", name(bdf), LINK_MS);
            ok = false;
        }
    }
    if (ok)
        sleep_ms(SETTLE_MS);
    /* Bridges below in depth order (ascending bus), then the functions. */
    for (unsigned pass = 0; pass < 2 && ok; ++pass)
        for (unsigned bus = b->secondary; bus <= b->subordinate && ok; ++bus)
            for (unsigned i = 0; i < platform_pci_count() && ok; ++i) {
                const struct pci_device *d = platform_pci_device(i);
                if ((unsigned)(d->bdf >> 8) != bus || (d->header == 1) != (pass == 0))
                    continue;
                ok = snaps[i].valid && ready(d->bdf, &snaps[i], release) &&
                     (pass == 0 ? restore_bridge(d, &snaps[i]) : restore_function(d, &snaps[i]));
                done[i] = true;
            }
    if (p)
        wr(bdf, p + 0x0a, 0x000f, 2); /* Device Status errors (RW1C) */
    wr(bdf, 0x1e, 0xf900, 2);         /* Secondary Status errors (RW1C) */
    if (aer)
        wr(bdf, aer + 4, rd(bdf, aer + 4, 4), 4); /* Uncorrectable Error Status (RW1C) */
    if (aer)
        wr(bdf, aer + 8, mask, 4);
    if (p)
        wr(bdf, p + 8, devctl, 2);
    wr(bdf, 0x3e, control & ~0x40u, 2);
    if (slot) {
        wr(bdf, p + 0x1a, 0x0108, 2); /* Presence Detect, DLL State Changed (RW1C) */
        if (slot_control & 0x1008)
            slot_command(bdf, p, slot_control);
    }
    LOGI("restart: bridge %s reset done %llu ms after release%s", name(bdf),
         (unsigned long long)ms_since(release), ok ? "" : "; a function did not come back");
    return ok;
}

static bool flr_capable(const struct snapshot *s)
{
    return s->valid && s->pcie && (field(s, s->pcie + 4, 4) & (1u << 28));
}
static bool flr(int at)
{
    const struct pci_device *d = platform_pci_device((unsigned)at);
    const struct snapshot *s = &snaps[at];
    uint16_t bdf = d->bdf;
    unsigned p = s->pcie;
    to_d0(bdf, s);
    uint64_t start = hal_counter();
    /* PCIe Base §6.6.2: wait for Transactions Pending to clear first. */
    while ((rd(bdf, p + 0x0a, 2) & 0x20) && ms_since(start) < SETTLE_MS)
        sleep_ms(1);
    if (rd(bdf, p + 0x0a, 2) & 0x20)
        LOGW("restart: %s Transactions Pending after %u ms; Function Level Reset anyway", name(bdf),
             SETTLE_MS);
    wr(bdf, p + 8, rd(bdf, p + 8, 2) | 0x8000u, 2);
    uint64_t release = hal_counter();
    sleep_ms(SETTLE_MS);
    LOGI("restart: Function Level Reset of %s", name(bdf));
    return ready(bdf, s, release) && restore_function(d, s);
}

/* Display on restart: what VMM start did. A
 * display=vbios card POSTed or mode-set at VMM start gets the same call
 * again; any other display keeps the state the previous boot left. */
static bool restart_display(const struct pci_device *d)
{
    const struct platform_vbios_state *v = platform_vbios_state();
    if (v->posted && v->bdf == d->bdf)
        return platform_vbios_restart();
    LOGI("restart: display %s keeps its state%s", name(d->bdf),
         platform_pci_restart_untouched(d) ? " (untouched: no Bus Master Enable this boot)"
                                           : " (quiesced)");
    return true;
}

bool platform_pci_reset_functions(void)
{
    static bool done[PCI_MAX_DEVICES];
    memset(done, 0, sizeof(done));
    bool ok = true;
    /* By bus: a bridge's reset covers the functions on later buses below it. */
    for (unsigned bus = 0; bus < 256 && ok; ++bus)
        for (unsigned i = 0; i < platform_pci_count() && ok; ++i) {
            const struct pci_device *d = platform_pci_device(i);
            if ((unsigned)(d->bdf >> 8) != bus || done[i] || !resettable(d))
                continue;
            int parent = d->parent != 0xffff ? index_of(d->parent) : -1;
            if (parent >= 0 && snaps[parent].valid &&
                sbr_allowed(platform_pci_device((unsigned)parent))) {
                ok = sbr(parent, done);
                continue;
            }
            done[i] = true;
            if (flr_capable(&snaps[i]))
                ok = flr((int)i);
            else {
                LOGI("restart: %s has no reset method; quiesced and restored only", name(d->bdf));
                ok = restore_function(d, &snaps[i]);
            }
        }
    for (unsigned i = 0; i < platform_pci_count() && ok; ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (d->owner == PCI_OWNER_GUEST && d->header == 0 && display(d))
            ok = restart_display(d);
    }
    if (!ok)
        LOGE("restart: a function reset failed; the hardware no longer matches the inventory");
    return ok;
}
