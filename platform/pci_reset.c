/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reset the GPU before legacy VBIOS POST, on the management CPU before
 * guest execution, through host configuration space.
 *
 * Secondary Bus Reset (PCI-to-PCI Bridge Architecture 1.2 §3.2.5.17, bridge
 * control bit 6) on the GPU's upstream bridge when nothing but the GPU's
 * own functions sits below it; otherwise Function Level Reset (PCIe Base
 * 5.0 §6.6.2) of the one function when it advertises FLR; otherwise refused.
 * Timing follows PCIe Base 5.0 §6.6.1. Restore Resizable BAR controls
 * before BAR addresses, then the header, PCIe controls, MSI/MSI-X and
 * finally the command register.
 */
#include "ia64vmm/pci.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define RESET_MAX_FUNCTIONS 8u
#define RESET_HOLD_MS 2u     /* Trst 1 ms + margin */
#define RESET_SETTLE_MS 100u /* §6.6.1 */
#define RESET_LINK_MS 1000u  /* Link training bound */
/* Bounded readiness polling for devices that continue returning CRS. */
#define RESET_READY_MS 10000u
#define REBAR_MAX 6u

struct saved {
    uint16_t bdf, vendor, device, acs_ctrl, ari_ctrl;
    uint32_t config[64];      /* 0x00-0xFF as read before the reset */
    uint16_t rebar, acs, ari; /* extended capability offsets, 0 if none */
    uint8_t pcie, msi, msix;
    unsigned rebar_count;
    uint32_t rebar_ctrl[REBAR_MAX];
};

static uint32_t rd(uint16_t bdf, unsigned off, unsigned size)
{
    return platform_pci_host_read(bdf, off, size);
}
static void wr(uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    platform_pci_host_write(bdf, off, value, size);
}
static uint32_t field(const struct saved *s, unsigned off, unsigned size)
{
    uint64_t next = off % 4 + size > 4 && off / 4 + 1 < 64 ? s->config[off / 4 + 1] : 0;
    uint64_t dword = s->config[off / 4] | next << 32;
    uint64_t value = dword >> ((off % 4) * 8);
    return (uint32_t)(size == 4 ? value : value & ((1u << (size * 8)) - 1));
}
static uint8_t capability(const struct saved *s, unsigned id)
{
    if (!(field(s, 6, 2) & 0x10))
        return 0;
    bool seen[64] = {false};
    for (unsigned p = field(s, 0x34, 1) & ~3u; p >= 0x40 && p <= 0xfc && !seen[p / 4];
         p = field(s, p + 1, 1) & ~3u) {
        seen[p / 4] = true;
        if (field(s, p, 1) == id)
            return (uint8_t)p;
    }
    return 0;
}

static uint64_t ms_since(uint64_t start)
{
    uint64_t hz = hal_counter_hz();
    return hz >= 1000 ? (hal_counter() - start) / (hz / 1000) : 0;
}
static void sleep_ms(uint64_t ms)
{
    uint64_t hz = hal_counter_hz(), deadline = hal_counter() + ms * (hz / 1000);
    while (hal_counter() < deadline)
        hal_idle_until(deadline);
}

static void save(struct saved *s, uint16_t bdf)
{
    memset(s, 0, sizeof(*s));
    s->bdf = bdf;
    for (unsigned i = 0; i < 64; ++i)
        s->config[i] = rd(bdf, i * 4, 4);
    s->vendor = (uint16_t)s->config[0];
    s->device = (uint16_t)(s->config[0] >> 16);
    s->pcie = capability(s, 0x10);
    s->msi = capability(s, 0x05);
    s->msix = capability(s, 0x11);
    /* Extended configuration (PCIe Base §7.6): only on PCIe functions. */
    if (!s->pcie)
        return;
    bool seen[1024] = {false};
    for (unsigned at = 0x100; at >= 0x100 && at <= 0xffc && !seen[at / 4];) {
        seen[at / 4] = true;
        uint32_t header = rd(bdf, at, 4);
        if (!header || header == UINT32_MAX)
            break;
        if ((header & 0xffff) == 0x15 && at <= 0xff4) { /* Resizable BAR */
            s->rebar = (uint16_t)at;
            s->rebar_count = (rd(bdf, at + 8, 4) >> 5) & 7;
            if (s->rebar_count > REBAR_MAX)
                s->rebar_count = REBAR_MAX;
            if (s->rebar_count > (0xffc - at) / 8)
                s->rebar_count = (0xffc - at) / 8;
            for (unsigned i = 0; i < s->rebar_count; ++i)
                s->rebar_ctrl[i] = rd(bdf, at + 8 + i * 8, 4);
        }
        if ((header & 0xffff) == 0x0d && at <= 0xff8) { /* Access Control Services */
            s->acs = (uint16_t)at;
            s->acs_ctrl = (uint16_t)rd(bdf, at + 6, 2);
        }
        if ((header & 0xffff) == 0x0e && at <= 0xff8) { /* ARI Function Groups */
            s->ari = (uint16_t)at;
            s->ari_ctrl = (uint16_t)rd(bdf, at + 6, 2);
        }
        at = (header >> 20) & ~3u;
    }
}

static const char *name(uint16_t bdf)
{
    static char text[4][12];
    static unsigned next;
    char *t = text[next++ % 4];
    vmm_snprintf(t, 12, "%02x:%02x.%u", bdf >> 8, (bdf >> 3) & 31, bdf & 7);
    return t;
}

/* Verify isolation before any reset function regains Bus Master Enable. */
static bool restore_acs(const struct saved *s)
{
    if (s->ari && rd(s->bdf, s->ari + 6, 2) != s->ari_ctrl)
        wr(s->bdf, s->ari + 6, s->ari_ctrl, 2);
    if (s->acs && rd(s->bdf, s->acs + 6, 2) != s->acs_ctrl)
        wr(s->bdf, s->acs + 6, s->acs_ctrl, 2);
    if ((!s->acs || rd(s->bdf, s->acs + 6, 2) == s->acs_ctrl) &&
        (!s->ari || rd(s->bdf, s->ari + 6, 2) == s->ari_ctrl))
        return true;
    wr(s->bdf, 4, rd(s->bdf, 4, 2) & ~4u, 2);
    LOGE("VBIOS reset: %s ACS/ARI Control did not restore; Bus Master Enable clear", name(s->bdf));
    return false;
}

/* Restore changed registers, command last; return the number written. */
static unsigned restore(const struct saved *s)
{
    unsigned writes = 0;
    uint16_t bdf = s->bdf;
#define RESTORE(off, size)                                                                         \
    do {                                                                                           \
        uint32_t v_ = field(s, (off), (size));                                                     \
        if (rd(bdf, (off), (size)) != v_) {                                                        \
            wr(bdf, (off), v_, (size));                                                            \
            ++writes;                                                                              \
        }                                                                                          \
    } while (0)
    for (unsigned i = 0; s->rebar && i < s->rebar_count; ++i) {
        unsigned off = s->rebar + 8 + i * 8;
        if (rd(bdf, off, 4) != s->rebar_ctrl[i]) {
            wr(bdf, off, s->rebar_ctrl[i], 4);
            ++writes;
        }
    }
    unsigned type = field(s, 0x0e, 1) & 0x7f;
    if (type == 0) {
        for (unsigned off = 0x10; off <= 0x24; off += 4)
            RESTORE(off, 4);
        RESTORE(0x30, 4);
    } else if (type == 1) {
        RESTORE(0x10, 4);
        RESTORE(0x14, 4);
        RESTORE(0x18, 4); /* bus numbers, secondary latency */
        RESTORE(0x1c, 2); /* I/O base/limit (not the RW1C status) */
        for (unsigned off = 0x20; off <= 0x30; off += 4)
            RESTORE(off, 4);
        RESTORE(0x38, 4);
    }
    RESTORE(0x0c, 1);
    RESTORE(0x0d, 1);
    RESTORE(0x3c, 1);
    if (type == 1)
        RESTORE(0x3e, 2); /* bridge control, Secondary Bus Reset clear */
    if (s->pcie) {
        unsigned p = s->pcie;
        uint16_t devctl = (uint16_t)(field(s, p + 8, 2) & ~0x8000u); /* never Initiate FLR */
        if (rd(bdf, p + 8, 2) != devctl) {
            wr(bdf, p + 8, devctl, 2);
            ++writes;
        }
        uint16_t lnkctl = (uint16_t)(field(s, p + 0x10, 2) & ~0x0020u); /* never Retrain Link */
        if ((rd(bdf, p + 0x10, 2) & ~0x0020u) != lnkctl) {
            wr(bdf, p + 0x10, lnkctl, 2);
            ++writes;
        }
        if ((field(s, p + 2, 2) & 0xf) >= 2) {
            RESTORE(p + 0x28, 2);
            RESTORE(p + 0x30, 2);
        }
    }
    if (s->msi) {
        unsigned p = s->msi, control = field(s, p + 2, 2);
        bool wide = control & 0x80, mask = control & 0x100;
        RESTORE(p + 4, 4);
        if (wide)
            RESTORE(p + 8, 4);
        RESTORE(p + (wide ? 12 : 8), 2);
        if (mask)
            RESTORE(p + (wide ? 16 : 12), 4);
        RESTORE(p + 2, 2);
    }
    if (s->msix)
        RESTORE(s->msix + 2, 2);
    RESTORE(0x04, 2); /* command last */
#undef RESTORE
    return writes;
}

/* After the reset: until the function answers with its own identifiers. */
static bool ready(const struct saved *s, uint64_t start, unsigned *retries)
{
    *retries = 0;
    for (;;) {
        uint32_t id = rd(s->bdf, 0, 4);
        if (id == ((uint32_t)s->device << 16 | s->vendor))
            return true;
        /* FFFFh: nothing yet; 0001h: Configuration Request Retry Status
         * with CRS Software Visibility. Anything else is another device. */
        if ((id & 0xffff) != 0xffff && (id & 0xffff) != 0x0001) {
            LOGE("VBIOS reset: %s answers %04x:%04x, not %04x:%04x; not restored", name(s->bdf),
                 id & 0xffff, id >> 16, s->vendor, s->device);
            return false;
        }
        if (ms_since(start) >= RESET_READY_MS) {
            LOGE("VBIOS reset: %s not ready %u ms after the reset (last read %08x); not restored",
                 name(s->bdf), RESET_READY_MS, id);
            return false;
        }
        ++*retries;
        sleep_ms(1);
    }
}

static bool finish(struct saved *fns, unsigned n, uint64_t start)
{
    for (unsigned i = 0; i < n; ++i) {
        unsigned retries;
        if (!ready(&fns[i], start, &retries))
            return false;
        LOGI("VBIOS reset: %s answers %04x:%04x %llu ms after release (%u not-ready reads)",
             name(fns[i].bdf), fns[i].vendor, fns[i].device, (unsigned long long)ms_since(start),
             retries);
    }
    for (unsigned i = 0; i < n; ++i)
        if (!restore_acs(&fns[i])) {
            for (unsigned j = 0; j < n; ++j)
                wr(fns[j].bdf, 4, rd(fns[j].bdf, 4, 2) & ~4u, 2);
            return false;
        }
    for (unsigned i = 0; i < n; ++i) {
        const struct saved *s = &fns[i];
        unsigned writes = restore(s), bad = 0;
        for (unsigned off = 0x10; off <= ((field(s, 0x0e, 1) & 0x7f) == 1 ? 0x14u : 0x24u);
             off += 4)
            bad += rd(s->bdf, off, 4) != s->config[off / 4];
        bad += rd(s->bdf, 0, 4) != s->config[0];
        LOGI("VBIOS reset: %s restored %u register(s) (ReBAR %u, BARs, ROM BAR%s%s%s), "
             "command %04x%s",
             name(s->bdf), writes, s->rebar_count, s->pcie ? ", PCIe control" : "",
             s->msi ? ", MSI" : "", s->msix ? ", MSI-X" : "", field(s, 4, 2),
             bad ? "; IDs or BARs DIFFER from the saved values" : "; IDs and BARs as saved");
        if (bad)
            return false;
    }
    return true;
}

static enum pci_reset_result flr(uint16_t bdf, const char *why)
{
    static struct saved fn;
    save(&fn, bdf);
    LOGI(
        "VBIOS reset: Secondary Bus Reset unsuitable (%s); Function Level Reset of %s only, which leaves the "
        "link and the other functions (HD audio) as they are",
        why, name(bdf));
    if (fn.rebar && !fn.rebar_count)
        LOGW("VBIOS reset: %s Resizable BAR without entries", name(bdf));
    uint64_t start = hal_counter();
    /* PCIe Base §6.6.2: wait for Transactions Pending to clear first. */
    while ((rd(bdf, fn.pcie + 0x0a, 2) & 0x20) && ms_since(start) < RESET_SETTLE_MS)
        sleep_ms(1);
    if (rd(bdf, fn.pcie + 0x0a, 2) & 0x20)
        LOGW("VBIOS reset: %s Transactions Pending still set after %u ms; FLR anyway", name(bdf),
             RESET_SETTLE_MS);
    uint16_t devctl = (uint16_t)rd(bdf, fn.pcie + 8, 2);
    wr(bdf, fn.pcie + 8, devctl | 0x8000u, 2);
    uint64_t release = hal_counter();
    sleep_ms(RESET_SETTLE_MS);
    LOGI("VBIOS reset: %s Initiate Function Level Reset (device control %04x), waited %u ms",
         name(bdf), devctl, RESET_SETTLE_MS);
    return finish(&fn, 1, release) ? PCI_RESET_DONE : PCI_RESET_FAILED;
}

enum pci_reset_result platform_pci_reset_function(uint16_t bdf)
{
    const struct pci_device *gpu = NULL, *bridge = NULL;
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (d->bdf == bdf)
            gpu = d;
    }
    if (!gpu) {
        LOGE("VBIOS reset: %s is not in the PCI inventory; refused", name(bdf));
        return PCI_RESET_REFUSED;
    }
    for (unsigned i = 0; gpu->parent != 0xffff && i < platform_pci_count(); ++i)
        if (platform_pci_device(i)->bdf == gpu->parent)
            bridge = platform_pci_device(i);
    static struct saved gpu_saved;
    save(&gpu_saved, bdf);
    bool flr_capable = gpu_saved.pcie && (rd(bdf, gpu_saved.pcie + 4, 4) & (1u << 28));
    /* Secondary Bus Reset resets everything below the bridge: only when
     * that is the GPU's own functions on the bridge's secondary bus. */
    static char why[96];
    if (!bridge) {
        vmm_snprintf(why, sizeof(why), "%s is on a root bus, no bridge above it", name(bdf));
        goto fallback;
    }
    if (bridge->header != 1) {
        vmm_snprintf(why, sizeof(why), "parent %s is not a PCI-to-PCI bridge", name(bridge->bdf));
        goto fallback;
    }
    if ((bdf >> 8) != bridge->secondary) {
        vmm_snprintf(why, sizeof(why), "%s is not on bridge %s's secondary bus", name(bdf),
                     name(bridge->bdf));
        goto fallback;
    }
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        unsigned bus = d->bdf >> 8;
        if (bus >= bridge->secondary && bus <= bridge->subordinate &&
            (pci_device_group(d) != pci_device_group(gpu) || d->owner != PCI_OWNER_GUEST)) {
            vmm_snprintf(why, sizeof(why), "%s %04x:%04x is below bridge %s too", name(d->bdf),
                         d->vendor, d->device, name(bridge->bdf));
            goto fallback;
        }
    }
    static struct saved fns[RESET_MAX_FUNCTIONS], bridge_saved;
    unsigned n = 0;
    char list[RESET_MAX_FUNCTIONS * 32] = ""; /* ", bb:dd.f vvvv:dddd (ReBAR)" is 27 */
    int used = 0;
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (pci_device_group(d) != pci_device_group(gpu))
            continue;
        if (n == RESET_MAX_FUNCTIONS) {
            vmm_snprintf(why, sizeof(why),
                         "ARI device exceeds the POST reset's %u-function save limit",
                         RESET_MAX_FUNCTIONS);
            goto fallback;
        }
        save(&fns[n], d->bdf);
        if (used < (int)sizeof(list))
            used += vmm_snprintf(list + used, sizeof(list) - (size_t)used, "%s%s %04x:%04x%s",
                                 n ? ", " : "", name(d->bdf), fns[n].vendor, fns[n].device,
                                 fns[n].rebar ? " (ReBAR)" : "");
        ++n;
    }
    save(&bridge_saved, bridge->bdf);
    LOGI(
        "VBIOS reset: Secondary Bus Reset on bridge %s (bus %02x): only the GPU's function(s) below: %s; "
        "configuration saved",
        name(bridge->bdf), bridge->secondary, list);
    if (gpu_saved.pcie && rd(bdf, 0x100, 4) == UINT32_MAX)
        LOGW(
            "VBIOS reset: no extended configuration space for %s (no ECAM, or a conventional PCI function): "
            "a Resizable BAR cannot be seen or restored",
            name(bdf));
    if (!bridge_saved.pcie)
        LOGW("VBIOS reset: bridge %s has no PCIe capability; fixed waits", name(bridge->bdf));
    /* §6.6.1: above 5.0 GT/s the port reports Data Link Layer Link Active
     * and the 100 ms count from Link training; else from the reset. A port
     * that claims the reporting but showed the link down while the GPU
     * answered cannot be believed after the reset either: fixed wait. */
    unsigned p = bridge_saved.pcie;
    uint32_t lnkcap = p ? rd(bridge->bdf, p + 0x0c, 4) : 0;
    uint16_t lnksta = p ? (uint16_t)rd(bridge->bdf, p + 0x12, 2) : 0;
    unsigned speed = lnkcap & 0xf;
    bool dlll = lnkcap & (1u << 20);
    if (dlll && !(lnksta & 0x2000)) {
        LOGW(
            "VBIOS reset: bridge %s reports Data Link Layer Link Active capability but Link Status %04x shows the "
            "link down while %s answers; the fixed wait applies",
            name(bridge->bdf), lnksta, name(bdf));
        dlll = false;
    }
    uint16_t control = (uint16_t)rd(bridge->bdf, 0x3e, 2);
    wr(bridge->bdf, 0x3e, control | 0x40u, 2);
    uint64_t asserted = hal_counter();
    sleep_ms(RESET_HOLD_MS);
    wr(bridge->bdf, 0x3e, control & ~0x40u, 2);
    uint64_t release = hal_counter(), hz = hal_counter_hz();
    LOGI("VBIOS reset: bridge %s control %04x -> %04x (Secondary Bus Reset) for %llu ms, released",
         name(bridge->bdf), control, control | 0x40u,
         (unsigned long long)(hz >= 1000 ? (release - asserted) / (hz / 1000) : 0));
    if (dlll) {
        while (!(rd(bridge->bdf, p + 0x12, 2) & 0x2000) && ms_since(release) < RESET_LINK_MS)
            sleep_ms(1);
        if (!(rd(bridge->bdf, p + 0x12, 2) & 0x2000)) {
            LOGE(
                "VBIOS reset: bridge %s link not active %u ms after the reset (Link Status %04x); GPU not "
                "restored",
                name(bridge->bdf), RESET_LINK_MS, (unsigned)rd(bridge->bdf, p + 0x12, 2));
            return PCI_RESET_FAILED;
        }
        LOGI(
            "VBIOS reset: link active %llu ms after release (Data Link Layer Link Active); waiting %u ms",
            (unsigned long long)ms_since(release), RESET_SETTLE_MS);
    } else {
        if (speed > 2 && !(lnkcap & (1u << 20)))
            LOGW("VBIOS reset: bridge %s supports %s GT/s but no Data Link Layer Link Active "
                 "reporting (PCIe Base §7.5.3.6 requires it); fixed %u ms",
                 name(bridge->bdf),
                 speed == 3   ? "8.0"
                 : speed == 4 ? "16.0"
                              : "32.0+",
                 RESET_SETTLE_MS);
        LOGI("VBIOS reset: no Link Active reporting on %s; waiting %u ms", name(bridge->bdf),
             RESET_SETTLE_MS);
    }
    sleep_ms(RESET_SETTLE_MS);
    if (!restore_acs(&bridge_saved) || !finish(fns, n, release))
        return PCI_RESET_FAILED;
    unsigned writes = restore(&bridge_saved);
    LOGI("VBIOS reset: bridge %s registers %s (%u written); done %llu ms after the assert",
         name(bridge->bdf), writes ? "restored" : "unchanged", writes,
         (unsigned long long)ms_since(asserted));
    return PCI_RESET_DONE;
fallback:
    if (flr_capable)
        return flr(bdf, why);
    LOGE("VBIOS reset: refused: %s, and %s does not advertise Function Level Reset", why,
         name(bdf));
    return PCI_RESET_REFUSED;
}
