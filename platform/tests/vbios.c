/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hosted checks of the VBIOS environment. The HAL's container is replaced by a scripted
 * fake (--wrap=hal_x86rm_*), the host's configuration space by a fake PCI
 * topology with a bridge above the card, a sibling function, a second
 * bridge and a device behind it. Every check also runs on a broken
 * variant, which must be rejected.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "platform/vbios.h"
#include "platform/vbios_trace.h"
#include "ia64/memory.h"
#include "platform/platform.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/guest_mem.h"
#include "firmware/new/legacy-handoff.h"
#include "core/log_internal.h"

static unsigned failures;
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #e);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

/* --- fake configuration space -------------------------------------------- */

struct fake_fn {
    uint16_t bdf;
    uint8_t config[4096], defaults[4096]; /* defaults: the power-on state a reset restores */
    bool ecam;                            /* extended configuration reachable */
    bool acs_readonly;
    uint32_t masks[64];
    bool probing[64];
    unsigned writes;
    uint64_t ready_at; /* PCI reset fixture: answers from this time on */
    unsigned writes_not_ready, rebar_order_violations;
};
static struct fake_fn fns[16];
static unsigned nfns;
struct config_write {
    uint16_t bdf;
    unsigned off;
    uint32_t value;
};
static struct config_write wlog[1024];
static unsigned wlog_count;
/* Every configuration access with the mocked time (PCI reset reset checks). */
struct config_access {
    uint64_t time;
    uint32_t value;
    uint16_t bdf, off;
    uint8_t size;
    bool write;
};
static struct config_access alog[8192];
static unsigned alog_count;
static uint8_t rom_image[0x10000];
static uint8_t scratch[0x10000];
static unsigned flash_maps;
static uint8_t rom_view[0x10000];
static uint64_t rom_view_base;
static bool rom_irqs_off;
static bool dispi_mock;
static uint16_t dispi_index, dispi_id;
static unsigned dispi_reads, dispi_writes;
static unsigned rom_windows;
static bool expect_fb_quiesced;
static uint64_t rom_log_pos;
uint64_t __wrap_hal_irq_save(void)
{
    CHECK(!rom_irqs_off);
    rom_irqs_off = true;
    ++rom_windows;
    rom_log_pos = log_get_ring()->write_pos;
    if (expect_fb_quiesced)
        CHECK(!vmm_log_fb_enabled());
    return 0x200;
}
void __wrap_hal_irq_restore(uint64_t flags)
{
    CHECK(rom_irqs_off && flags == 0x200);
    CHECK(log_get_ring()->write_pos == rom_log_pos);
    rom_irqs_off = false;
}
static void refresh_rom_view(void);

static bool clock_mocked;
static uint64_t clock_now;
/* The fake's reset behaviour: link training after release, readiness of
 * the functions below, CRS (vendor 0001h) instead of all-ones meanwhile. */
static uint64_t fake_link_ms = 30, fake_ready_ms = 150, fake_flr_ms = 80, link_up_at;
static bool fake_crs, fake_link_never, fake_no_link_status;
#define MS (UINT64_C(1000000)) /* hal_counter units per ms: the hosted 1 GHz counter */

static struct fake_fn *fake(uint16_t bdf)
{
    for (unsigned i = 0; i < nfns; ++i)
        if (fns[i].bdf == bdf)
            return &fns[i];
    return NULL;
}
static void put(struct fake_fn *f, unsigned off, uint32_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i)
        f->config[off + i] = (uint8_t)(value >> (i * 8));
}
static uint32_t get(uint16_t bdf, unsigned off, unsigned size)
{
    struct fake_fn *f = fake(bdf);
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint32_t)f->config[off + i] << (i * 8);
    return value;
}
static void access_log(uint16_t bdf, unsigned off, unsigned size, bool write, uint32_t value)
{
    if (alog_count < sizeof(alog) / sizeof(alog[0]))
        alog[alog_count++] =
            (struct config_access){clock_now, value, bdf, (uint16_t)off, (uint8_t)size, write};
}
#define BRIDGE_PCIE 0x40u /* the fake bridge's PCIe capability */
#define CARD_PCIE 0x60u
#define CARD_MSI 0xa0u
#define CARD_REBAR 0x100u
#define CARD_ACS 0x140u
static uint32_t fake_value(struct fake_fn *f, uint16_t bdf, unsigned off, unsigned size)
{
    if (!f || off + size > (f->ecam ? 4096u : 256u))
        return UINT32_MAX;
    if (clock_now < f->ready_at)
        return fake_crs && off == 0 ? (size == 2 ? 0x0001u : 0xffff0001u)
                                    : (size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1);
    if (size == 4 && off < 256 && f->probing[off / 4])
        return f->masks[off / 4];
    uint32_t value = get(bdf, off, size);
    /* The bridge's Link Status: Data Link Layer Link Active (bit 13) once trained. */
    if (bdf == 0x0008 && off == BRIDGE_PCIE + 0x12 && size == 2)
        value = (value & ~0x2000u) | (!fake_no_link_status && (!fake_link_never || !link_up_at) &&
                                              clock_now >= link_up_at
                                          ? 0x2000u
                                          : 0);
    return value;
}
static uint32_t fake_read(void *opaque, uint16_t bdf, unsigned off, unsigned size)
{
    (void)opaque;
    uint32_t value = fake_value(fake(bdf), bdf, off, size);
    access_log(bdf, off, size, false, value);
    return value;
}
static void fake_write(void *opaque, uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    (void)opaque;
    struct fake_fn *f = fake(bdf);
    CHECK(f && off + size <= (f && f->ecam ? 4096u : 256u));
    if (!f)
        return;
    access_log(bdf, off, size, true, value);
    if (clock_now < f->ready_at) {
        ++f->writes_not_ready;
        return;
    } /* lost on a resetting function */
    ++f->writes;
    if (f->acs_readonly && off == CARD_ACS + 6)
        return;
    if (wlog_count < sizeof(wlog) / sizeof(wlog[0]))
        wlog[wlog_count++] = (struct config_write){bdf, off, value};
    /* A BAR's size follows its Resizable BAR control: programming BAR0
     * before the control is back would size it wrongly on real hardware. */
    if (f->ecam && off == 0x10 && get(bdf, CARD_REBAR + 8, 4) != 0x0420)
        ++f->rebar_order_violations;
    /* Secondary Bus Reset (bridge control bit 6): the functions on the
     * secondary bus are in reset while it is set and come back in their
     * power-on state; the link trains first. */
    if ((f->config[0x0e] & 0x7f) == 1 && off <= 0x3e && off + size > 0x3e) {
        bool was = f->config[0x3e] & 0x40, now = (value >> ((0x3e - off) * 8)) & 0x40;
        for (unsigned i = 0; was != now && i < nfns; ++i) {
            struct fake_fn *g = &fns[i];
            if ((g->bdf >> 8) != f->config[0x19])
                continue;
            if (now)
                g->ready_at = UINT64_MAX;
            else {
                memcpy(g->config, g->defaults, sizeof(g->config));
                g->ready_at = clock_now + fake_ready_ms * MS;
            }
        }
        if (was && !now)
            link_up_at = clock_now + fake_link_ms * MS;
        if (now)
            link_up_at = UINT64_MAX;
    }
    /* Function Level Reset: Device Control bit 15. */
    if (bdf == 0x0100 && off == CARD_PCIE + 8 && (value & 0x8000)) {
        memcpy(f->config, f->defaults, sizeof(f->config));
        f->ready_at = clock_now + fake_flr_ms * MS;
        return;
    }
    if (off < 256 && size == 4 && (value == UINT32_MAX || value == 0xfffffffeu) &&
        (f->masks[off / 4] || off == 0x30 || (off == 0x38 && (f->config[0x0e] & 0x7f) == 1))) {
        f->probing[off / 4] = true;
        return;
    }
    if (off < 256)
        f->probing[off / 4] = false;
    put(f, off, value, size);
    refresh_rom_view();
}
/* The IGD's register BAR (GTTMMADR, 0xf0000000): vbios_text_hold reads the
 * display registers below 1 MiB. */
static uint8_t igd_mmio[0x100000];
/* The mapped bytes follow both command.memory and ROM enable. Moving a
 * BAR with memory decode on is invalid even when its ROM bytes look right. */
static void refresh_rom_view(void)
{
    memset(rom_view, 0xff, sizeof(rom_view));
    for (unsigned i = 0; i < nfns; ++i) {
        struct fake_fn *f = &fns[i];
        unsigned off = (f->config[0x0e] & 0x7f) == 1 ? 0x38 : 0x30;
        uint32_t rom = get(f->bdf, off, 4);
        if (!f->probing[off / 4] && (get(f->bdf, 4, 2) & 2) && (rom & 1) &&
            (rom & ~0x7ffu) == rom_view_base)
            memcpy(rom_view, rom_image, sizeof(rom_view));
    }
}
static uint8_t *fake_map(void *opaque, uint64_t base, uint64_t size)
{
    (void)opaque;
    (void)size;
    if (base >= 0xffff0000)
        ++flash_maps;
    if (base == 0xf0000000)
        return igd_mmio;
    if (size == sizeof(rom_image)) {
        rom_view_base = base;
        refresh_rom_view();
        return rom_view;
    }
    return scratch;
}

#define HOST_BRIDGE 0x0000u
#define BRIDGE 0x0008u       /* 00:01.0, above the card */
#define OTHER_BRIDGE 0x0010u /* 00:02.0, bus 2 */
#define MEDIA 0x0018u        /* 00:03.0, the VMM's boot controller */
#define CARD 0x0100u         /* 01:00.0 */
#define AUDIO 0x0101u        /* 01:00.1 */
#define NIC 0x0200u          /* 02:00.0 */

static void build_rom(uint8_t *rom, unsigned size)
{
    /* PCI Firmware 3.0 §5.1: an NVIDIA-style data image (code type E0h)
     * first, then the legacy image (code type 0, last), 55AAh/PCIR. */
    memset(rom, 0xff, size);
    const struct {
        unsigned at, length;
        uint8_t type, indicator;
    } images[] = {
        {0, 0x200, 0xe0, 0},
        {0x200, 0x800, 0, 0x80},
    };
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *image = rom + images[i].at;
        memset(image, 0, images[i].length);
        image[0] = 0x55;
        image[1] = 0xaa;
        image[2] = (uint8_t)(images[i].length / 512);
        image[3] = 0xcb; /* retf: any entry */
        image[0x18] = 0x40;
        uint8_t *pcir = image + 0x40;
        memcpy(pcir, "PCIR", 4);
        pcir[4] = 0xde;
        pcir[5] = 0x10;
        pcir[6] = 0x04;
        pcir[7] = 0x2f;
        pcir[0x0a] = 0x18;
        pcir[0x0f] = 0x03;
        pcir[0x10] = (uint8_t)(images[i].length / 512);
        pcir[0x14] = images[i].type;
        pcir[0x15] = images[i].indicator;
        uint8_t sum = 0;
        for (unsigned k = 0; k < images[i].length; ++k)
            sum = (uint8_t)(sum + image[k]);
        image[images[i].length - 1] = (uint8_t)(0 - sum);
    }
}

static bool
    topology_extra; /* PCI reset fixture: a second device below the card's bridge (01:01.0) */
static bool topology_no_vga, topology_bad_mapping, topology_bad_rom;
static bool
    topology_no_flr; /* PCI reset fixture: the card does not advertise Function Level Reset */
static void build_topology(void)
{
    static const struct {
        uint16_t bdf;
        uint32_t id, cls;
        uint8_t header;
    } list[] = {
        {HOST_BRIDGE, 0x14801022, 0x060000, 0x80},
        {BRIDGE, 0x14831022, 0x060400, 1},
        {OTHER_BRIDGE, 0x14831022, 0x060400, 1},
        {MEDIA, 0x00101b36, 0x010802, 0},
        {CARD, 0x2f0410de, 0x030000, 0x80},
        {AUDIO, 0x22e810de, 0x040300, 0},
        {NIC, 0x12348086, 0x020000, 0},
        {0x0108, 0x816810ec, 0x020000, 0},
    };
    memset(fns, 0, sizeof(fns));
    nfns = sizeof(list) / sizeof(list[0]) - (topology_extra ? 0 : 1);
    for (unsigned i = 0; i < nfns; ++i) {
        struct fake_fn *f = &fns[i];
        f->bdf = list[i].bdf;
        put(f, 0, list[i].id, 4);
        put(f, 8, list[i].cls << 8, 4);
        put(f, 0x0e, list[i].header, 1);
        put(f, 4, 0x0006, 2);
    }
    put(fake(BRIDGE), 0x18, 0x00010100, 4);       /* buses 0 -> 1..1 */
    put(fake(OTHER_BRIDGE), 0x18, 0x00020200, 4); /* buses 0 -> 2..2 */
    put(fake(BRIDGE), 0x20, 0x84f08100, 4);       /* non-prefetchable 81000000..84ffffff */
    put(fake(BRIDGE), 0x24, 0x80f08000, 4);       /* prefetchable 80000000..80ffffff */
    put(fake(OTHER_BRIDGE), 0x20, 0x83f08300, 4);
    put(fake(OTHER_BRIDGE), 0x3e, 0x0008, 2); /* VGA Enable on the wrong path */
    put(fake(OTHER_BRIDGE), 4, 0x0007, 2);
    struct fake_fn *card = fake(CARD);
    put(card, 0x10, 0x80000008, 4);
    card->masks[0x10 / 4] = 0xff000008; /* 16 MiB prefetchable */
    put(card, 0x18, 0x81010000, 4);
    card->masks[0x18 / 4] = 0xfffff000; /* BAR2 4 KiB */
    put(card, 0x24, 0x0000e001, 4);
    card->masks[0x24 / 4] = 0xffffff81; /* BAR5 I/O 128 ports */
    /* EDK2's RomDecode(FALSE) left FFFFFFFEh: reads back the 64 KiB mask. */
    put(card, 0x30, 0xffff0000, 4);
    card->masks[0x30 / 4] = 0xffff0000;
    /* PCI reset fixture: PCIe capabilities, MSI and a Resizable BAR on the card, a
     * PCIe root port with Data Link Layer Link Active reporting above it. */
    struct fake_fn *bridge = fake(BRIDGE);
    put(bridge, 6, 0x0010, 2);
    put(bridge, 0x34, BRIDGE_PCIE, 1);
    put(bridge, BRIDGE_PCIE, 0x00420010, 4);                          /* PCIe v2, root port */
    put(bridge, BRIDGE_PCIE + 0x0c, (1u << 20) | (16u << 4) | 4u, 4); /* 16 GT/s x16, DLLLARC */
    put(bridge, BRIDGE_PCIE + 0x12, 0x2044, 2);                       /* DLLLA, 16 GT/s x4 */
    put(card, 6, 0x0010, 2);
    put(card, 0x34, CARD_PCIE, 1);
    put(card, CARD_PCIE, (CARD_MSI << 8) | 0x00020010u, 4); /* PCIe v2, endpoint; next MSI */
    put(card, CARD_PCIE + 0x04, topology_no_flr ? 0x00008fc0 : 0x10008fc0,
        4);                                 /* DevCap, FLR (bit 28) */
    put(card, CARD_PCIE + 0x08, 0x2936, 2); /* DevCtl as firmware left it */
    put(card, CARD_PCIE + 0x10, 0x0042, 2); /* LnkCtl: CCC, RCB */
    put(card, CARD_PCIE + 0x28, 0x0400, 2); /* DevCtl2 */
    put(card, CARD_PCIE + 0x30, 0x0004, 2); /* LnkCtl2 */
    put(card, CARD_MSI, 0x00800005, 4);     /* MSI, 64-bit, next 0 */
    put(card, CARD_MSI + 4, 0xfee00000, 4);
    put(card, CARD_MSI + 12, 0x4021, 2);
    card->ecam = true;
    put(card, CARD_REBAR, 0x00010015, 4);     /* Resizable BAR, v1, last */
    put(card, CARD_REBAR + 4, 0x0000fff0, 4); /* 16 MiB .. 32 GiB supported */
    put(card, CARD_REBAR + 8, 0x0420, 4);     /* one BAR (index 0) at 16 MiB */
    struct fake_fn *media = fake(MEDIA);
    put(media, 0x10, 0x82000000, 4);
    media->masks[0x10 / 4] = 0xffffc000;
    put(fake(NIC), 0x10, 0x83000000, 4);
    fake(NIC)->masks[0x10 / 4] = 0xfffe0000;
    build_rom(rom_image, sizeof(rom_image));
    /* The power-on state a reset brings back: BAR addresses, ROM BAR and
     * command clear, control registers at their defaults, ReBAR at 256 MiB. */
    for (unsigned i = 0; i < nfns; ++i) {
        struct fake_fn *f = &fns[i];
        memcpy(f->defaults, f->config, sizeof(f->config));
        for (unsigned off = 0x10; off <= 0x24 && (f->config[0x0e] & 0x7f) == 0; off += 4) {
            uint32_t v = (uint32_t)f->defaults[off] | (uint32_t)f->defaults[off + 1] << 8 |
                         (uint32_t)f->defaults[off + 2] << 16 |
                         (uint32_t)f->defaults[off + 3] << 24;
            v &= v & 1 ? 3u : 0xfu;
            for (unsigned k = 0; k < 4; ++k)
                f->defaults[off + k] = (uint8_t)(v >> (k * 8));
        }
        memset(f->defaults + 0x30, 0, 4);
        f->defaults[4] = f->defaults[5] = 0;
        f->ready_at = 0;
    }
    struct fake_fn *c = fake(CARD);
    c->defaults[CARD_PCIE + 8] = 0x10;
    c->defaults[CARD_PCIE + 9] = 0x28; /* DevCtl 2810h */
    c->defaults[CARD_PCIE + 0x10] = 0;
    c->defaults[CARD_PCIE + 0x28 + 1] = 0;
    c->defaults[CARD_PCIE + 0x30] = 0;
    memset(c->defaults + CARD_MSI + 4, 0, 12);
    c->defaults[CARD_REBAR + 8] = 0x20;
    c->defaults[CARD_REBAR + 9] = 0x08; /* 256 MiB */
    link_up_at = 0;
}

/* VMX: a gen6 host layout: host bridge 00:00.0, the Sandy
 * Bridge IGD 8086:0126 at 00:02.0 without a ROM BAR (BAR0 GTTMMADR 4 MiB at
 * F0000000h, BAR2 GMADR 256 MiB at E0000000h, BAR4 I/O at 5000h, GGC with
 * IVD clear, ASLS), a PCH root port 00:1C.0 with VGA Enable set, the LPC
 * bridge 00:1F.0 and the VMM's controller at 00:03.0. */
#define IGD 0x0010u
#define ROOT_PORT 0x00e0u
#define LPC 0x00f8u
#define ASLS_VALUE 0xdae9d018u
static bool topology_igd;
static void build_igd_topology(void)
{
    static const struct {
        uint16_t bdf;
        uint32_t id, cls;
        uint8_t header;
    } list[] = {
        {HOST_BRIDGE, 0x01048086, 0x060000, 0}, {IGD, 0x01268086, 0x030000, 0},
        {MEDIA, 0x00101b36, 0x010802, 0},       {ROOT_PORT, 0x1c108086, 0x060400, 0x81},
        {LPC, 0x1c4f8086, 0x060100, 0x80},      {0x0200, 0x12348086, 0x020000, 0},
    };
    memset(fns, 0, sizeof(fns));
    nfns = sizeof(list) / sizeof(list[0]);
    for (unsigned i = 0; i < nfns; ++i) {
        struct fake_fn *f = &fns[i];
        f->bdf = list[i].bdf;
        put(f, 0, list[i].id, 4);
        put(f, 8, list[i].cls << 8, 4);
        put(f, 0x0e, list[i].header, 1);
        put(f, 4, 0x0006, 2);
    }
    put(fake(ROOT_PORT), 0x18, 0x00020200, 4);
    put(fake(ROOT_PORT), 0x3e, 0x0008, 2);
    struct fake_fn *igd = fake(IGD);
    put(igd, 0x10, 0xf0000000, 4);
    igd->masks[0x10 / 4] = 0xffc00000;
    put(igd, 0x18, 0xe0000008, 4);
    igd->masks[0x18 / 4] = 0xf0000008;
    /* BAR4: 64 I/O ports decoded on 16 address bits, bits 31:16 read 0 as
     * on gen6 IGD (its sizing once dropped the index/data pair). */
    put(igd, 0x20, 0x00005001, 4);
    igd->masks[0x20 / 4] = 0x0000ffc1;
    put(igd, 0x50, 0x0000, 2); /* GGC mirror: IVD (bit 1) clear */
    put(igd, 0xfc, ASLS_VALUE, 4);
    struct fake_fn *media = fake(MEDIA);
    put(media, 0x10, 0x82000000, 4);
    media->masks[0x10 / 4] = 0xffffc000;
    put(fake(0x0200), 0x10, 0x83000000, 4);
    fake(0x0200)->masks[0x10 / 4] = 0xfffe0000;
    for (unsigned i = 0; i < nfns; ++i) {
        memcpy(fns[i].defaults, fns[i].config, sizeof(fns[i].config));
        fns[i].ready_at = 0;
    }
    link_up_at = 0;
}

static const struct pci_host_ops fake_ops = {fake_read, fake_write, fake_map};
static void inventory(void)
{
    if (topology_igd)
        build_igd_topology();
    else
        build_topology();
    if (topology_no_vga)
        put(fake(CARD), 8, 0x02000000, 4);
    if (topology_bad_mapping)
        put(fake(CARD), 0x10, 0xe0000, 4);
    if (topology_bad_rom)
        memset(rom_image, 0xff, sizeof(rom_image));
    platform_pci_set_host(&fake_ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=00:03.0"));
    wlog_count = 0;
}
static const struct pci_device *device(uint16_t bdf)
{
    for (unsigned i = 0; i < platform_pci_count(); ++i)
        if (platform_pci_device(i)->bdf == bdf)
            return platform_pci_device(i);
    return NULL;
}

/* --- legacy image search --------------------------------------------------- */

static void test_find(void)
{
    static uint8_t rom[0x1000];
    struct vbios_image image;
    build_rom(rom, sizeof(rom));
    CHECK(vbios_find_legacy(rom, sizeof(rom), &image) && image.offset == 0x200 &&
          image.length == 0x800 && image.init_size == 0x800 && image.vendor == 0x10de &&
          image.device == 0x2f04 && image.checksum_ok);
    rom[0x200 + 5] ^= 1;
    CHECK(vbios_find_legacy(rom, sizeof(rom), &image) && !image.checksum_ok);
    build_rom(rom, sizeof(rom));
    rom[0x40 + 0x15] = 0x80; /* the data image ends the chain */
    CHECK(!vbios_find_legacy(rom, sizeof(rom), &image));
    build_rom(rom, sizeof(rom));
    rom[0x200] = 0; /* no 55AAh */
    CHECK(!vbios_find_legacy(rom, sizeof(rom), &image));
    build_rom(rom, sizeof(rom));
    rom[0x202] = 5; /* init size beyond the image */
    CHECK(!vbios_find_legacy(rom, sizeof(rom), &image));
    build_rom(rom, sizeof(rom));
    rom[0x218] = 0xf0;
    rom[0x219] = 0x0f; /* PCIR pointer beyond the ROM */
    CHECK(!vbios_find_legacy(rom, sizeof(rom), &image));
    build_rom(rom, sizeof(rom));
    CHECK(!vbios_find_legacy(rom, 0x900, &image)); /* truncated */
    /* A vendor region before the chain (an NVIDIA flash layout): the
     * chain starts at the first boundary with a valid image header; a
     * stray 55AAh without PCIR is skipped. */
    static uint8_t flash[0x2000];
    memset(flash, 0x5a, sizeof(flash));
    flash[0x200] = 0x55;
    flash[0x201] = 0xaa;
    build_rom(flash + 0x600, 0x1000);
    CHECK(vbios_find_legacy(flash, sizeof(flash), &image) && image.offset == 0x800 &&
          image.checksum_ok);
    flash[0x800] = 0; /* the legacy image's header broken: not found */
    CHECK(!vbios_find_legacy(flash, sizeof(flash), &image));
    flash[0x600] = 0; /* then no header anywhere */
    CHECK(!vbios_find_legacy(flash, sizeof(flash), &image));
    build_rom(rom, sizeof(rom));
    rom[0x40 + 0x15] = 0x80; /* a chain at 0 without type 0 ends the search */
    memcpy(flash, rom, sizeof(rom));
    build_rom(flash + 0x1000, 0x1000);
    CHECK(!vbios_find_legacy(flash, sizeof(flash), &image));
}

/* --- guest physical map and IOPM pass-through -------------------------------- */

static uint8_t low[VBIOS_LOW_SIZE] __attribute__((aligned(4096)));
static uint8_t shadow[0xa000] __attribute__((aligned(4096)));
static uint8_t stub[VBIOS_STUB_SIZE] __attribute__((aligned(4096)));

static const char *layout_problem(const struct vbios_layout *l)
{
    const struct x86rm_mapping expected[] = {
        {0, 0xa0000, low, X86RM_MAP_WRITE},
        {0xa0000, 0x20000, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
        {0xc0000, 0xa000, shadow, X86RM_MAP_WRITE},
        {0xf0000, 0x10000, stub, 0},
        {0x80000000, 0x1000000, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
        {0x81010000, 0x1000, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
    };
    if (l->map_count != sizeof(expected) / sizeof(expected[0]))
        return "mapping count";
    for (unsigned i = 0; i < l->map_count; ++i)
        if (l->map[i].gpa != expected[i].gpa || l->map[i].size != expected[i].size ||
            l->map[i].hva != expected[i].hva || l->map[i].flags != expected[i].flags)
            return "mapping";
    for (unsigned p = 0; p < 65536; ++p) {
        /* PIT channel 2, the PIT command port and 61h pass;
         * channels 0 and 1 do not. */
        bool pass = (p >= 0x3b0 && p <= 0x3df) || p == 0x80 || p == 0x42 || p == 0x43 ||
                    p == 0x61 || (p >= 0xe000 && p < 0xe080);
        if (!!(l->io_pass[p / 8] & (1u << (p & 7))) != pass)
            return "I/O pass-through";
    }
    return NULL;
}

static void test_layout(void)
{
    inventory();
    const struct pci_device *card = device(CARD);
    CHECK(card && card->bars[5].io && card->bars[5].size == 0x80 && card->bars[6].size == 0x10000);
    static struct vbios_layout l;
    CHECK(vbios_build_layout(card, low, shadow, sizeof(shadow), stub, &l));
    const char *problem = layout_problem(&l);
    if (problem)
        fprintf(stderr, "layout: %s\n", problem);
    CHECK(!problem);
    static struct vbios_layout bad;
    bad = l;
    bad.map[bad.map_count++] = (struct x86rm_mapping){0xd0000, 0x1000, NULL, X86RM_MAP_WRITE};
    CHECK(layout_problem(&bad));
    bad = l;
    bad.io_pass[0x1ce / 8] |= 1u << (0x1ce & 7);
    CHECK(layout_problem(&bad));
    bad = l;
    bad.io_pass[0x40 / 8] |= 1u << (0x40 & 7); /* PIT channel 0: the host's, never the ROM's */
    CHECK(layout_problem(&bad));
    bad = l;
    bad.io_pass[0x42 / 8] &= (uint8_t)~(1u << (0x42 & 7)); /* the ROM's timer dropped again */
    CHECK(layout_problem(&bad));
    bad = l;
    bad.map[3].flags |= X86RM_MAP_WRITE;
    CHECK(layout_problem(&bad));
    CHECK(!vbios_build_layout(card, low, shadow, 0x21000, stub, &bad));
    CHECK(!vbios_build_layout(card, low, shadow, 0x800, stub, &bad));
    /* Two sub-page BARs sharing a page become one mapping; a memory BAR
     * below 1 MiB or an I/O BAR beyond 64 KiB is refused. */
    struct pci_device small = *card;
    memset(small.bars, 0, sizeof(small.bars));
    small.bars[0] = (struct pci_bar){.base = 0x90000010, .size = 16};
    small.bars[1] = (struct pci_bar){.base = 0x90000020, .size = 16};
    CHECK(vbios_build_layout(&small, low, shadow, sizeof(shadow), stub, &bad) &&
          bad.map_count == 5 && bad.map[4].gpa == 0x90000000 && bad.map[4].size == 0x1000);
    small.bars[1].base = 0xe0000;
    CHECK(!vbios_build_layout(&small, low, shadow, sizeof(shadow), stub, &bad));
    memset(small.bars, 0, sizeof(small.bars));
    small.bars[0] = (struct pci_bar){.base = 0xfff0, .size = 0x20, .io = true};
    CHECK(!vbios_build_layout(&small, low, shadow, sizeof(shadow), stub, &bad));
}

/* --- CF8/CFC restricted to the card and its bridges ---------------------------- */

typedef uint64_t (*config_read_fn)(void *, uint32_t *, uint16_t, unsigned);
typedef void (*config_write_fn)(void *, uint32_t *, uint16_t, uint64_t, unsigned);
static uint64_t vb_read(void *c, uint32_t *l, uint16_t p, unsigned s)
{
    return vbios_config_read(c, l, p, s);
}
static void vb_write(void *c, uint32_t *l, uint16_t p, uint64_t v, unsigned s)
{
    vbios_config_write(c, l, p, v, s);
}
static uint64_t open_read(void *c, uint32_t *l, uint16_t p, unsigned s)
{
    (void)c;
    return platform_config_port_read(l, p, s, NULL, NULL);
}
static void open_write(void *c, uint32_t *l, uint16_t p, uint64_t v, unsigned s)
{
    (void)c;
    platform_config_port_write(l, p, v, s, NULL, NULL);
}

static const char *mediation_problem(struct vbios_card *card, config_read_fn rd, config_write_fn wr)
{
    uint32_t latch = 0;
    const struct {
        uint16_t bdf;
        bool visible;
        uint32_t id;
    } table[] = {
        {CARD, true, 0x2f0410de}, {BRIDGE, true, 0x14831022}, {AUDIO, false, 0}, {NIC, false, 0},
        {OTHER_BRIDGE, false, 0}, {HOST_BRIDGE, false, 0},    {MEDIA, false, 0},
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        wr(card, &latch, 0xcf8, 0x80000000u | (uint32_t)table[i].bdf << 8, 4);
        uint64_t id = rd(card, &latch, 0xcfc, 4);
        if (table[i].visible ? id != table[i].id : id != UINT32_MAX)
            return "identifier read";
        unsigned before = fake(table[i].bdf)->writes;
        wr(card, &latch, 0xcf8, 0x80000004u | (uint32_t)table[i].bdf << 8, 4);
        wr(card, &latch, 0xcfc, 0x0006, 2);
        bool reached = fake(table[i].bdf)->writes != before;
        if (!table[i].visible && reached)
            return "write reached another function";
    }
    /* The ROM sees the card's BAR as the guest will: the host value. */
    wr(card, &latch, 0xcf8, 0x80000010u | (uint32_t)CARD << 8, 4);
    if (rd(card, &latch, 0xcfc, 4) != 0x80000008)
        return "card BAR0";
    if (rd(card, &latch, 0xcf8, 4) != (0x80000010u | (uint32_t)CARD << 8))
        return "CONFIG_ADDRESS readback";
    return NULL;
}

/* QEMU docs/specs/standard-vga.rst: 1234:1111 owns hardwired VBE
 * index 1CEh and x86 data 1CFh, outside PCI I/O BARs. Both are word
 * ports, so the IOPM must pass the data word's byte at 1D0h too. */
static void test_bochs_ports(void)
{
    inventory();
    struct pci_device card = *device(CARD);
    struct vbios_layout original, bochs;
    CHECK(vbios_build_layout(&card, low, shadow, sizeof(shadow), stub, &original));
    card.vendor = 0x1234;
    card.device = 0x1111;
    CHECK(vbios_build_layout(&card, low, shadow, sizeof(shadow), stub, &bochs));
    for (unsigned p = 0; p < 65536; ++p) {
        bool expected = (p >= 0x1ce && p <= 0x1d0) || (original.io_pass[p / 8] & (1u << (p & 7)));
        CHECK(!!(bochs.io_pass[p / 8] & (1u << (p & 7))) == expected);
    }
    card.device = 0x1112;
    CHECK(vbios_build_layout(&card, low, shadow, sizeof(shadow), stub, &bochs));
    CHECK(!memcmp(original.io_pass, bochs.io_pass, sizeof(original.io_pass)));
    card.vendor = 0x80ee;
    card.device = 0xbeef;
    CHECK(platform_vga_has_dispi(&card));
    CHECK(vbios_build_layout(&card, low, shadow, sizeof(shadow), stub, &bochs));
    CHECK((bochs.io_pass[0x1ce / 8] & (1u << (0x1ce & 7))) &&
          (bochs.io_pass[0x1d0 / 8] & (1u << (0x1d0 & 7))));
    /* VMSVGA shares its PCI identity with VMware. Only a routed VGA
     * function with a readable DISPI ID owns the optional fixed ports. */
    CHECK(platform_pci_route_vga(CARD));
    card = *device(CARD);
    card.vendor = 0x15ad;
    card.device = 0x0405;
    dispi_mock = true;
    const uint16_t ids[] = {0xb0c0, 0xb0c4, 0xbe00, 0xbe01, 0xbe02,
                            0xbe03, 0,      0xffff, 0xb0c5, 0xbe04};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        dispi_index = 4;
        dispi_id = ids[i];
        dispi_reads = dispi_writes = 0;
        CHECK(vbios_build_layout(&card, low, shadow, sizeof(shadow), stub, &bochs));
        bool present = i < 6;
        for (unsigned p = 0x1ce; p <= 0x1d0; ++p)
            CHECK(!!(bochs.io_pass[p / 8] & (1u << (p & 7))) == present);
        CHECK(dispi_index == 4 && dispi_reads == 2 && dispi_writes == 2 && !rom_irqs_off);
    }
    dispi_index = 0xffff;
    dispi_reads = dispi_writes = 0;
    CHECK(!platform_vga_has_dispi(&card));
    CHECK(dispi_reads == 1 && !dispi_writes && dispi_index == 0xffff && !rom_irqs_off);
    dispi_reads = dispi_writes = 0;
    card.command &= ~1u;
    CHECK(!platform_vga_has_dispi(&card) && !dispi_reads && !dispi_writes);
    card.command |= 1;
    put(fake(BRIDGE), 0x3e, 0, 2);
    CHECK(!platform_vga_has_dispi(&card) && !dispi_reads && !dispi_writes);
    put(fake(BRIDGE), 0x3e, 8, 2);
    card.vendor = 0x10de;
    CHECK(!platform_vga_has_dispi(&card) && !dispi_reads && !dispi_writes);
    card.vendor = 0x15ad;
    card.class_code = 0x020000;
    CHECK(!platform_vga_has_dispi(&card) && !dispi_reads && !dispi_writes);
    dispi_mock = false;
}

static void test_mediation(void)
{
    inventory();
    struct vbios_card card;
    CHECK(vbios_card_from_inventory(CARD, &card));
    CHECK(card.bdf == CARD && card.bridge_count == 1 && card.bridges[0] == BRIDGE &&
          card.last_bus == 2);
    CHECK(vbios_visible(&card, CARD) && vbios_visible(&card, BRIDGE) &&
          !vbios_visible(&card, AUDIO) && !vbios_visible(&card, OTHER_BRIDGE));
    const char *problem = mediation_problem(&card, vb_read, vb_write);
    if (problem)
        fprintf(stderr, "mediation: %s\n", problem);
    CHECK(!problem);
    /* Broken variant: the guest's unrestricted view used for the ROM. */
    CHECK(mediation_problem(&card, open_read, open_write));
}

/* --- legacy VGA routing ---------------------------------------------------------- */

static const char *route_problem(void)
{
    if (!(get(BRIDGE, 0x3e, 2) & 8) || (get(BRIDGE, 4, 2) & 3) != 3)
        return "bridge above the card";
    if (get(OTHER_BRIDGE, 0x3e, 2) & 8)
        return "VGA Enable left on another path";
    if ((get(CARD, 4, 2) & 3) != 3)
        return "card decode";
    return NULL;
}
static void test_route(void)
{
    inventory();
    unsigned audio = fake(AUDIO)->writes, nic = fake(NIC)->writes;
    CHECK(route_problem());
    CHECK(platform_pci_route_vga(CARD));
    const char *problem = route_problem();
    if (problem)
        fprintf(stderr, "route: %s\n", problem);
    CHECK(!problem);
    CHECK(fake(AUDIO)->writes == audio && fake(NIC)->writes == nic);
    CHECK((device(BRIDGE)->command & 3) == 3 && (device(CARD)->command & 3) == 3);
    put(fake(OTHER_BRIDGE), 0x3e, 8, 2); /* broken variant */
    CHECK(route_problem());
}

/* --- the ROM read through a borrowed BAR --------------------------------------------- */

static void test_rom(void)
{
    inventory();
    uint64_t size = 0;
    const uint8_t *rom = platform_pci_rom(CARD, &size);
    CHECK(rom && size == 0x10000 && !memcmp(rom, rom_image, sizeof(rom_image)));
    /* RomDecode order: decode off, BAR0 to 0, ROM at BAR0's address with
     * its enable bit, memory decode on; afterwards everything restored. */
    bool off = false, moved = false, placed = false, on = false;
    for (unsigned i = 0; i < wlog_count; ++i) {
        const struct config_write *w = &wlog[i];
        if (w->bdf != CARD)
            continue;
        if (w->off == 4 && !(w->value & 7) && !moved)
            off = true;
        if (w->off == 0x10 && w->value == 0 && off)
            moved = true;
        if (w->off == 0x30 && w->value == 0x80000001 && moved)
            placed = true;
        if (w->off == 4 && (w->value & 7) == 2 && placed)
            on = true;
    }
    CHECK(off && moved && placed && on);
    CHECK(get(CARD, 0x10, 4) == 0x80000008 && get(CARD, 0x30, 4) == 0xffff0000 &&
          get(CARD, 4, 2) == 0x0006);
    unsigned writes = fake(CARD)->writes;
    CHECK(platform_pci_rom(CARD, &size) == rom && fake(CARD)->writes == writes); /* one snapshot */
    CHECK(!platform_pci_rom(MEDIA, &size)); /* reserved for the VMM */
}

/* Another enabled ROM must be isolated and restored, including on a
 * failed signature check. Restoring the target cannot hide this omission. */
static void test_rom_isolation(void)
{
    for (unsigned broken = 0; broken < 3; ++broken) {
        build_topology();
        struct fake_fn *other = fake(AUDIO);
        put(other, 0x30, 0x84000001, 4);
        other->masks[0x30 / 4] = 0xffff0000;
        if (broken)
            rom_image[0] = 0xff;
        platform_pci_set_host(&fake_ops, NULL);
        CHECK(platform_pci_inventory("vmm_media=00:03.0"));
        uint64_t size = 0;
        if (broken == 2) {
            CHECK(!platform_pci_rom(AUDIO, &size));
            CHECK(!device(AUDIO)->bars[6].size);
            build_rom(rom_image, sizeof(rom_image));
        }
        wlog_count = 0;
        CHECK((platform_pci_rom(CARD, &size) != NULL) == (broken != 1));
        bool off = false, restored = false;
        for (unsigned i = 0; i < wlog_count; ++i)
            if (wlog[i].bdf == AUDIO && wlog[i].off == 0x30) {
                if (wlog[i].value == 0x84000000)
                    off = true;
                if (off && wlog[i].value == 0x84000001)
                    restored = true;
            }
        CHECK(off && restored && get(AUDIO, 0x30, 4) == 0x84000001);
        platform_pci_cleanup();
    }
}

/* Exact writes around the copy, independent of the implementation. */
static void check_rom_order(uint16_t bdf, unsigned rom_off, uint32_t rom_original, unsigned bar_off,
                            uint32_t bar_original, bool wide, uint32_t base, uint16_t command)
{
    struct config_write writes[32];
    unsigned n = 0;
    for (unsigned i = 0; i < wlog_count; ++i)
        if (wlog[i].bdf == bdf && n < 32)
            writes[n++] = wlog[i];
    struct config_write expected[16];
    unsigned m = 0;
#define W(off_, value_) expected[m++] = (struct config_write){bdf, off_, value_}
    W(rom_off, rom_original & ~1u);
    W(4, command & ~7u);
    if (bar_off) {
        W(bar_off, 0);
        if (wide)
            W(bar_off + 4, 0);
    }
    W(rom_off, base | 1u);
    W(4, (command & ~7u) | 2u);
    W(4, command & ~7u);
    W(rom_off, rom_original);
    if (bar_off) {
        W(bar_off, bar_original);
        if (wide)
            W(bar_off + 4, 0);
    }
    W(4, command);
#undef W
    CHECK(n == m);
    for (unsigned i = 0; i < n && i < m; ++i)
        CHECK(writes[i].off == expected[i].off && writes[i].value == expected[i].value);
}
/* Default inventory uses the same decode policy without VBIOS. */
static void test_default_rom(void)
{
    static struct platform_state state;
    enum {
        MASK,
        EDK2_DISABLE,
        ENABLED,
        DISABLED,
        ZERO,
        NO_BAR,
        WIDE,
        BAR2,
        BRIDGE_ROM,
        OUTSIDE,
        DECODE_OFF,
        BAD_SIGNATURE,
        BAD_PCIR,
        FB_ONLY,
        FB_ALTERNATIVE,
        ASSIGNED_OUTSIDE,
        ANCESTOR_OFF,
        EXACT_END,
        TRUNCATED_NEXT,
        OVERSIZED_IMAGE,
        CASES
    };
    for (unsigned k = 0; k < CASES; ++k) {
        build_topology();
        struct fake_fn *card = fake(k == BRIDGE_ROM ? BRIDGE : CARD);
        uint16_t bdf = card->bdf;
        unsigned rom_off = k == BRIDGE_ROM ? 0x38 : 0x30;
        uint32_t original = k == ENABLED            ? 0x84000001
                            : k == DISABLED         ? 0x84000000
                            : k == ZERO             ? 0
                            : k == ASSIGNED_OUTSIDE ? 0x86000000
                                                    : 0xffff0000;
        put(card, rom_off, original, 4);
        card->masks[rom_off / 4] = 0xffff0000;
        if (k == EDK2_DISABLE)
            fake_write(NULL, bdf, rom_off, 0xfffffffe, 4);
        if (k == BRIDGE_ROM) {
            put(fake(CARD), 0x30, 0, 4);
            fake(CARD)->masks[0x30 / 4] = 0;
            put(card, 0x10, 0x80000000, 4);
            card->masks[4] = 0xffff0000;
        }
        if (k == NO_BAR) {
            put(card, 0x10, 0, 4);
            card->masks[4] = 0;
        }
        if (k == WIDE) {
            put(card, 0x10, 0x8000000c, 4);
            card->masks[4] = 0xff00000c;
            put(card, 0x14, 0, 4);
            card->masks[5] = UINT32_MAX;
        }
        if (k == BAR2) {
            put(card, 0x18, 0x81010000, 4);
            card->masks[6] = 0xffff0000;
        }
        if (k == FB_ALTERNATIVE) {
            /* The framebuffer BAR is smaller: avoiding it must take
             * priority over choosing the smallest candidate. */
            card->masks[4] = 0xffff0008;
            put(card, 0x18, 0x81020000, 4);
            card->masks[6] = 0xfffe0000;
        }
        if (k == OUTSIDE)
            put(card, 0x10, 0x85000008, 4);
        if (k == DECODE_OFF)
            put(card, 4, 4, 2);
        if (k == ANCESTOR_OFF)
            put(fake(BRIDGE), 4, 4, 2);
        if (k == BAD_SIGNATURE)
            memset(rom_image, 0xff, sizeof(rom_image));
        if (k == BAD_PCIR)
            rom_image[0x40] = 'X';
        /* EDK2 LoadOpRomImage bounds a complete image chain by RomSize
         * independently of the last-image marker. A further image must
         * still fit the remaining aperture in full. */
        if (k == EXACT_END || k == TRUNCATED_NEXT) {
            uint8_t *image = rom_image + 0x200;
            unsigned length = sizeof(rom_image) - 0x200;
            image[2] = image[0x50] = (uint8_t)(length / 512);
            image[0x55] = 0;
            if (k == TRUNCATED_NEXT) {
                unsigned next = sizeof(rom_image) - 512;
                memcpy(rom_image + next, image, 512);
                image[2] = image[0x50] = (uint8_t)((next - 0x200) / 512);
            }
        }
        if (k == OVERSIZED_IMAGE)
            rom_image[0x50] = sizeof(rom_image) / 512 + 1;
        for (unsigned i = 0; i < nfns; ++i)
            put(&fns[i], 6, 0, 2);
        memset(&state, 0, sizeof(state));
        platform_pci_set_host(&fake_ops, NULL);
        CHECK(platform_pci_inventory("vmm_media=00:03.0"));
        expect_fb_quiesced = k == FB_ONLY;
        if (k == FB_ONLY || k == FB_ALTERNATIVE) {
            struct boot_framebuffer fb = {
                .format = BOOT_FB_RGB_MASKS, .base = 0x80000000, .size = 0x1000000};
            platform_pci_boot_framebuffer(&fb);
        }
        bool fb_before = vmm_log_fb_enabled();
        wlog_count = flash_maps = rom_windows = 0;
        static uint8_t saved[9][4096];
        for (unsigned i = 0; i < nfns; ++i)
            memcpy(saved[i], fns[i].config, 4096);
        CHECK(platform_pci_map(&state));
        const struct pci_bar *b = &device(bdf)->bars[6];
        bool absent = k == NO_BAR || k == OUTSIDE || k == DECODE_OFF || k == ANCESTOR_OFF ||
                      k == BAD_SIGNATURE || k == BAD_PCIR || k == TRUNCATED_NEXT ||
                      k == OVERSIZED_IMAGE;
        if (absent)
            CHECK(!b->size && !b->hva);
        else {
            CHECK(b->hva && b->size == sizeof(rom_image) &&
                  !memcmp(b->hva, rom_image, sizeof(rom_image)));
        }
        bool no_read = k == NO_BAR || k == OUTSIDE || k == DECODE_OFF || k == ANCESTOR_OFF;
        CHECK(rom_windows == (no_read ? 0u : 1u) && !rom_irqs_off);
        if (!no_read) {
            bool assigned = k == ENABLED || k == DISABLED;
            bool bar2 = k == BAR2 || k == FB_ALTERNATIVE;
            check_rom_order(bdf, rom_off, original,
                            assigned ? 0
                            : bar2   ? 0x18
                                     : 0x10,
                            k == WIDE             ? 0x8000000c
                            : k == BRIDGE_ROM     ? 0x80000000
                            : k == FB_ALTERNATIVE ? 0x81020000
                            : bar2                ? 0x81010000
                                                  : 0x80000008,
                            k == WIDE,
                            assigned              ? 0x84000000
                            : k == FB_ALTERNATIVE ? 0x81020000
                            : bar2                ? 0x81010000
                                                  : 0x80000000,
                            6);
        }
        CHECK(vmm_log_fb_enabled() == fb_before && flash_maps == 0);
        for (unsigned i = 0; i < nfns; ++i)
            CHECK(!memcmp(saved[i], fns[i].config, 4096));
        const struct gm_region *flash = gm_lookup(0xffff0000);
        CHECK(!flash);
        struct ia64vmm_handoff h = {0};
        CHECK(platform_pci_handoff(&h));
        CHECK(h.pci_mmio_base + h.pci_mmio_size < UINT64_C(0x100000000));
        if (!absent && (k == ENABLED || k == DISABLED)) {
            const struct gm_region *r = gm_lookup(0x84000000);
            CHECK(r && !strcmp(r->name, "pci-rom"));
            if (k == DISABLED)
                platform_pci_write(((uint64_t)bdf << 12) + rom_off, 0x84000001, 4);
            CHECK(r && r->ops->read(r->opaque, NULL, 0, 2) == 0xaa55);
        }
        platform_pci_cleanup();
        expect_fb_quiesced = false;
    }
}

/* --- IVT and BDA --------------------------------------------------------------------- */

static void test_low(void)
{
    static uint8_t expected[0x500];
    memset(expected, 0, sizeof(expected));
    for (unsigned v = 0; v < 256; ++v) {
        unsigned ip = v == 0x10   ? 0xf065
                      : v == 0x1a ? 0xfe6e
                                  : 0xd000 + 4 * v; /* container entries */
        expected[v * 4] = (uint8_t)ip;
        expected[v * 4 + 1] = (uint8_t)(ip >> 8);
        expected[v * 4 + 2] = 0x00;
        expected[v * 4 + 3] = 0xf0;
    }
    expected[0x410] = 0x02; /* equipment: x87 */
    expected[0x413] = 0x80;
    expected[0x414] = 0x02; /* 640 KiB, no EBDA */
    expected[0x41a] = 0x1e;
    expected[0x41c] = 0x1e;
    expected[0x480] = 0x1e;
    expected[0x482] = 0x3e;
    expected[0x46c] = 0x45;
    expected[0x46d] = 0x23;
    expected[0x46e] = 0x01;
    memset(low, 0xa5, 0x600);
    vbios_build_low(low, 0x12345);
    CHECK(!memcmp(low, expected, sizeof(expected)) && low[0x500] == 0xa5);
    low[0x413] ^= 1; /* broken variant */
    CHECK(memcmp(low, expected, sizeof(expected)));
}

/* --- time ------------------------------------------------------------------------------- */

static uint64_t reference_ticks(uint64_t elapsed, uint64_t hz)
{
    return (uint64_t)((unsigned __int128)elapsed * 1193182 / ((unsigned __int128)hz * 65536));
}
static uint64_t naive_ticks(uint64_t elapsed, uint64_t hz)
{
    return elapsed / hz * 18;
}

static bool ticks_match(uint64_t (*fn)(uint64_t, uint64_t))
{
    const uint64_t hz[] = {1000000000ull, 3799987123ull, 25000000ull, 4294967311ull};
    uint64_t x = 0x9e3779b97f4a7c15ull;
    for (unsigned h = 0; h < 4; ++h)
        for (unsigned i = 0; i < 20000; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            uint64_t elapsed = i < 10 ? hz[h] * i : x % (hz[h] * 100000);
            if (fn(elapsed, hz[h]) != reference_ticks(elapsed, hz[h]))
                return false;
        }
    return true;
}
static void test_ticks(void)
{
    const uint64_t hz = 1000000000;
    CHECK(vbios_ticks(hz, hz) == 18 && vbios_ticks(10 * hz, hz) == 182 &&
          vbios_ticks(3600 * hz, hz) == 65543);
    CHECK(vbios_ticks(86400 * hz, hz) == 1573042);
    CHECK(ticks_match(vbios_ticks));
    CHECK(!ticks_match(naive_ticks)); /* broken variant */
    for (uint64_t t = 1; t < 5000; t += 7) {
        uint64_t at = vbios_tick_time(t, 3799987123ull);
        CHECK(vbios_ticks(at, 3799987123ull) >= t && vbios_ticks(at - 1, 3799987123ull) < t);
    }
    CHECK(vbios_ticks_of_day(0) == 0 && vbios_ticks_of_day(86399) == 1573024 &&
          vbios_ticks_of_day(86400) == 0);
    uint8_t bda[0x500] = {0};
    uint64_t days = 0;
    vbios_store_ticks(bda, 0x1800a0, 0x0f, &days);
    CHECK(bda[0x46c] == 0xaf && bda[0x46e] == 0x18 && !bda[0x470] && !days);
    vbios_store_ticks(bda, 0x1800a0, 0x20, &days); /* past midnight */
    CHECK(bda[0x46c] == 0x10 && bda[0x46d] == 0 && bda[0x46e] == 0 && bda[0x470] == 1 && days == 1);
    bda[0x470] = 0; /* a reader cleared it */
    vbios_store_ticks(bda, 0x1800a0, 0x21, &days);
    CHECK(!bda[0x470] && bda[0x46c] == 0x11);
}

/* --- instruction lengths without NRIPS -------------------------------------------------- */

struct length_case {
    uint8_t bytes[16];
    unsigned n;
    enum x86rm_exit_reason reason;
    int length;
};
static const struct length_case length_cases[] = {
    {{0x0f, 0xa2}, 2, X86RM_EXIT_CPUID, 2},
    {{0x66, 0x0f, 0xa2}, 3, X86RM_EXIT_CPUID, 3},
    {{0x2e, 0x66, 0x67, 0x0f, 0xa2}, 5, X86RM_EXIT_CPUID, 5},
    {{0xf4}, 1, X86RM_EXIT_HLT, 1},
    {{0x0f, 0x01, 0xd9}, 3, X86RM_EXIT_VMMCALL, 3},
    {{0x0f, 0x32}, 2, X86RM_EXIT_MSR, 2},
    {{0x66, 0x0f, 0x30}, 3, X86RM_EXIT_MSR, 3},
    {{0x0f, 0x08}, 2, X86RM_EXIT_INVD, 2},
    {{0xf3, 0x0f, 0x09}, 3, X86RM_EXIT_WBINVD, 3},
    {{0x0f, 0x32}, 2, X86RM_EXIT_CPUID, -1},
    {{0x0f}, 1, X86RM_EXIT_CPUID, -1},
    {{0x90, 0x0f, 0xa2}, 3, X86RM_EXIT_CPUID, -1},
    {{0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x0f,
      0xa2},
     16,
     X86RM_EXIT_CPUID,
     -1},
};
static bool lengths_match(bool (*fn)(const uint8_t *, unsigned, enum x86rm_exit_reason, unsigned *))
{
    for (unsigned i = 0; i < sizeof(length_cases) / sizeof(length_cases[0]); ++i) {
        unsigned got = 0;
        bool ok = fn(length_cases[i].bytes, length_cases[i].n, length_cases[i].reason, &got);
        if (length_cases[i].length < 0 ? ok : !ok || got != (unsigned)length_cases[i].length)
            return false;
    }
    return true;
}
static bool broken_length(const uint8_t *code, unsigned avail, enum x86rm_exit_reason reason,
                          unsigned *length)
{
    /* Ignores prefixes: the opcode alone. */
    unsigned i = 0;
    while (i < avail && (code[i] == 0x66 || code[i] == 0x2e || code[i] == 0x67 || code[i] == 0xf3))
        ++i;
    bool ok = vbios_insn_length(code + i, avail - i, reason, length);
    return ok;
}
static void test_lengths(void)
{
    CHECK(lengths_match(vbios_insn_length));
    CHECK(!lengths_match(broken_length));
}

/* --- int 1Ah PCI BIOS -------------------------------------------------------------------- */

static unsigned call1a(struct vbios_card *card, struct x86rm_regs *r, uint64_t ax, uint64_t bx,
                       uint64_t cx, uint64_t dx, uint64_t si, uint64_t di)
{
    memset(r, 0, sizeof(*r));
    r->rax = ax;
    r->rbx = bx;
    r->rcx = cx;
    r->rdx = dx;
    r->rsi = si;
    r->rdi = di;
    return vbios_int1a(card, r);
}
static void test_int1a(void)
{
    inventory();
    struct vbios_card card;
    CHECK(vbios_card_from_inventory(CARD, &card));
    struct x86rm_regs r;
    CHECK(call1a(&card, &r, 0xb101, 0, 0xff00, 0, 0, 0xffffffff) == 1);
    CHECK((r.rax & 0xffff) == 0x0001 && (r.rbx & 0xffff) == 0x0300 && (r.rcx & 0xffff) == 0xff02 &&
          (uint32_t)r.rdx == 0x20494350 && (uint32_t)r.rdi == 0);
    CHECK(call1a(&card, &r, 0xb102, 0, 0x2f04, 0x10de, 0, 0) == 1 && (r.rbx & 0xffff) == CARD &&
          !(r.rax & 0xff00));
    CHECK(call1a(&card, &r, 0xb102, 0, 0x2f04, 0x10de, 1, 0) == 2 && (r.rax & 0xff00) == 0x8600);
    CHECK(call1a(&card, &r, 0xb102, 0, 0x22e8, 0x10de, 0, 0) == 2 &&
          (r.rax & 0xff00) == 0x8600); /* not visible */
    CHECK(call1a(&card, &r, 0xb102, 0, 0x1234, 0xffff, 0, 0) == 2 && (r.rax & 0xff00) == 0x8300);
    CHECK(call1a(&card, &r, 0xb103, 0, 0x060400, 0, 0, 0) == 1 && (r.rbx & 0xffff) == BRIDGE);
    CHECK(call1a(&card, &r, 0xb103, 0, 0x060400, 0, 1, 0) == 2); /* the other bridge is invisible */
    CHECK(call1a(&card, &r, 0xb103, 0, 0x030000, 0, 0, 0) == 1 && (r.rbx & 0xffff) == CARD);
    CHECK(call1a(&card, &r, 0xb108, CARD, 0xff00, 0, 0, 0) == 1 && (r.rcx & 0xffff) == 0xffde);
    CHECK(call1a(&card, &r, 0xb109, CARD, 0, 0, 0, 2) == 1 && (r.rcx & 0xffff) == 0x2f04);
    CHECK(call1a(&card, &r, 0xb10a, CARD, 0, 0, 0, 0x10) == 1 && (uint32_t)r.rcx == 0x80000008);
    CHECK(call1a(&card, &r, 0xb10a, NIC, 0, 0, 0, 0) == 1 && (uint32_t)r.rcx == UINT32_MAX);
    CHECK(call1a(&card, &r, 0xb109, CARD, 0, 0, 0, 1) == 2 && (r.rax & 0xff00) == 0x8700);
    CHECK(call1a(&card, &r, 0xb10a, CARD, 0, 0, 0, 0x100) == 2 && (r.rax & 0xff00) == 0x8700);
    unsigned card_writes = fake(CARD)->writes, nic_writes = fake(NIC)->writes;
    CHECK(call1a(&card, &r, 0xb10c, CARD, 0x0007, 0, 0, 4) == 1 &&
          fake(CARD)->writes > card_writes);
    CHECK(call1a(&card, &r, 0xb10c, NIC, 0x0007, 0, 0, 4) == 1 && fake(NIC)->writes == nic_writes);
    CHECK(call1a(&card, &r, 0xb1ff, 0, 0, 0, 0, 0) == 2 && (r.rax & 0xff00) == 0x8100);
    CHECK(call1a(&card, &r, 0x0000, 1, 2, 3, 4, 5) == 0 && r.rax == 0 && r.rbx == 1 && r.rdx == 3);
}

/* --- VMX: the container's vectors ----------------------------------------------------- */

static uint32_t ivt_word(const uint8_t *ivt, unsigned n)
{
    return (uint32_t)(ivt[n * 4 + 3] << 24 | ivt[n * 4 + 2] << 16 | ivt[n * 4 + 1] << 8 |
                      ivt[n * 4]);
}
typedef unsigned (*vector_fn)(unsigned, bool, const uint8_t *, struct x86rm_regs *);
/* NULL when every call answers as the per-vector table says. */
static const char *vectors_problem(vector_fn fn)
{
    static uint8_t bda[0x500];
    vbios_build_low(bda, 0);
    bda[0x410] = 0x27;
    bda[0x411] = 0x44;
    struct x86rm_regs r;
    const struct {
        unsigned vector;
        bool post;
        uint32_t ax, cx;
        unsigned result;
        uint32_t ax_after, cx_after;
    } cases[] = {
        {0x15, false, 0x8600, 0x0001, 2, 0x8600, 0x0001}, /* AH=86h, CF=1 */
        {0x15, false, 0xe820, 0x0014, 2, 0x8620, 0x0014}, /* AH only */
        {0x15, false, 0x5f34, 0x1234, 2, 0x5f34, 0x1234}, /* mode set: no 5Fxx answered */
        {0x15, false, 0x5f51, 0x1234, 2, 0x5f51, 0x1234},
        {0x15, true, 0x5f34, 0x1234, 1, 0x005f, 0x0000}, /* POST: coreboot's answers */
        {0x15, true, 0x5f35, 0x1234, 1, 0x005f, 0x0000},
        {0x15, true, 0x5f51, 0x1234, 1, 0x005f, 0x0001},
        {0x15, true, 0x5f70, 0x0200, 1, 0x005f, 0x0000},
        {0x15, true, 0x5f70, 0x0300, 2, 0x5f70, 0x0300}, /* CH=3: not a coreboot function */
        {0x15, true, 0x5f40, 0x1234, 2, 0x5f40, 0x1234}, /* panel type: never answered */
        {0x15, true, 0x5f14, 0x1234, 2, 0x5f14, 0x1234},
        {0x11, false, 0xffff, 0x1234, 0, 0x4427, 0x1234},
        {0x12, true, 0xffff, 0x1234, 0, 0x0280, 0x1234}, /* 640 KiB */
        {0x13, false, 0x0201, 0x0001, 2, 0x0201, 0x0001},
        {0x14, false, 0x0300, 0, 2, 0x0300, 0},
        {0x16, false, 0x0000, 0, 2, 0x0000, 0},
        {0x17, false, 0x0200, 0, 2, 0x0200, 0},
        {0x10, false, 0x0e41, 0, 0, 0x0e41, 0},
        {0x08, false, 0x1111, 0x2222, 0, 0x1111, 0x2222},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(&r, 0, sizeof(r));
        r.rax = 0xabcd0000u | cases[i].ax;
        r.rbx = 0x5555;
        r.rcx = cases[i].cx;
        r.rdx = 0x7777;
        unsigned result = fn(cases[i].vector, cases[i].post, bda, &r);
        if (result != cases[i].result || r.rax != (0xabcd0000u | cases[i].ax_after) ||
            r.rcx != cases[i].cx_after || r.rbx != 0x5555 || r.rdx != 0x7777)
            return "vector answer";
    }
    return NULL;
}
/* Broken copy: the coreboot answers given under the mode set as well. */
static unsigned vector_always_hooks(unsigned vector, bool post, const uint8_t *low,
                                    struct x86rm_regs *r)
{
    (void)post;
    return vbios_vector(vector, true, low, r);
}
/* Broken copy: int 15h left as the bare IRET did. */
static unsigned vector_bare_15h(unsigned vector, bool post, const uint8_t *low,
                                struct x86rm_regs *r)
{
    return vector == 0x15 && ((r->rax >> 8) & 0xff) != 0x5f ? 0
                                                            : vbios_vector(vector, post, low, r);
}

/* A synthetic gen6 host snapshot: a POSTed
 * 64 KiB Intel image at C0000 (PCIR 8086:0106, a non-zero sum), the CSM's
 * vectors in F000h/E000h, data pointers into the image, an EBDA. */
static uint8_t host_low[VBIOS_LOW_SIZE], host_option[0x20000];
static void build_host_snapshot(void)
{
    memset(host_low, 0, sizeof(host_low));
    memset(host_option, 0xff, sizeof(host_option));
    uint8_t *image = host_option;
    memset(image, 0, 0x10000);
    image[0] = 0x55;
    image[1] = 0xaa;
    image[2] = 0x80;
    image[3] = 0xe9;
    image[4] = 0x5d;
    image[5] = 0x01; /* jmp c000:0163 */
    image[0x18] = 0x40;
    uint8_t *pcir = image + 0x40;
    memcpy(pcir, "PCIR", 4);
    pcir[4] = 0x86;
    pcir[5] = 0x80;
    pcir[6] = 0x06;
    pcir[7] = 0x01;
    pcir[0x0a] = 0x18;
    pcir[0x0f] = 0x03;
    pcir[0x10] = 0x80;
    pcir[0x14] = 0;
    pcir[0x15] = 0x80;
    image[0x14] = 0xcf;
    image[0x2000] = 0x5a; /* the sum is not zero */
    const struct {
        unsigned v;
        uint16_t cs, ip;
    } ivt[] = {
        {0x10, 0xc000, 0x0014}, {0x15, 0xf000, 0xf859},
        {0x1a, 0xf000, 0xfe6e}, {0x13, 0xe000, 0x1234},
        {0x1f, 0xc000, 0x5000}, {0x43, 0xc000, 0x6000},
        {0x6d, 0xc000, 0x0100}, {0x42, 0xf000, 0xf065},
        {0x40, 0xd000, 0x0000}, /* past the 64 KiB image: redirected */
    };
    for (unsigned v = 0; v < 256; ++v) {
        host_low[v * 4] = 0x53;
        host_low[v * 4 + 1] = 0xff;
        host_low[v * 4 + 3] = 0xf0;
    }
    host_low[0x08 * 4] = host_low[0x08 * 4 + 1] = host_low[0x08 * 4 + 2] = host_low[0x08 * 4 + 3] =
        0; /* null */
    for (unsigned i = 0; i < sizeof(ivt) / sizeof(ivt[0]); ++i) {
        uint8_t *e = host_low + ivt[i].v * 4;
        e[0] = (uint8_t)ivt[i].ip;
        e[1] = (uint8_t)(ivt[i].ip >> 8);
        e[2] = (uint8_t)ivt[i].cs;
        e[3] = (uint8_t)(ivt[i].cs >> 8);
    }
    host_low[0x40e] = 0xc0;
    host_low[0x40f] = 0x9f; /* EBDA at 9FC0h */
    host_low[0x410] = 0x27;
    host_low[0x411] = 0x44;
    host_low[0x413] = 0x7f;
    host_low[0x414] = 0x02; /* 639 KiB */
    host_low[0x449] = 0x12;
    host_low[0x44a] = 80;     /* a VGA graphics mode */
    host_low[0x9fc00] = 0x01; /* EBDA size, KiB */
}

/* A classic platform VGA ROM header: 32 KiB, an initialization entry at
 * +3, IBM at +1Eh, and no PCI data structure. The IVT is uninitialized. */
static void build_classic_snapshot(void)
{
    memset(host_low, 0, sizeof(host_low));
    memset(host_option, 0, sizeof(host_option));
    memcpy(host_option, "\x55\xaa\x40\xeb\x1d", 5);
    memcpy(host_option + 0x1e, "IBM", 3);
    uint8_t sum = 0;
    for (unsigned i = 0; i < 0x8000; ++i)
        sum += host_option[i];
    host_option[0x7fff] -= sum;
}

static void test_host_rom(void)
{
    topology_igd = true;
    inventory();
    const struct pci_device *card = device(IGD);
    struct vbios_image image;
    build_classic_snapshot();
    CHECK(!vbios_host_rom(host_option, sizeof(host_option), card, &image));
    CHECK(image.offset == 0 && image.length == 0x8000 && image.init_size == 0x8000 &&
          image.checksum_ok && !image.vendor && !image.device && !image.class_code &&
          !image.revision);
    CHECK(!vbios_find_legacy(host_option, sizeof(host_option), &image));
    CHECK(vbios_host_image(host_option, sizeof(host_option), host_low, sizeof(host_low), card,
                           &image));
    host_option[6] ^= 1;
    CHECK(vbios_host_rom(host_option, sizeof(host_option), card, &image));
    build_classic_snapshot();
    CHECK(vbios_host_rom(host_option, 0x7fff, card, &image));
    host_option[2] = 0;
    CHECK(vbios_host_rom(host_option, sizeof(host_option), card, &image));
    build_classic_snapshot();
    host_option[0] = 0;
    CHECK(vbios_host_rom(host_option, sizeof(host_option), card, &image));
    build_classic_snapshot();
    host_option[0x18] = 0x40; /* A declared but invalid PCI structure is not classic. */
    host_option[0x7fff] -= 0x40;
    CHECK(vbios_host_rom(host_option, sizeof(host_option), card, &image));
    host_option[0x18] = 0xf0;
    host_option[0x19] = 0xff;
    CHECK(vbios_host_rom(host_option, sizeof(host_option), card, &image));
    build_host_snapshot();
    memset(host_low + 0x40, 0, 4);
    CHECK(!vbios_host_rom(host_option, sizeof(host_option), card, &image));
    CHECK(vbios_host_image(host_option, sizeof(host_option), host_low, sizeof(host_low), card,
                           &image));
    /* A compatibility INT10 service inside a classic image does not
     * establish the initialized VGA state needed by the live-mode path. */
    build_classic_snapshot();
    host_low[0x40] = 7;
    host_low[0x43] = 0xc0;
    CHECK(vbios_host_image(host_option, sizeof(host_option), host_low, sizeof(host_low), card,
                           &image));
    topology_igd = false;
}

static void test_vmx_services(void)
{
    const char *problem = vectors_problem(vbios_vector);
    if (problem)
        fprintf(stderr, "vectors: %s\n", problem);
    CHECK(!problem);
    CHECK(vectors_problem(vector_always_hooks));
    CHECK(vectors_problem(vector_bare_15h));
    /* Host image acceptance against the IGD. */
    topology_igd = true;
    inventory();
    const struct pci_device *igd = device(IGD), *nic = device(0x0200);
    CHECK(igd && !igd->bars[6].size && igd->bars[4].io && nic);
    build_host_snapshot();
    struct vbios_image image;
    CHECK(!vbios_host_image(host_option, sizeof(host_option), host_low, sizeof(host_low), igd,
                            &image));
    CHECK(image.offset == 0 && image.init_size == 0x10000 && image.vendor == 0x8086 &&
          image.device == 0x0106 && image.class_code == 0x030000 && !image.checksum_ok &&
          image.checksum);
    host_option[0x4f] = 0x02; /* a network image */
    CHECK(vbios_host_image(host_option, sizeof(host_option), host_low, sizeof(host_low), igd,
                           &image));
    build_host_snapshot();
    host_low[0x40] = 0x00;
    host_low[0x42] = 0x00;
    host_low[0x43] = 0xd0; /* int 10h past the image */
    CHECK(vbios_host_image(host_option, sizeof(host_option), host_low, sizeof(host_low), igd,
                           &image));
    build_host_snapshot();
    host_option[0x44] = 0xde;
    host_option[0x45] = 0x10; /* an NVIDIA image */
    CHECK(vbios_host_image(host_option, sizeof(host_option), host_low, sizeof(host_low), igd,
                           &image));
    build_host_snapshot();
    host_option[0] = 0; /* no image at C0000 */
    CHECK(vbios_host_image(host_option, sizeof(host_option), host_low, sizeof(host_low), igd,
                           &image));
    CHECK(vbios_host_image(NULL, 0, host_low, sizeof(host_low), igd, &image));
    build_host_snapshot();
    /* The IVT copy: entries into the image stay, every other one goes to
     * the container's entry for its vector. */
    static uint8_t ivt[0x400];
    memcpy(ivt, host_low, sizeof(ivt));
    CHECK(vbios_host_ivt(ivt, 0x10000) == 252);
    bool right = true;
    for (unsigned v = 0; v < 256; ++v) {
        bool kept = v == 0x10 || v == 0x1f || v == 0x43 || v == 0x6d;
        uint32_t want = kept ? ivt_word(host_low, v) : 0xf0000000u | (0xd000 + 4 * v);
        right &= ivt_word(ivt, v) == want;
    }
    CHECK(right);
    ivt[0x15 * 4] = 0x59;
    ivt[0x15 * 4 + 1] = 0xf8; /* broken: a CSM vector kept */
    right = true;
    for (unsigned v = 0; v < 256 && right; ++v)
        right = (v == 0x10 || v == 0x1f || v == 0x43 || v == 0x6d) ||
                ivt_word(ivt, v) == (0xf0000000u | (0xd000 + 4 * v));
    CHECK(!right);
    /* The IGD's visibility: 00:00.0 and 00:1F.0 readable, never written;
     * the root port and the NIC invisible. */
    struct vbios_card card;
    CHECK(vbios_card_from_inventory(IGD, &card) && card.readonly_count == 2 && !card.bridge_count);
    CHECK(vbios_visible(&card, HOST_BRIDGE) && vbios_visible(&card, LPC) &&
          !vbios_writable(&card, HOST_BRIDGE) && !vbios_writable(&card, LPC) &&
          vbios_writable(&card, IGD) && !vbios_visible(&card, ROOT_PORT) &&
          !vbios_visible(&card, 0x0200));
    uint32_t latch = 0;
    vbios_config_write(&card, &latch, 0xcf8, 0x80000000u | LPC << 8, 4);
    CHECK(vbios_config_read(&card, &latch, 0xcfc, 4) == 0x1c4f8086);
    /* The host bridge is the VMM's (host-system): the guest's mediation
     * reads it as absent, the VBIOS reads the host's registers. */
    vbios_config_write(&card, &latch, 0xcf8, 0x80000000u | HOST_BRIDGE << 8, 4);
    CHECK(device(HOST_BRIDGE)->owner == PCI_OWNER_VMM &&
          vbios_config_read(&card, &latch, 0xcfc, 4) == 0x01048086);
    CHECK(platform_config_port_read(&latch, 0xcfc, 4, vbios_visible, &card) ==
          UINT32_MAX); /* broken: mediated */
    CHECK(vbios_config_read(&card, &latch, 0xcfe, 2) == 0x0104);
    unsigned host_writes = fake(HOST_BRIDGE)->writes;
    vbios_config_write(&card, &latch, 0xcf8, 0x80000004u | HOST_BRIDGE << 8, 4);
    vbios_config_write(&card, &latch, 0xcfc, 0x0007, 2); /* command: dropped */
    CHECK(fake(HOST_BRIDGE)->writes == host_writes);
    unsigned lpc_writes = fake(LPC)->writes;
    vbios_config_write(&card, &latch, 0xcf8, 0x80000004u | LPC << 8, 4);
    vbios_config_write(&card, &latch, 0xcfc, 0x0007, 2);
    CHECK(fake(LPC)->writes == lpc_writes);
    platform_config_port_write(&latch, 0xcfc, 0x0007, 2, vbios_visible,
                               &card); /* broken: the read filter */
    CHECK(fake(LPC)->writes != lpc_writes);
    vbios_config_write(&card, &latch, 0xcf8, 0x80000000u | ROOT_PORT << 8, 4);
    CHECK(vbios_config_read(&card, &latch, 0xcfc, 4) == UINT32_MAX);
    struct x86rm_regs r;
    CHECK(call1a(&card, &r, 0xb10c, HOST_BRIDGE, 0x0007, 0, 0, 4) == 1 &&
          fake(HOST_BRIDGE)->writes == host_writes);
    CHECK(call1a(&card, &r, 0xb10a, LPC, 0, 0, 0, 0) == 1 && (uint32_t)r.rcx == 0x1c4f8086);
    CHECK(call1a(&card, &r, 0xb10a, HOST_BRIDGE, 0, 0, 0, 0) == 1 && (uint32_t)r.rcx == 0x01048086);
    /* A discrete card keeps the old rule: nothing read-only. */
    topology_igd = false;
    inventory();
    CHECK(vbios_card_from_inventory(CARD, &card) && !card.readonly_count);
    /* The OpRegion: only over ACPI NVS or reserved memory. */
    static struct boot_info bi;
    memset(&bi, 0, sizeof(bi));
    bi.mem_range_count = 3;
    bi.mem[0] = (struct boot_mem_range){0x100000, 0xdad00000 - 0x100000, BOOT_MEM_USABLE, NULL};
    bi.mem[1] = (struct boot_mem_range){0xdae00000, 0x9d000, BOOT_MEM_RESERVED, NULL};
    bi.mem[2] = (struct boot_mem_range){0xdae9d000, 0x63000, BOOT_MEM_ACPI_NVS, NULL};
    uint64_t base = 0, size = 0;
    CHECK(!vbios_opregion_range(&bi, ASLS_VALUE, &base, &size) && base == 0xdae9d000 &&
          size == 0x3000);
    CHECK(!vbios_opregion_range(&bi, 0xdae9c000, &base, &size) && base == 0xdae9c000 &&
          size == 0x2000);                                      /* reserved+NVS */
    CHECK(vbios_opregion_range(&bi, 0xdacff000, &base, &size)); /* usable RAM */
    CHECK(vbios_opregion_range(&bi, 0xdad00000, &base, &size)); /* a hole */
    CHECK(vbios_opregion_range(&bi, 0, &base, &size));
    bi.mem[2].type = BOOT_MEM_VMM;
    CHECK(vbios_opregion_range(&bi, ASLS_VALUE, &base, &size));
    static struct vbios_layout l;
    memset(&l, 0, sizeof(l));
    CHECK(vbios_add_mapping(&l, 0xdae9d000, 0x3000, X86RM_MAP_WRITE) && l.map_count == 1 &&
          l.map[0].hva == NULL && l.map[0].flags == X86RM_MAP_WRITE);
    CHECK(!vbios_add_mapping(&l, 0xdae9f000, 0x1000, X86RM_MAP_WRITE)); /* overlap */
    /* The snapshot lines. */
    build_host_snapshot();
    char line[256];
    platform_legacy_snapshot_describe(line, sizeof(line), 0, host_low, sizeof(host_low),
                                      host_option, sizeof(host_option));
    CHECK(strstr(line, "40E=9fc0 (EBDA) 413=639 KiB 449=12 44A=80"));
    platform_legacy_snapshot_describe(line, sizeof(line), 1, host_low, sizeof(host_low),
                                      host_option, sizeof(host_option));
    CHECK(strstr(
        line,
        "10=c000:0014(image) 15=f000:f859(redirected) 1A=f000:fe6e(redirected) 1F=c000:5000(image)"));
    platform_legacy_snapshot_describe(line, sizeof(line), 2, host_low, sizeof(host_low),
                                      host_option, sizeof(host_option));
    CHECK(strstr(line, " 55 aa 80 e9 5d 01") && strstr(line, "+3 jmp c000:0163"));
}

/* --- the whole flow through a scripted container ------------------------------------------------ */

enum scenario {
    SCRIPT_POST,
    SCRIPT_NPF,
    SCRIPT_INTR,
    SCRIPT_CLI,
    SCRIPT_FATAL,
    SCRIPT_TRACE,
    SCRIPT_LIVE,
    SCRIPT_LIVE_FAIL,
    SCRIPT_HOST_POST,
    SCRIPT_LIVE_HOLD,
    SCRIPT_RECLAIM,
    SCRIPT_MODE_FAIL,
    SCRIPT_MODE_NPF
};
static struct x86rm_exit fatal_exit;
static uint64_t fatal_flags;
static enum scenario scenario;
static unsigned step, creates, destroys, consoles;
static bool backend_unavailable, container_unavailable, console_unavailable;
static uint8_t *guest_low, *guest_shadow, *guest_stub;
static unsigned guest_maps;
static struct x86rm_mapping guest_map[VBIOS_MAX_MAPS];
static const char *script_error;

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
bool __wrap_hal_x86rm_probe(const char **why)
{
    if (backend_unavailable) {
        *why = "test unavailable";
        return false;
    }
    return true;
}
/* The fake backend: SVM (VMMCALL) by default; VMX runs report VMX and
 * VMCALL. */
static const struct x86rm_info fake_svm = {"SVM", {0x0f, 0x01, 0xd9}},
                               fake_vmx = {"VMX", {0x0f, 0x01, 0xc1}};
static const struct x86rm_info *fake_info = &fake_svm;
const struct x86rm_info *__wrap_hal_x86rm_info(void)
{
    return fake_info;
}
static uint8_t guest_io_pass[8192];
/* PCI reset fixture: the card's BAR5 I/O window behind the wrapped port accessors
 * (NVIDIA layout: key/enable at +0/+4, index +8, data +0Ch; PTIMER_TIME_0
 * at 9400h advances 1000h per read). Every forwarded access is logged. */
struct port_call {
    uint16_t port;
    uint32_t value;
    unsigned size;
    bool out;
};
static struct port_call port_calls[64];
static unsigned port_call_count;
static uint32_t window_index, ptimer;
uint32_t __real_hal_port_in(uint16_t port, unsigned size);
void __real_hal_port_out(uint16_t port, uint32_t value, unsigned size);
uint32_t __wrap_hal_port_in(uint16_t port, unsigned size)
{
    if (dispi_mock && (port == 0x1ce || port == 0x1cf)) {
        CHECK(size == 2 && rom_irqs_off);
        ++dispi_reads;
        if (port == 0x1cf)
            CHECK(!dispi_index);
        return port == 0x1ce ? dispi_index : dispi_id;
    }
    if (port < 0xe000 || port >= 0xe080)
        return __real_hal_port_in(port, size);
    uint32_t value = port == 0xe00c && window_index == 0x9400 ? (ptimer += 0x1000) : 0x5a5a5a5au;
    if (port_call_count < 64)
        port_calls[port_call_count++] = (struct port_call){port, value, size, false};
    return value;
}
void __wrap_hal_port_out(uint16_t port, uint32_t value, unsigned size)
{
    if (dispi_mock && (port == 0x1ce || port == 0x1cf)) {
        CHECK(size == 2 && rom_irqs_off && port == 0x1ce);
        ++dispi_writes;
        dispi_index = (uint16_t)value;
        return;
    }
    if (port < 0xe000 || port >= 0xe080) {
        __real_hal_port_out(port, value, size);
        return;
    }
    if (port == 0xe008)
        window_index = value;
    if (port_call_count < 64)
        port_calls[port_call_count++] = (struct port_call){port, value, size, true};
}
bool __wrap_hal_x86rm_create(const struct x86rm_config *cfg, struct x86rm **out, const char **why)
{
    ++creates;
    if (backend_unavailable || container_unavailable) {
        *why = "test container unavailable";
        return false;
    }
    memcpy(guest_io_pass, cfg->io_pass, sizeof(guest_io_pass));
    guest_maps = cfg->map_count;
    memcpy(guest_map, cfg->map, cfg->map_count * sizeof(cfg->map[0]));
    guest_low = cfg->map[0].hva;
    guest_shadow = cfg->map[2].hva;
    guest_stub = cfg->map[3].hva;
    *out = (struct x86rm *)&step;
    return true;
}
static bool reclaim_unavailable, reclaim_call_failed, reclaim_capture;
void __real_hal_checkpoint(const char *label);
void __wrap_hal_checkpoint(const char *label)
{
    if (reclaim_capture && !strncmp(label, "vbios-trace-", 12))
        CHECK(!route_problem());
    __real_hal_checkpoint(label);
}
bool __wrap_hal_x86rm_reclaim_create(const struct x86rm_config *cfg, struct x86rm **out,
                                     const char **why)
{
    if (reclaim_unavailable) {
        *why = "test unavailable";
        return false;
    }
    return __wrap_hal_x86rm_create(cfg, out, why);
}
void __wrap_hal_x86rm_destroy(struct x86rm *rm)
{
    (void)rm;
    ++destroys;
}
bool __wrap_hal_console_vga_text(void)
{
    ++consoles;
    return !console_unavailable;
}

static void at(struct x86rm_regs *r, uint16_t cs, uint64_t ip)
{
    r->cs = (struct x86rm_segment){cs, 0x9b, 0xffff, (uint64_t)cs << 4};
    r->rip = ip;
}
#define EXPECT(e)                                                                                  \
    do {                                                                                           \
        if (!(e) && !script_error)                                                                 \
            script_error = #e;                                                                     \
    } while (0)

/* SeaBIOS INT10 uses farcall16 (stacks.c), unlike __callrom. */
static void check_small_segments(const struct x86rm_regs *r)
{
    const struct x86rm_segment *segments[] = {&r->cs, &r->ss, &r->ds, &r->es, &r->fs, &r->gs};
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(segments[i]->limit == 0xffff);
        CHECK(segments[i]->attrib == (i ? 0x93 : 0x9b));
    }
}
static uint8_t reclaim_bda[256];
static void post_script(struct x86rm_regs *r, struct x86rm_exit *e)
{
    switch (step) {
    case 0: /* the POST call registers */
        EXPECT(r->cs.selector == 0xf000 && r->cs.base == 0xf0000 && r->rip == 0xe000);
        EXPECT(r->rax == CARD && r->rbx == 0xffff && r->rdx == 0xffff && r->es.selector == 0xf000 &&
               r->rdi == 0);
        EXPECT(r->ss.selector == 0 && r->rsp == 0x7c00 && r->rflags == 0x202 && r->cr0 == 0x10);
        /* SeaBIOS optionroms.c uses farcall16big; misc.c supplies 4-GiB
         * code/data limits with G=1 and D/B=0 for that entry. */
        EXPECT(r->cs.limit == UINT32_MAX && r->ss.limit == UINT32_MAX &&
               r->ds.limit == UINT32_MAX && r->es.limit == UINT32_MAX &&
               r->fs.limit == UINT32_MAX && r->gs.limit == UINT32_MAX);
        EXPECT(r->cs.attrib == 0x89b && r->ss.attrib == 0x893 && r->ds.attrib == 0x893 &&
               r->es.attrib == 0x893);
        EXPECT(guest_low[0x40] == 0x65 && guest_low[0x41] == 0xf0 && guest_low[0x42] == 0 &&
               guest_low[0x43] == 0xf0);
        EXPECT(guest_stub[0xe000] == 0x9a && guest_stub[0xfe6e] == 0x0f && guest_maps == 6);
        /* The stubs end with the backend's hypercall. */
        EXPECT(!memcmp(guest_stub + 0xe005, fake_info->hypercall, 3) &&
               !memcmp(guest_stub + 0xe015, fake_info->hypercall, 3) &&
               !memcmp(guest_stub + 0xfe6e, fake_info->hypercall, 3) && guest_stub[0xfe71] == 0xcf);
        at(r, 0xc000, 0x100); /* inside the ROM now: out dx(CF8), eax */
        r->rax = 0x80000000u | (uint32_t)CARD << 8;
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_IO,
                                 .port = 0xcf8,
                                 .size = 4,
                                 .next_rip = 0x101,
                                 .next_rip_valid = true};
        break;
    case 1:
        EXPECT(r->rip == 0x101);
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_IO,
                                 .port = 0xcfc,
                                 .size = 4,
                                 .in = true,
                                 .next_rip = 0x102,
                                 .next_rip_valid = true};
        break;
    case 2:
        EXPECT((uint32_t)r->rax == 0x2f0410de && r->rip == 0x102);
        r->rax = 0x12340000;
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_IO,
                                 .port = 0x1ce,
                                 .size = 2,
                                 .in = true,
                                 .next_rip = 0x104,
                                 .next_rip_valid = true};
        break;
    case 3: /* denied: all-ones in AX only; then CPUID without NRIPS */
        EXPECT(r->rax == 0x1234ffff);
        guest_low[0x7000] = 0x66;
        guest_low[0x7001] = 0x0f;
        guest_low[0x7002] = 0xa2;
        at(r, 0x0700, 0);
        r->rax = 0;
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_CPUID, .cpuid = {1, 2, 3, 4}};
        break;
    case 4: /* then int 1Ah B101h: IP, CS, FLAGS(CF=1) on the stack */
        EXPECT(r->rip == 3 && r->rax == 1 && r->rbx == 2 && r->rcx == 3 && r->rdx == 4);
        r->rsp = 0x7bf0;
        guest_low[0x7bf0] = 0x03;
        guest_low[0x7bf1] = 0x00;
        guest_low[0x7bf2] = 0x00;
        guest_low[0x7bf3] = 0x07;
        guest_low[0x7bf4] = 0x03;
        guest_low[0x7bf5] = 0x02;
        at(r, 0xf000, 0xfe6e);
        r->rax = 0xb101;
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_VMMCALL};
        break;
    case 5:
        EXPECT(r->rip == 0xfe71 && (uint32_t)r->rdx == 0x20494350 && (r->rax & 0xffff) == 1 &&
               (r->rbx & 0xffff) == 0x300);
        EXPECT(guest_low[0x7bf4] == 0x02); /* CF cleared in the FLAGS image */
        at(r, 0xf000, 0xfe6e);
        r->rax = 0xb102;
        r->rcx = 0x22e8;
        r->rdx = 0x10de;
        r->rsi = 0; /* the audio function */
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_VMMCALL};
        break;
    case 6: /* not visible: CF set; then HLT with IF set */
        EXPECT(guest_low[0x7bf4] == 0x03 && (r->rax & 0xff00) == 0x8600);
        guest_low[0x7000] = 0xf4;
        at(r, 0x0700, 0);
        r->rflags = 0x202;
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_HLT};
        break;
    case 7: /* then RDMSR: denied, reads 0 */
        EXPECT(r->rip == 1);
        guest_low[0x7000] = 0x0f;
        guest_low[0x7001] = 0x32;
        at(r, 0x0700, 0);
        r->rcx = 0x1b;
        r->rax = 0x55;
        r->rdx = 0x55;
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_MSR};
        break;
    case 8: /* POST returns: int 10h now in the ROM */
        EXPECT(r->rip == 2 && r->rax == 0 && r->rdx == 0);
        guest_low[0x40] = 0x34;
        guest_low[0x41] = 0x12;
        guest_low[0x42] = 0x00;
        guest_low[0x43] = 0xc0;
        at(r, 0xf000, 0xe005);
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_VMMCALL};
        break;
    case 9: /* the mode set call */
        check_small_segments(r);
        EXPECT(r->cs.base == 0xf0000 && r->rip == 0xe010 && r->rsp == 0x7c00 && r->rax == 0);
        if (scenario == SCRIPT_MODE_NPF) {
            *e = (struct x86rm_exit){.reason = X86RM_EXIT_NPF, .gpa = 0xd0000, .npf_write = true};
            break;
        }
        guest_low[0x449] = scenario == SCRIPT_MODE_FAIL ? 0x12 : 3;
        guest_low[0x44a] = 80;
        guest_low[0x484] = 24;
        at(r, 0xf000, 0xe015);
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_VMMCALL};
        break;
    default:
        EXPECT(!"run after the mode set returned");
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_SHUTDOWN};
        break;
    }
}

/* PCI reset fixture: the NVIDIA BAR5 sequence (unlock key, PTIMER_TIME_0 twice) as
 * IO exits at C000:43C9..4428, then the POST returns, or a nested page
 * fault ends it so that the failure dump runs. With vbios_trace_io the
 * reads return the card's values; without it the ports are denied. */
static bool trace_fail;
static void io(struct x86rm_regs *r, struct x86rm_exit *e, uint16_t ip, uint16_t port, bool in,
               uint32_t eax)
{
    at(r, 0xc000, ip);
    if (!in)
        r->rax = eax;
    *e = (struct x86rm_exit){.reason = X86RM_EXIT_IO,
                             .port = port,
                             .size = 4,
                             .in = in,
                             .next_rip = ip + 2u,
                             .next_rip_valid = true};
}
static void trace_script(struct x86rm_regs *r, struct x86rm_exit *e)
{
    switch (step) {
    case 0:
        EXPECT(!(guest_io_pass[0xe000 / 8] & 1) && !(guest_io_pass[0xe07f / 8] & 0x80) &&
               (guest_io_pass[0x3c0 / 8] & 1) && (guest_io_pass[0x80 / 8] & 1));
        io(r, e, 0x43c9, 0xe000, false, 0x2469fdb9);
        break;
    case 1:
        io(r, e, 0x43d4, 0xe004, false, 1);
        break;
    case 2:
        io(r, e, 0x4423, 0xe008, false, 0x9400);
        break;
    case 3:
        io(r, e, 0x4428, 0xe00c, true, 0);
        break;
    case 4:
        EXPECT((uint32_t)r->rax == 0x1000);
        io(r, e, 0x4428, 0xe00c, true, 0);
        break;
    case 5:
        EXPECT((uint32_t)r->rax == 0x2000);
        if (trace_fail) {
            *e = (struct x86rm_exit){.reason = X86RM_EXIT_NPF, .gpa = 0xd0000, .npf_write = true};
            break;
        }
        at(r, 0xf000, 0xe005);
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_VMMCALL};
        break;
    case 6:
        guest_low[0x449] = 3;
        at(r, 0xf000, 0xe015);
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_VMMCALL};
        break;
    default:
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_SHUTDOWN};
        break;
    }
}

/* VMX: the ROM executes INT n and reaches the container's entry for n;
 * the INT pushed IP, CS (C000h) and FLAGS at SS:SP. */
static void soft_int(struct x86rm_regs *r, struct x86rm_exit *e, unsigned vector, uint32_t ax,
                     uint32_t cx, uint16_t flags)
{
    r->ss = (struct x86rm_segment){0, 0x93, 0xffff, 0};
    r->rsp = 0x7bf0;
    guest_low[0x7bf0] = 0x34;
    guest_low[0x7bf1] = 0x12;
    guest_low[0x7bf2] = 0x00;
    guest_low[0x7bf3] = 0xc0;
    guest_low[0x7bf4] = (uint8_t)flags;
    guest_low[0x7bf5] = (uint8_t)(flags >> 8);
    at(r, 0xf000, 0xd000 + 4 * vector);
    r->rax = ax;
    r->rcx = cx;
    *e = (struct x86rm_exit){.reason = X86RM_EXIT_VMMCALL};
}
static uint16_t frame_flags(void)
{
    return (uint16_t)(guest_low[0x7bf4] | guest_low[0x7bf5] << 8);
}
static void call_returns(struct x86rm_regs *r, struct x86rm_exit *e, uint16_t done)
{
    at(r, 0xf000, done);
    *e = (struct x86rm_exit){.reason = X86RM_EXIT_VMMCALL};
}
static unsigned host_bridge_writes;

/* The live-state sequence on gen6 IGD-like IGD, with int 15h,
 * int 11h and the IGD's configuration visibility exercised by the "VBIOS"
 * on the way; SCRIPT_LIVE_FAIL fails the mode set, so the VBE restore runs. */
static void live_script(struct x86rm_regs *r, struct x86rm_exit *e)
{
    switch (step) {
    case 0: /* the AH=0Fh query first, on the host's conventional memory */
        check_small_segments(r);
        EXPECT(r->cs.base == 0xf0000 && r->rip == 0xe028 && (r->rax & 0xffff) == 0x0f00);
        EXPECT(ivt_word(guest_low, 0x10) == 0xc0000014 && ivt_word(guest_low, 0x1f) == 0xc0005000 &&
               ivt_word(guest_low, 0x15) == 0xf000d054 && ivt_word(guest_low, 0x13) == 0xf000d04c &&
               ivt_word(guest_low, 0x08) == 0xf000d020 && ivt_word(guest_low, 0x1a) == 0xf000d068);
        EXPECT(guest_low[0x413] == 0x7f && guest_low[0x40e] == 0xc0 && guest_low[0x449] == 0x12 &&
               guest_low[0x9fc00] == 1);
        EXPECT(guest_shadow[0] == 0x55 && guest_shadow[2] == 0x80 && guest_shadow[0x2000] == 0x5a);
        EXPECT(guest_maps == 7 && guest_map[6].gpa == 0xdae9d000 && guest_map[6].size == 0x3000 &&
               !guest_map[6].hva && guest_map[6].flags == X86RM_MAP_WRITE);
        EXPECT(!memcmp(guest_stub + 0xe02a, fake_info->hypercall, 3) &&
               !memcmp(guest_stub + 0xd054, fake_info->hypercall, 3) &&
               guest_stub[0xd057] == 0xcf && guest_stub[0xe028] == 0xcd &&
               guest_stub[0xe029] == 0x10);
        EXPECT((guest_io_pass[0x5000 / 8] & 1) && (guest_io_pass[0x3c0 / 8] & 1) &&
               !(guest_io_pass[0xcf8 / 8] & 1));
        soft_int(r, e, 0x15, 0x5f35, 0x1234, 0x0202);
        break;
    case 1: /* 5Fxx unhandled under the mode set: unchanged, CF=1 */
        EXPECT(r->rip == 0xd057 && (r->rax & 0xffff) == 0x5f35 && (r->rcx & 0xffff) == 0x1234 &&
               (frame_flags() & 1));
        soft_int(r, e, 0x11, 0, 0, 0x0203);
        break;
    case 2: /* int 11h from the host's BDA, FLAGS untouched */
        EXPECT((r->rax & 0xffff) == 0x4427 && frame_flags() == 0x0203);
        soft_int(r, e, 0x15, 0x8600, 0, 0x0202);
        break;
    case 3: /* any other int 15h: AH=86h, CF=1; then the host bridge's command written */
        EXPECT((r->rax & 0xff00) == 0x8600 && (frame_flags() & 1));
        host_bridge_writes = fake(HOST_BRIDGE)->writes;
        at(r, 0xc000, 0x200);
        r->rax = 0x80000004u;
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_IO,
                                 .port = 0xcf8,
                                 .size = 4,
                                 .next_rip = 0x201,
                                 .next_rip_valid = true};
        break;
    case 4:
        r->rax = 0x0007;
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_IO,
                                 .port = 0xcfc,
                                 .size = 2,
                                 .next_rip = 0x202,
                                 .next_rip_valid = true};
        break;
    case 5: /* dropped; the LPC bridge's IDs read */
        EXPECT(fake(HOST_BRIDGE)->writes == host_bridge_writes);
        r->rax = 0x80000000u | LPC << 8;
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_IO,
                                 .port = 0xcf8,
                                 .size = 4,
                                 .next_rip = 0x203,
                                 .next_rip_valid = true};
        break;
    case 6:
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_IO,
                                 .port = 0xcfc,
                                 .size = 4,
                                 .in = true,
                                 .next_rip = 0x204,
                                 .next_rip_valid = true};
        break;
    case 7: /* the PCH root port stays invisible */
        EXPECT((uint32_t)r->rax == 0x1c4f8086);
        r->rax = 0x80000000u | ROOT_PORT << 8;
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_IO,
                                 .port = 0xcf8,
                                 .size = 4,
                                 .next_rip = 0x205,
                                 .next_rip_valid = true};
        break;
    case 8:
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_IO,
                                 .port = 0xcfc,
                                 .size = 4,
                                 .in = true,
                                 .next_rip = 0x206,
                                 .next_rip_valid = true};
        break;
    case 9: /* AH=0Fh returns: mode 12h, 80 columns */
        EXPECT((uint32_t)r->rax == UINT32_MAX);
        r->rax = 0x5012;
        r->rbx = 0;
        call_returns(r, e, 0xe02a);
        break;
    case 10: /* then 4F03h: VBE mode 117h with the linear frame buffer */
        EXPECT(r->rip == 0xe028 && (r->rax & 0xffff) == 0x4f03);
        r->rax = 0x004f;
        r->rbx = 0x4117;
        call_returns(r, e, 0xe02a);
        break;
    case 11: /* the mode set */
        EXPECT(r->rip == 0xe010 && r->rsp == 0x7c00);
        if (scenario == SCRIPT_LIVE_FAIL) {
            at(r, 0xc000, 0x300);
            *e = (struct x86rm_exit){.reason = X86RM_EXIT_EXCEPTION, .vector = 6};
            break;
        }
        if (scenario == SCRIPT_LIVE_HOLD)
            memcpy(igd_mmio + 0xe1180, "\x02\x03\x30\x80", 4); /* PCH_LVDS 80300302 */
        guest_low[0x449] = 3;
        guest_low[0x44a] = 80;
        guest_low[0x484] = 24;
        call_returns(r, e, 0xe015);
        break;
    case 12: /* SCRIPT_LIVE_FAIL, and SCRIPT_LIVE_HOLD after its test page:
               * the GOP's mode back, memory kept */
        EXPECT((scenario == SCRIPT_LIVE_FAIL || scenario == SCRIPT_LIVE_HOLD) && r->rip == 0xe028 &&
               (r->rax & 0xffff) == 0x4f02 && (r->rbx & 0xffff) == 0xc117);
        if (scenario == SCRIPT_LIVE_HOLD)
            EXPECT(scratch[0] == 'I' && scratch[1] == 0x1f && scratch[(2 * 80 + 2) * 2] == 1 &&
                   scratch[(11 * 80 + 4) * 2 + 1] == 0x1f);
        r->rax = 0x004f;
        call_returns(r, e, 0xe02a);
        break;
    default:
        EXPECT(!"run after the sequence ended");
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_SHUTDOWN};
        break;
    }
}

/* vbios_call=post on the host image: the fresh PC BIOS environment, and the
 * coreboot int 15h answers for an Intel card. */
static void host_post_script(struct x86rm_regs *r, struct x86rm_exit *e)
{
    switch (step) {
    case 0:
        EXPECT(r->rip == 0xe000 && r->rax == IGD && guest_shadow[0] == 0x55 &&
               guest_shadow[2] == host_option[2]);
        EXPECT(ivt_word(guest_low, 0x15) == 0xf000d054 && ivt_word(guest_low, 0x10) == 0xf000f065 &&
               guest_low[0x413] == 0x80 && !guest_low[0x40e] && !guest_low[0x9fc00]);
        soft_int(r, e, 0x15, 0x5f51, 0x1234, 0x0203);
        break;
    case 1:
        EXPECT((r->rax & 0xffff) == 0x005f && (r->rcx & 0xffff) == 0x0001 && !(frame_flags() & 1));
        soft_int(r, e, 0x15, 0x5f40, 0x1234, 0x0202);
        break;
    case 2:
        EXPECT((r->rax & 0xffff) == 0x5f40 && (r->rcx & 0xffff) == 0x1234 && (frame_flags() & 1));
        call_returns(r, e, 0xe005);
        break;
    case 3:
        EXPECT(r->rip == 0xe010);
        guest_low[0x449] = 3;
        call_returns(r, e, 0xe015);
        break;
    default:
        EXPECT(!"run after the mode set returned");
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_SHUTDOWN};
        break;
    }
}

bool __wrap_hal_x86rm_run(struct x86rm *rm, struct x86rm_regs *r, struct x86rm_exit *e)
{
    (void)rm;
    memset(e, 0, sizeof(*e));
    switch (scenario) {
    case SCRIPT_POST:
    case SCRIPT_MODE_FAIL:
    case SCRIPT_MODE_NPF:
        post_script(r, e);
        break;
    case SCRIPT_RECLAIM:
        check_small_segments(r);
        CHECK(!route_problem());
        CHECK(r->rip == 0xe010);
        for (unsigned i = 0x400; i < 0x500; ++i) {
            if (i >= 0x46c && i <= 0x470)
                continue; /* container timer updates */
            CHECK(guest_low[i] == ((i >= 0x449 && i <= 0x466) || (i >= 0x484 && i <= 0x48a)
                                       ? 0x5a
                                       : reclaim_bda[i - 0x400]));
        }
        if (reclaim_call_failed) {
            *e = (struct x86rm_exit){.reason = X86RM_EXIT_NPF, .gpa = 0xd0000, .npf_write = true};
        } else {
            guest_low[0x449] = 3;
            call_returns(r, e, 0xe015);
        }
        break;
    case SCRIPT_NPF:
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_NPF, .gpa = 0xd0000, .npf_write = true};
        break;
    case SCRIPT_FATAL:
        /* One exit at a stub-free address with IF as given. */
        at(r, 0x0700, 0);
        r->rflags = fatal_flags;
        guest_low[0x7000] = 0xf4;
        *e = fatal_exit;
        break;
    case SCRIPT_TRACE:
        trace_script(r, e);
        break;
    case SCRIPT_LIVE:
    case SCRIPT_LIVE_FAIL:
    case SCRIPT_LIVE_HOLD:
        live_script(r, e);
        break;
    case SCRIPT_HOST_POST:
        host_post_script(r, e);
        break;
    case SCRIPT_INTR:
    case SCRIPT_CLI:
        if (!step)
            EXPECT(r->rip == (scenario == SCRIPT_CLI ? 0xe020 : 0xe000));
        clock_now += 10000000; /* 10 ms of guest time per exit */
        *e = (struct x86rm_exit){.reason = X86RM_EXIT_INTR};
        break;
    }
    ++step;
    return true;
}

static const char *run_quiet(enum scenario which, const char *cmdline, bool expected)
{
    scenario = which;
    step = creates = destroys = consoles = 0;
    script_error = NULL;
    port_call_count = 0;
    window_index = 0;
    ptimer = 0;
    inventory();
    static struct boot_info bi;
    memset(&bi, 0, sizeof(bi));
    snprintf(bi.cmdline, sizeof(bi.cmdline), "vmm_media=00:03.0 %s", cmdline);
    bi.fb = (struct boot_framebuffer){.base = 0x80000000, .size = 0x300000};
    CHECK(platform_vbios_post(&bi) == expected);
    if (!expected)
        CHECK(!platform_vbios_state()->posted);
    return script_error;
}
static void run(enum scenario which, const char *cmdline, bool expected)
{
    const char *error = run_quiet(which, cmdline, expected);
    if (error) {
        fprintf(stderr, "script: %s (step %u)\n", error, step);
        ++failures;
    }
}

/* VMX: the gen6 platform, the host snapshot and an OpRegion. */
static bool logged(uint64_t from, const char *text);
static uint8_t opregion_mem[0x3000];
static bool igd_ivd;
static enum {
    HOST_PCI_LIVE,
    HOST_CLASSIC_UNINITIALIZED,
    HOST_CLASSIC_COMPAT,
    HOST_PCI_UNINITIALIZED
} host_snapshot_kind;
static enum boot_mem_type opregion_type = BOOT_MEM_ACPI_NVS;
static const char *run_igd(enum scenario which, const char *cmdline, bool expected)
{
    scenario = which;
    step = creates = destroys = consoles = 0;
    script_error = NULL;
    topology_igd = true;
    inventory();
    if (igd_ivd)
        put(fake(IGD), 0x50, 0x0002, 2);
    build_host_snapshot();
    if (host_snapshot_kind == HOST_CLASSIC_UNINITIALIZED ||
        host_snapshot_kind == HOST_CLASSIC_COMPAT)
        build_classic_snapshot();
    if (host_snapshot_kind == HOST_CLASSIC_COMPAT) {
        host_low[0x40] = 7;
        host_low[0x43] = 0xc0;
    }
    if (host_snapshot_kind == HOST_PCI_UNINITIALIZED)
        memset(host_low + 0x40, 0, 4);
    memcpy(opregion_mem + 0x18, "IntelGraphicsMem", 16);
    opregion_mem[0x18 + 0x10] = 8;
    static struct boot_info bi;
    memset(&bi, 0, sizeof(bi));
    snprintf(bi.cmdline, sizeof(bi.cmdline), "vmm_media=00:03.0 %s", cmdline);
    bi.fb = (struct boot_framebuffer){.base = 0xe0000000, .size = 0x300000};
    bi.legacy = (struct boot_legacy){
        host_low, sizeof(host_low), host_option, sizeof(host_option), false, false, 0, 0};
    bi.mem_range_count = 1;
    bi.mem[0] = (struct boot_mem_range){0xdae9d000, 0x3000, opregion_type, opregion_mem};
    CHECK(platform_vbios_post(&bi) == expected);
    if (!expected)
        CHECK(!platform_vbios_state()->posted);
    topology_igd = false;
    if (script_error) {
        fprintf(stderr, "script: %s (step %u)\n", script_error, step);
        ++failures;
    }
    return script_error;
}

static void test_flow_vmx(void)
{
    const struct platform_vbios_state *s = platform_vbios_state();
    uint64_t from = log_get_ring()->write_pos;
    /* The defaults for a card without a ROM BAR: host image, mode set, on a
     * backend reporting VMX (VMCALL in every stub). */
    fake_info = &fake_vmx;
    run_igd(SCRIPT_LIVE, "display=vbios", true);
    CHECK(step == 12 && creates == 1 && destroys == 1 && consoles == 1 && s->posted &&
          s->bdf == IGD);
    CHECK(s->runtime_size == 0x10000 && s->stub[0xd000] == 0x0f && s->stub[0xd002] == 0xc1 &&
          s->stub[0xd003] == 0xcf);
    CHECK(
        logged(from, "vbios_image=host vbios_call=mode") &&
        logged(from, "host C0000 image 8086:0106") &&
        logged(
            from,
            "VBIOS live state: int 10h AH=0Fh -> mode 12, 80 columns, page 0; AX=4F03h -> AX=004f BX=4117") &&
        logged(from,
               "VBIOS OpRegion: ASLS dae9d018, signature \"IntelGraphicsMem\", size field 8 KiB") &&
        logged(from, "read-only for the Intel IGD: 00:00.0 00:1f.0") &&
        logged(from, "VBIOS int 15h AX=5f35") &&
        logged(from, "software interrupts served by the container: 11h=1 15h=2"));
    /* The mode set fails: the GOP's VBE mode goes back through the VBIOS. */
    run_igd(SCRIPT_LIVE_FAIL, "display=vbios", false);
    CHECK(step == 13 && destroys == 1 && consoles == 0 && !s->posted);
    /* vbios_text_hold: mode 3 reached, the IGD registers logged, the test
     * page held, then the GOP's mode back and no console switch. */
    from = log_get_ring()->write_pos;
    clock_mocked = true;
    run_igd(SCRIPT_LIVE_HOLD, "display=vbios vbios_text_hold=5", false);
    clock_mocked = false;
    CHECK(step == 13 && destroys == 1 && consoles == 0 && !s->posted);
    CHECK(logged(from, "VBIOS text hold: mode 3 reached") && logged(from, "PCH_LVDS=80300302") &&
          logged(from, "vbios_text_hold diagnostic over, the GOP's VBE mode is back") &&
          !logged(from, "no text mode;"));
    /* ,keep: the same page and hold, no restore, and the run goes on as a
     * successful mode set (the console switch, the guest's legacy state). */
    from = log_get_ring()->write_pos;
    clock_mocked = true;
    run_igd(SCRIPT_LIVE, "display=vbios vbios_text_hold=5,keep", true);
    clock_mocked = false;
    CHECK(step == 12 && destroys == 1 && consoles == 1 && s->posted);
    CHECK(logged(from, "VBIOS text hold: mode 3 reached") && logged(from, "mode 3 stays (,keep)") &&
          !logged(from, "VBIOS restore:"));
    fake_info = &fake_svm;
    /* Explicit POST on the same image, with the coreboot answers. */
    run_igd(SCRIPT_HOST_POST, "display=vbios vbios_call=post", true);
    CHECK(step == 4 && consoles == 1 && s->posted && s->stub[0xd002] == 0xd9);
    /* A C0000 ROM with no initialized INT10 is POSTed directly. Explicit
     * mode selection still requires the platform's initialized state. */
    host_snapshot_kind = HOST_CLASSIC_UNINITIALIZED;
    from = log_get_ring()->write_pos;
    run_igd(SCRIPT_HOST_POST, "display=vbios", true);
    CHECK(step == 4 && creates == 1 && consoles == 1 && s->runtime_size == 0x8000 &&
          logged(from, "vbios_image=host vbios_call=post"));
    run_igd(SCRIPT_HOST_POST, "display=vbios vbios_image=host vbios_call=post", true);
    CHECK(step == 4 && creates == 1);
    run_igd(SCRIPT_HOST_POST, "display=vbios vbios_call=mode", false);
    CHECK(!creates && !step);
    host_snapshot_kind = HOST_CLASSIC_COMPAT;
    run_igd(SCRIPT_HOST_POST, "display=vbios", false);
    CHECK(!creates && !step); /* A classic compatibility service is not live VGA. */
    host_snapshot_kind = HOST_PCI_UNINITIALIZED;
    run_igd(SCRIPT_HOST_POST, "display=vbios vbios_call=post", true);
    CHECK(step == 4 && creates == 1); /* PCI host POST does not depend on INT10. */
    run_igd(SCRIPT_HOST_POST, "display=vbios", false);
    CHECK(!creates &&
          !step); /* Its changed checksum cannot establish an intact uninitialized ROM. */
    host_snapshot_kind = HOST_PCI_LIVE;
    /* Refusals before any container: an IGD has no ROM BAR; GGC.IVD set;
     * the OpRegion over usable RAM is left out, the rest runs. */
    run_igd(SCRIPT_LIVE, "display=vbios vbios_image=rom", false);
    CHECK(!creates && !step);
    run_igd(SCRIPT_LIVE, "display=vbios vbios_reset=sbr", false);
    CHECK(!creates && !step);
    igd_ivd = true;
    run_igd(SCRIPT_LIVE, "display=vbios", false);
    CHECK(!creates && !step);
    igd_ivd = false;
    opregion_type = BOOT_MEM_USABLE;
    scenario = SCRIPT_NPF;
    run_igd(SCRIPT_NPF, "display=vbios", false);
    CHECK(creates == 1 && guest_maps == 6 && consoles == 0);
    opregion_type = BOOT_MEM_ACPI_NVS;
    /* On the ROM-BAR card: vbios_call=mode alone is refused before any
     * container, the explicit pair rom+mode and unknown values fail setup. */
    run(SCRIPT_POST, "display=vbios vbios_call=mode", false);
    CHECK(!creates && !step);
    static struct boot_info bad;
    const char *invalid[] = {"display=vbios vbios_image=rom vbios_call=mode",
                             "display=vbios vbios_image=flash", "display=vbios vbios_call=init",
                             "display=vbios vbios_reset=sbr vbios_call=mode"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memset(&bad, 0, sizeof(bad));
        snprintf(bad.cmdline, sizeof(bad.cmdline), "%s", invalid[i]);
        CHECK(!platform_vbios_post(&bad));
    }
}

static void test_flow(void)
{
    run(SCRIPT_POST, "display=vbios", true);
    const struct platform_vbios_state *s = platform_vbios_state();
    CHECK(step == 10 && creates == 1 && destroys == 1 && consoles == 1);
    /* The same POST on a backend reporting VMX: VMCALL in every stub. */
    fake_info = &fake_vmx;
    run(SCRIPT_POST, "display=vbios", true);
    CHECK(step == 10 && consoles == 1 && s->stub[0xe007] == 0xc1 && s->stub[0xfe70] == 0xc1);
    fake_info = &fake_svm;
    run(SCRIPT_POST, "display=vbios", true);
    CHECK(step == 10 && consoles == 1 && s->stub[0xe007] == 0xd9);
    CHECK(s->posted && s->bdf == CARD && s->runtime_size == 0x800 && s->shadow_size == 0x1000);
    CHECK(s->low && s->low[0x449] == 3 && s->shadow[0] == 0x55 && s->stub[0xff53] == 0xcf);
    CHECK((get(BRIDGE, 0x3e, 2) & 8) && !(get(OTHER_BRIDGE, 0x3e, 2) & 8));
    /* Broken variant of this check: a POST whose script fails must not
     * reach the console. */
    run(SCRIPT_NPF, "display=vbios", false);
    CHECK(step == 1 && creates == 1 && destroys == 1 && consoles == 0);
    /* Every other fatal exit ends setup at once: string I/O, HLT with IF
     * clear, a VMMCALL outside the stubs, an exception, a triple fault,
     * a refused VMCB, an SVM instruction. */
    const struct {
        struct x86rm_exit exit;
        uint64_t flags;
    } fatal[] = {
        {{.reason = X86RM_EXIT_IO,
          .port = 0x3f8,
          .size = 1,
          .string = true,
          .rep = true,
          .next_rip = 2,
          .next_rip_valid = true},
         0x202},
        {{.reason = X86RM_EXIT_HLT}, 0x002},
        {{.reason = X86RM_EXIT_VMMCALL}, 0x202},
        {{.reason = X86RM_EXIT_EXCEPTION, .vector = 6}, 0x202},
        {{.reason = X86RM_EXIT_SHUTDOWN}, 0x202},
        {{.reason = X86RM_EXIT_INVALID, .code = UINT64_MAX}, 0x202},
        {{.reason = X86RM_EXIT_OTHER, .code = 0x80}, 0x202},
    };
    for (unsigned i = 0; i < sizeof(fatal) / sizeof(fatal[0]); ++i) {
        fatal_exit = fatal[i].exit;
        fatal_flags = fatal[i].flags;
        run(SCRIPT_FATAL, "display=vbios", false);
        CHECK(step == 1 && destroys == 1 && consoles == 0 && !platform_vbios_state()->posted);
    }
    /* A mismatch between the exit and the opcode at CS:IP (F4, not CPUID)
     * is fatal as well when the length must be decoded. */
    fatal_exit = (struct x86rm_exit){.reason = X86RM_EXIT_CPUID};
    fatal_flags = 0x202;
    run(SCRIPT_FATAL, "display=vbios", false);
    CHECK(step == 1 && destroys == 1 && consoles == 0);
    /* The bound on INTR exits only (the cli-loop proof, hosted): 100 ms at
     * 10 ms per exit, and the BDA tick follows the clock on every exit. */
    clock_mocked = true;
    clock_now = 1;
    run(SCRIPT_INTR, "display=vbios vbios_post_ms=100", false);
    CHECK(step >= 10 && step <= 11 && destroys == 1 && consoles == 0);
    clock_now = 1;
    run(SCRIPT_CLI, "display=vbios vbios_selftest=cli-loop vbios_post_ms=100", false);
    CHECK(step >= 10 && step <= 11 && destroys == 1 && consoles == 0);
    clock_mocked = false;
    /* Options: no POST without display=vbios; invalid values fail setup. */
    run(SCRIPT_POST, "display=gop", true);
    CHECK(!creates && !step);
    static struct boot_info bad;
    memset(&bad, 0, sizeof(bad));
    const char *invalid[] = {"display=vga", "display=vbios vbios_post_ms=0",
                             "display=vbios vbios_mode_ms=600001",
                             "display=vbios vbios_selftest=hang", "display=vbios vbios_post_ms=1x"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        snprintf(bad.cmdline, sizeof(bad.cmdline), "%s", invalid[i]);
        CHECK(!platform_vbios_post(&bad));
    }
}

static void test_setup_failures(void)
{
    run(SCRIPT_POST, "display=vbios", true);
    backend_unavailable = true;
    run(SCRIPT_POST, "display=vbios", false);
    CHECK(!creates && !step && !consoles);
    backend_unavailable = false;

    topology_no_vga = true;
    run(SCRIPT_POST, "display=vbios", false);
    CHECK(!creates && !step && !consoles);
    topology_no_vga = false;

    topology_bad_rom = true;
    run(SCRIPT_POST, "display=vbios", false);
    CHECK(!creates && !step && !consoles);
    topology_bad_rom = false;
    run(SCRIPT_POST, "display=vbios vbios_image=host", false);
    CHECK(!creates && !step && !consoles);

    topology_bad_mapping = true;
    run(SCRIPT_POST, "display=vbios", false);
    CHECK(!creates && !step && !consoles);
    topology_bad_mapping = false;

    container_unavailable = true;
    run(SCRIPT_POST, "display=vbios", false);
    CHECK(creates == 1 && !destroys && !step && !consoles);
    container_unavailable = false;

    run(SCRIPT_MODE_FAIL, "display=vbios", false);
    CHECK(step == 10 && creates == 1 && destroys == 1 && !consoles);
    run(SCRIPT_MODE_NPF, "display=vbios", false);
    CHECK(step == 10 && creates == 1 && destroys == 1 && !consoles);

    console_unavailable = true;
    run(SCRIPT_POST, "display=vbios", false);
    CHECK(step == 10 && creates == 1 && destroys == 1 && consoles == 1);
    console_unavailable = false;
}

/* --- PCI reset fixture: the GPU reset (vbios_reset=sbr) ------------------------- */

static uint8_t before_card[4096], before_audio[4096], before_bridge[4096];
static void snapshot(void)
{
    memcpy(before_card, fake(CARD)->config, 4096);
    memcpy(before_audio, fake(AUDIO)->config, 4096);
    memcpy(before_bridge, fake(BRIDGE)->config, 256);
}
static bool gpu_fn(uint16_t bdf)
{
    return bdf == CARD || bdf == AUDIO;
}
static uint32_t id_of(uint16_t bdf)
{
    const uint8_t *c = bdf == CARD ? before_card : before_audio;
    return (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24;
}
/* NULL when the logged accesses of one SBR are what a reset requires:
 * nothing written before the assert; held at least 1 ms; nothing sent to
 * the GPU's functions until 100 ms after the link reported active; then
 * per function no write before it answered with its own IDs, the ReBAR
 * control before BAR0, the BARs before the command register, the command
 * register last; no write to any other function; and the final state
 * equals the saved one. */
static const char *reset_problem(const struct config_access *log, unsigned n)
{
    unsigned assert_at = n, release_at = n;
    uint64_t link_active = UINT64_MAX;
    /* With Link Status believable, 100 ms count from the link coming up
     * (the fake's time, not what the code read); else from the release. */
    bool link_status = !fake_no_link_status;
    for (unsigned i = 0; i < n; ++i) {
        const struct config_access *a = &log[i];
        if (a->write && a->bdf != BRIDGE && !gpu_fn(a->bdf))
            return "write to another function";
        bool control = a->bdf == BRIDGE && a->write && a->off <= 0x3e && a->off + a->size > 0x3e;
        if (control && assert_at == n && ((a->value >> ((0x3e - a->off) * 8)) & 0x40))
            assert_at = i;
        else if (control && assert_at < n && release_at == n &&
                 !((a->value >> ((0x3e - a->off) * 8)) & 0x40))
            release_at = i;
        if (assert_at == n && a->write)
            return "write before the assert";
    }
    if (assert_at == n || release_at == n)
        return "no Secondary Bus Reset";
    if (log[release_at].time - log[assert_at].time < MS)
        return "held less than 1 ms";
    link_active = link_status ? log[release_at].time + fake_link_ms * MS : log[release_at].time;
    unsigned answered[2] = {n, n}, rebar = n, bar0 = n, bars_last[2] = {0, 0}, command[2] = {n, n};
    for (unsigned i = assert_at + 1; i < n; ++i) {
        const struct config_access *a = &log[i];
        if (!gpu_fn(a->bdf))
            continue;
        unsigned k = a->bdf == AUDIO;
        if (a->time < link_active + 100 * MS)
            return "request to the GPU within 100 ms of Link Active";
        if (!a->write && a->off == 0 && a->size == 4 && a->value == id_of(a->bdf) &&
            answered[k] == n)
            answered[k] = i;
        if (!a->write)
            continue;
        if (answered[k] == n)
            return "write before the function answered";
        if (command[k] != n)
            return "write after the command register";
        if (a->bdf == CARD && a->off == CARD_REBAR + 8) {
            if (bar0 != n)
                return "Resizable BAR control after BAR0";
            rebar = i;
        }
        if (a->off >= 0x10 && a->off <= 0x27) {
            bars_last[k] = i;
            if (a->bdf == CARD && a->off == 0x10)
                bar0 = i;
        }
        if (a->off == 4)
            command[k] = i;
    }
    if (command[0] == n || command[1] == n)
        return "command register not restored";
    if (rebar == n)
        return "Resizable BAR control not restored";
    if (bars_last[0] > command[0] || bars_last[1] > command[1])
        return "BAR after the command register";
    if (memcmp(fake(CARD)->config, before_card, 256) ||
        memcmp(fake(CARD)->config + CARD_REBAR, before_card + CARD_REBAR, 16) ||
        memcmp(fake(AUDIO)->config, before_audio, 256) ||
        memcmp(fake(BRIDGE)->config, before_bridge, 256))
        return "final state differs from the saved one";
    return NULL;
}
static unsigned reset_start;
static enum pci_reset_result reset_run(uint16_t bdf)
{
    alog_count = reset_start = 0; /* the log holds the reset alone (an inventory scan fills it) */
    return platform_pci_reset_function(bdf);
}
static bool writes_since(uint16_t bdf, unsigned from)
{
    for (unsigned i = from; i < alog_count; ++i)
        if (alog[i].write && alog[i].bdf == bdf)
            return true;
    return false;
}

static void test_reset(void)
{
    clock_mocked = true;
    clock_now = 1;
    fake_crs = fake_link_never = false;
    topology_extra = false;
    alog_count = 0;
    inventory();
    snapshot();
    CHECK(reset_run(CARD) == PCI_RESET_DONE);
    const char *problem = reset_problem(alog + reset_start, alog_count - reset_start);
    if (problem)
        fprintf(stderr, "reset: %s\n", problem);
    CHECK(!problem);
    CHECK(!fake(CARD)->writes_not_ready && !fake(AUDIO)->writes_not_ready &&
          !fake(CARD)->rebar_order_violations);
    /* Broken variants of the log: each one breaks one rule. */
    static struct config_access bad[8192];
    unsigned n = alog_count - reset_start;
    const struct config_access *log = alog + reset_start;
    unsigned assert_at = 0, release_at = 0, rebar = 0, bar0 = 0, command = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (log[i].write && log[i].bdf == BRIDGE && log[i].off == 0x3e) {
            if (!assert_at)
                assert_at = i;
            else
                release_at = i;
        }
        if (log[i].write && log[i].bdf == CARD && log[i].off == CARD_REBAR + 8)
            rebar = i;
        if (log[i].write && log[i].bdf == CARD && log[i].off == 0x10)
            bar0 = i;
        if (log[i].write && log[i].bdf == CARD && log[i].off == 4)
            command = i;
    }
    CHECK(assert_at && release_at && rebar && bar0 && command && rebar < bar0 && bar0 < command);
    memcpy(bad, log, n * sizeof(*log));
    bad[release_at].time = bad[assert_at].time; /* no hold */
    CHECK(reset_problem(bad, n));
    memcpy(bad, log, n * sizeof(*log)); /* early request */
    for (unsigned i = release_at + 1; i < n; ++i)
        if (gpu_fn(bad[i].bdf)) {
            bad[i].time = bad[release_at].time + 50 * MS;
            break;
        }
    CHECK(reset_problem(bad, n));
    memcpy(bad, log, n * sizeof(*log)); /* BAR0 before ReBAR */
    struct config_access swap = bad[rebar];
    bad[rebar] = bad[bar0];
    bad[bar0] = swap;
    CHECK(reset_problem(bad, n));
    memcpy(bad, log, n * sizeof(*log)); /* command not last */
    swap = bad[command];
    bad[command] = bad[bar0];
    bad[bar0] = swap;
    CHECK(reset_problem(bad, n));
    memcpy(bad, log, n * sizeof(*log));
    bad[assert_at - 1 > 0 ? 1 : 0].write = true; /* write before */
    bad[assert_at - 1 > 0 ? 1 : 0].bdf = CARD;
    CHECK(reset_problem(bad, n));
    before_card[4] ^= 2; /* state differs */
    CHECK(reset_problem(log, n));
    before_card[4] ^= 2;
    /* Configuration Request Retry Status while the card gets ready. */
    fake_crs = true;
    inventory();
    snapshot();
    CHECK(reset_run(CARD) == PCI_RESET_DONE);
    problem = reset_problem(alog + reset_start, alog_count - reset_start);
    if (problem)
        fprintf(stderr, "reset with CRS: %s\n", problem);
    CHECK(!problem);
    fake_crs = false;
    /* A port claiming Link Active reporting with the link shown down while
     * the card answers (QEMU's pcie-root-port): the fixed 100 ms from the
     * release, then the same restore. */
    fake_no_link_status = true;
    inventory();
    snapshot();
    CHECK(reset_run(CARD) == PCI_RESET_DONE);
    problem = reset_problem(alog + reset_start, alog_count - reset_start);
    if (problem)
        fprintf(stderr, "reset without Link Status: %s\n", problem);
    CHECK(!problem);
    fake_no_link_status = false;
    /* The link never comes back: nothing restored, false. */
    fake_link_never = true;
    inventory();
    snapshot();
    CHECK(reset_run(CARD) == PCI_RESET_FAILED && !writes_since(CARD, reset_start) &&
          !writes_since(AUDIO, reset_start));
    fake_link_never = false;
    /* A card that is still not ready at the 10 s bound (a GPU may answer
     * CRS for seconds): failed, nothing restored; 9 s is still in time. */
    fake_ready_ms = 10200;
    inventory();
    snapshot();
    CHECK(reset_run(CARD) == PCI_RESET_FAILED && !writes_since(CARD, reset_start) &&
          !writes_since(AUDIO, reset_start));
    fake_ready_ms = 9000;
    inventory();
    snapshot();
    CHECK(reset_run(CARD) == PCI_RESET_DONE && !memcmp(fake(CARD)->config, before_card, 256));
    fake_ready_ms = 150;
    /* Other identifiers after the reset: not restored, false. */
    inventory();
    snapshot();
    fake(CARD)->defaults[2] ^= 1;
    CHECK(reset_run(CARD) == PCI_RESET_FAILED && !writes_since(CARD, reset_start));
    /* Another device below the bridge: no Secondary Bus Reset. With FLR
     * advertised, the card alone; without it, refused with no write. */
    topology_extra = true;
    inventory();
    snapshot();
    CHECK(reset_run(CARD) == PCI_RESET_DONE);
    CHECK(!writes_since(BRIDGE, reset_start) && !writes_since(AUDIO, reset_start) &&
          !writes_since(0x0108, reset_start));
    bool initiated = false;
    for (unsigned i = reset_start; i < alog_count; ++i)
        initiated |= alog[i].write && alog[i].bdf == CARD && alog[i].off == CARD_PCIE + 8 &&
                     (alog[i].value & 0x8000);
    CHECK(initiated && !memcmp(fake(CARD)->config, before_card, 256) &&
          !fake(CARD)->writes_not_ready);
    inventory();
    put(fake(CARD), CARD_PCIE + 4, 0x00008fc0, 4); /* no FLR */
    CHECK(reset_run(CARD) == PCI_RESET_REFUSED);
    for (unsigned i = 0; i < alog_count; ++i)
        CHECK(!alog[i].write);
    topology_extra = false;
    /* A function on a root bus without FLR (the VMM's controller here). */
    inventory();
    CHECK(reset_run(MEDIA) == PCI_RESET_REFUSED);
    for (unsigned i = 0; i < alog_count; ++i)
        CHECK(!alog[i].write);
    clock_mocked = false;
}

/* ACS Control must be verified before any reset function resumes DMA. */
static void reset_acs(bool sibling)
{
    struct fake_fn *f = fake(sibling ? AUDIO : CARD);
    if (sibling) {
        put(f, 6, 0x10, 2);
        put(f, 0x34, CARD_PCIE, 1);
        put(f, CARD_PCIE, 0x00020010, 4);
        f->ecam = true;
        put(f, 0x100, (CARD_ACS << 20) | 0x00010001, 4);
    } else {
        /* The next pointer's reserved low bits must be ignored. */
        put(f, CARD_REBAR, ((CARD_ACS | 3u) << 20) | 0x00010015, 4);
    }
    put(f, CARD_ACS, (CARD_ACS << 20) | 0x0001000d, 4); /* cycle */
    put(f, CARD_ACS + 4, 0x001d, 2);
    put(f, CARD_ACS + 6, 0x001d, 2);
    memcpy(f->defaults + 0x100, f->config + 0x100, sizeof(f->config) - 0x100);
    memset(f->defaults + CARD_ACS + 6, 0, 2);
    if (sibling)
        memcpy(f->defaults + 0x40, f->config + 0x40, 0xc0);
}
static void test_reset_ari(void)
{
    clock_mocked = true;
    clock_now = 1;
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        topology_extra = false;
        build_topology();
        put(fake(BRIDGE), BRIDGE_PCIE + 0x24, 0x20, 4);
        put(fake(BRIDGE), BRIDGE_PCIE + 0x28, 0x20, 2);
        put(fake(CARD), 0x0e, 0, 1); /* ARI does not require F0.MF. */
        if (scenario) {
            fake(AUDIO)->bdf = 0x109; /* Sparse high function in the same device. */
            put(fake(CARD), CARD_REBAR, (CARD_ACS << 20) | 0x00010015, 4);
            put(fake(CARD), CARD_ACS, 0x0001000e, 4);
            put(fake(CARD), CARD_ACS + 4, 2, 2);
            put(fake(CARD), CARD_ACS + 6, 2, 2);
            memcpy(fake(CARD)->defaults + CARD_ACS, fake(CARD)->config + CARD_ACS, 8);
            memset(fake(CARD)->defaults + CARD_ACS + 6, 0, 2);
        }
        if (scenario == 2)
            put(fake(0x109), 8, 0x01080200, 4); /* VMM media: SBR forbidden. */
        if (scenario == 3) {
            /* Nine actual functions exceed the bounded POST save array. */
            for (unsigned fn = 10; fn < 17; ++fn) {
                struct fake_fn *f = &fns[nfns++];
                f->bdf = 0x100 | fn;
                put(f, 0, 0x12348086, 4);
            }
            put(fake(CARD), CARD_PCIE + 4, 0x00008fc0, 4); /* no FLR */
        }
        platform_pci_set_host(&fake_ops, NULL);
        CHECK(platform_pci_inventory(scenario == 2 ? "vmm_media=01:01.1" : "vmm_media=00:03.0"));
        CHECK(reset_run(CARD) == (scenario == 3 ? PCI_RESET_REFUSED : PCI_RESET_DONE));
        CHECK(get(BRIDGE, BRIDGE_PCIE + 0x28, 2) == 0x20);
        if (scenario == 1) {
            CHECK(writes_since(0x109, 0));
            CHECK(get(0x109, 0, 4) == 0x22e810de);
            CHECK(get(CARD, CARD_ACS + 6, 2) == 2);
        }
        if (scenario >= 2) {
            CHECK(!writes_since(BRIDGE, 0) && !writes_since(0x109, 0));
            if (scenario == 3)
                for (unsigned i = 0; i < alog_count; ++i)
                    CHECK(!alog[i].write);
        }
    }
    platform_pci_set_host(NULL, NULL);
    clock_mocked = false;
}
static void test_reset_acs(void)
{
    clock_mocked = true;
    clock_now = 1;
    for (unsigned flr = 0; flr < 2; ++flr) {
        topology_extra = flr != 0;
        inventory();
        reset_acs(false);
        if (!flr)
            reset_acs(true);
        CHECK(reset_run(CARD) == PCI_RESET_DONE);
        CHECK(get(CARD, CARD_ACS + 6, 2) == 0x001d);
        if (!flr)
            CHECK(get(AUDIO, CARD_ACS + 6, 2) == 0x001d);
        bool checked[2] = {false, flr != 0};
        for (unsigned i = 0; i < alog_count; ++i) {
            const struct config_access *a = &alog[i];
            if (!gpu_fn(a->bdf))
                continue;
            unsigned k = a->bdf == AUDIO;
            if (!a->write && a->off == CARD_ACS + 6 && a->value == 0x001d)
                checked[k] = true;
            if (a->write && a->off == CARD_ACS + 6)
                checked[k] = false;
            if (a->write && a->off == 4 && (a->value & 4))
                CHECK(checked[0] && checked[1]);
        }
        inventory();
        reset_acs(false);
        fake(CARD)->acs_readonly = true;
        CHECK(reset_run(CARD) == PCI_RESET_FAILED);
        CHECK(!(get(CARD, 4, 2) & 4));
    }
    topology_extra = false;
    inventory();
    reset_acs(false);
    reset_acs(true);
    fake(AUDIO)->acs_readonly = true;
    CHECK(reset_run(CARD) == PCI_RESET_FAILED);
    CHECK(!(get(CARD, 4, 2) & 4) && !(get(AUDIO, 4, 2) & 4));
    for (unsigned i = 0; i < alog_count; ++i)
        CHECK(!(alog[i].write && gpu_fn(alog[i].bdf) && alog[i].off == 4 && (alog[i].value & 4)));
    /* A truncated ACS at the end of ECAM cannot expose its control. */
    inventory();
    put(fake(CARD), CARD_REBAR, (0xffcu << 20) | 0x00010015, 4);
    put(fake(CARD), 0xffc, 0x0001000d, 4);
    CHECK(reset_run(CARD) == PCI_RESET_DONE);
    for (unsigned i = 0; i < alog_count; ++i)
        CHECK(alog[i].off + alog[i].size <= 0x1000);
    /* Conventional PCI functions do not have PCIe extended capabilities. */
    inventory();
    fake(AUDIO)->ecam = true;
    put(fake(AUDIO), 0x100, 0x0001000d, 4);
    put(fake(AUDIO), 0x106, 0x001d, 2);
    CHECK(reset_run(CARD) == PCI_RESET_DONE);
    for (unsigned i = 0; i < alog_count; ++i)
        CHECK(alog[i].bdf != AUDIO || alog[i].off < 0x100);
    clock_mocked = false;
}

/* --- PCI reset fixture: the I/O BAR trace (vbios_trace_io=N) ------------------------ */

/* NULL when the kept events are exactly the sequence numbers expected:
 * the first `limit`, then the last `limit` not already among them. */
static const char *kept_problem(const struct vbios_io_event *const *kept, unsigned n,
                                uint64_t count, unsigned limit)
{
    unsigned k = 0;
    for (uint64_t seq = 0; seq < count; ++seq) {
        if (seq >= limit && seq + limit < count)
            continue;
        if (k >= n || kept[k]->seq != seq)
            return "kept sequence";
        ++k;
    }
    return k == n ? NULL : "extra events";
}
static bool logged(uint64_t from, const char *text)
{
    static char lines[400][LOG_MATCH_COLUMNS + 1];
    size_t total = 0, kept = log_ring_since(log_get_ring(), from, lines, 400, &total);
    for (size_t i = 0; i < kept; ++i)
        if (strstr(lines[i], text))
            return true;
    return false;
}
/* In the last 40 lines of the attempt, which the VMM replays on screen. */
static bool in_replay(uint64_t from, const char *text)
{
    static char lines[40][LOG_MATCH_COLUMNS + 1];
    size_t total = 0, kept = log_ring_since(log_get_ring(), from, lines, 40, &total);
    for (size_t i = 0; i < kept; ++i)
        if (strstr(lines[i], text))
            return true;
    return false;
}

static void test_trace(void)
{
    static struct vbios_io_trace t;
    static const struct vbios_io_event *kept[2 * VBIOS_TRACE_MAX];
    /* The ring: 10 accesses with N=4 keep 0-3 and 6-9; 6 keep all once. */
    for (unsigned total = 0; total <= 12; ++total) {
        vbios_trace_start(&t, 4);
        for (unsigned i = 0; i < total; ++i)
            vbios_trace_record(&t, 0xc000, (uint16_t)i, 0xe000 + 4 * (i % 4), 4, i % 2, i);
        unsigned n = vbios_trace_kept(&t, kept, 2 * VBIOS_TRACE_MAX);
        const char *problem = kept_problem(kept, n, total, 4);
        if (problem)
            fprintf(stderr, "trace ring (%u): %s\n", total, problem);
        CHECK(!problem && n == (total < 8 ? total : 8));
    }
    unsigned n = vbios_trace_kept(&t, kept, 2 * VBIOS_TRACE_MAX);
    CHECK(kept_problem(kept + 1, n - 1, t.count, 4)); /* broken: the first one lost */
    const struct vbios_io_event *dup[16];
    memcpy(dup, kept, n * sizeof(*kept));
    dup[5] = dup[4];
    CHECK(kept_problem(dup, n, t.count, 4)); /* broken: one repeated */
    CHECK(!vbios_trace_kept(&t, kept, 0));
    /* Index/data pairing and the register table: key, enable, PTIMER x3. */
    vbios_trace_start(&t, 64);
    vbios_trace_record(&t, 0xc000, 0x43c9, 0xe000, 4, true, 0x2469fdb9);
    vbios_trace_record(&t, 0xc000, 0x43d4, 0xe004, 4, true, 1);
    for (unsigned i = 0; i < 3; ++i) {
        vbios_trace_record(&t, 0xc000, 0x4423, 0xe008, 4, true, 0x9400);
        vbios_trace_record(&t, 0xc000, 0x4428, 0xe00c, 4, false, 0x7000);
    }
    vbios_trace_record(&t, 0xc000, 0x4455, 0xe010, 4, true, 0x1700);
    vbios_trace_record(&t, 0xc000, 0x445a, 0xe014, 4, false, 0x55);
    n = vbios_trace_kept(&t, kept, 64);
    CHECK(n == 10 && kept[1]->paired && kept[1]->index == 0x2469fdb9 && kept[3]->paired &&
          kept[3]->index == 0x9400 && !kept[2]->paired && kept[9]->paired &&
          kept[9]->index == 0x1700);
    const struct vbios_io_reg *top[4];
    unsigned m = vbios_trace_top(&t, top, 4);
    CHECK(m == 4 && top[0]->count == 3 && top[1]->count == 3);
    bool ptimer_read = false;
    for (unsigned i = 0; i < m; ++i)
        if (top[i]->paired && top[i]->index == 0x9400 && !top[i]->write)
            ptimer_read = top[i]->count == 3 && !top[i]->changed && top[i]->first_value == 0x7000 &&
                          top[i]->last_ip == 0x4428;
    CHECK(ptimer_read);
    /* The scripted POST: the card's ports exit and reach the hardware with
     * the exact values; the values read come back in EAX. */
    uint64_t from = log_get_ring()->write_pos;
    run(SCRIPT_TRACE, "display=vbios vbios_trace_io=2", true);
    CHECK(consoles == 1 && port_call_count == 5);
    const struct port_call expected[] = {{0xe000, 0x2469fdb9, 4, true},
                                         {0xe004, 1, 4, true},
                                         {0xe008, 0x9400, 4, true},
                                         {0xe00c, 0x1000, 4, false},
                                         {0xe00c, 0x2000, 4, false}};
    for (unsigned i = 0; i < 5 && i < port_call_count; ++i)
        CHECK(port_calls[i].port == expected[i].port && port_calls[i].value == expected[i].value &&
              port_calls[i].size == expected[i].size && port_calls[i].out == expected[i].out);
    CHECK(logged(
        from,
        "VBIOS trace summary: reg 00009400 read 2x at port e00c, first 00001000 last 00002000, last at c000:4428"));
    CHECK(!logged(from, "VBIOS trace 0 c000:43c9")); /* the kept events only on failure */
    /* On failure: the kept events (first 2, last 2, the gap) in the dump,
     * then the summary as the attempt's last VBIOS lines. */
    trace_fail = true;
    from = log_get_ring()->write_pos;
    CHECK(!run_quiet(SCRIPT_TRACE, "display=vbios vbios_trace_io=2", false) && consoles == 0);
    trace_fail = false;
    CHECK(logged(from, "VBIOS trace 0 c000:43c9 out e000/4 2469fdb9") &&
          logged(from, "VBIOS trace 1 c000:43d4 out e004/4 00000001 reg 2469fdb9") &&
          logged(from, "VBIOS trace   ... 1 access(es) not kept") &&
          logged(from, "VBIOS trace 4 c000:4428 in  e00c/4 00002000 reg 00009400"));
    CHECK(!logged(from, "VBIOS trace 2 c000:4423"));
    CHECK(in_replay(from, "VBIOS trace summary: reg 00009400 read 2x"));
    CHECK(in_replay(from,
                    "; first: out e000 2469fdb9, out e004 1")); /* the first min(6, N) accesses */

    /* Broken variant: the same POST without vbios_trace_io passes the ports
     * in the IOPM, the exits it then sees are denied (all-ones), and no
     * trace reaches the replayed lines. */
    trace_fail = true;
    from = log_get_ring()->write_pos;
    CHECK(run_quiet(SCRIPT_TRACE, "display=vbios", false) && port_call_count == 0);
    CHECK(!in_replay(from, "VBIOS trace summary"));
    trace_fail = false;
    /* With the trace the POST bound defaults to 120 s (10 ms per INTR
     * exit: about 12000 exits); an explicit vbios_post_ms still wins. */
    clock_mocked = true;
    clock_now = 1;
    run(SCRIPT_INTR, "display=vbios vbios_trace_io=4", false);
    CHECK(step >= 11990 && step <= 12010 && consoles == 0);
    clock_now = 1;
    run(SCRIPT_INTR, "display=vbios vbios_trace_io=4 vbios_post_ms=100", false);
    CHECK(step >= 10 && step <= 11);
    clock_now = 1;
    run(SCRIPT_INTR, "display=vbios", false); /* the default without the trace */
    CHECK(step >= 2990 && step <= 3010);
    clock_mocked = false;
    /* Options. */
    static struct boot_info bad;
    memset(&bad, 0, sizeof(bad));
    const char *invalid[] = {"display=vbios vbios_trace_io=0", "display=vbios vbios_trace_io=1025",
                             "display=vbios vbios_trace_io=x", "display=vbios vbios_reset=flr",
                             "display=vbios vbios_reset="};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        snprintf(bad.cmdline, sizeof(bad.cmdline), "%s", invalid[i]);
        CHECK(!platform_vbios_post(&bad));
    }
    /* vbios_trace_ports: exactly the card's I/O BAR leaves the IOPM. */
    inventory();
    static struct vbios_layout l;
    CHECK(vbios_build_layout(device(CARD), low, shadow, sizeof(shadow), stub, &l));
    vbios_trace_ports(device(CARD), &l);
    unsigned passed = 0, bar = 0;
    for (unsigned port = 0; port < 65536; ++port) {
        bool pass = l.io_pass[port / 8] & (1u << (port & 7));
        passed += pass;
        bar += pass && port >= 0xe000 && port < 0xe080;
    }
    CHECK(!bar && passed == 0x30 + 1 + 3);
}

/* vbios_reset=sbr in the POST flow: the reset before the ROM is read. */
static void test_post_reset(void)
{
    clock_mocked = true;
    clock_now = 1;
    alog_count = 0;
    run(SCRIPT_POST, "display=vbios vbios_reset=sbr", true);
    CHECK(consoles == 1 && platform_vbios_state()->posted);
    unsigned sbr = 0, rom = 0;
    for (unsigned i = 0; i < wlog_count; ++i) {
        if (wlog[i].bdf == BRIDGE && wlog[i].off == 0x3e && (wlog[i].value & 0x40) && !sbr)
            sbr = i + 1;
        if (wlog[i].bdf == CARD && wlog[i].off == 0x30 && wlog[i].value == 0x80000001 && !rom)
            rom = i + 1;
    }
    CHECK(sbr && rom && sbr < rom);
    /* Another device below the bridge: FLR of the card (advertised), POST;
     * without FLR the reset is refused and there is no POST at all. */
    topology_extra = true;
    run(SCRIPT_POST, "display=vbios vbios_reset=sbr", true);
    CHECK(consoles == 1 && creates == 1);
    topology_no_flr = true;
    run(SCRIPT_POST, "display=vbios vbios_reset=sbr", false);
    CHECK(consoles == 0 && creates == 0 && !platform_vbios_state()->posted);
    topology_extra = topology_no_flr = false;
    /* A reset that does not bring the card back stops the VMM's setup
     * (platform_vbios_post false): the inventory no longer matches. */
    fake_link_never = true;
    scenario = SCRIPT_POST;
    step = creates = consoles = 0;
    script_error = NULL;
    inventory();
    static struct boot_info bi;
    memset(&bi, 0, sizeof(bi));
    snprintf(bi.cmdline, sizeof(bi.cmdline), "vmm_media=00:03.0 display=vbios vbios_reset=sbr");
    bi.fb = (struct boot_framebuffer){.base = 0x80000000, .size = 0x300000};
    CHECK(!platform_vbios_post(&bi) && creates == 0);
    fake_link_never = false;
    clock_mocked = false;
}

/* --- legacy VGA: what the guest gets ------------------------------------ */

static uint32_t ivt_vector(const uint8_t *ivt, unsigned n)
{
    return (uint32_t)(ivt[n * 4 + 3] << 24 | ivt[n * 4 + 2] << 16 | ivt[n * 4 + 1] << 8 |
                      ivt[n * 4]);
}
static void set_vector(uint8_t *ivt, unsigned n, uint32_t v)
{
    ivt[n * 4] = (uint8_t)v;
    ivt[n * 4 + 1] = (uint8_t)(v >> 8);
    ivt[n * 4 + 2] = (uint8_t)(v >> 16);
    ivt[n * 4 + 3] = (uint8_t)(v >> 24);
}
/* NULL when an IVT built by the container (vbios_build_low plus what this
 * test's "POST" hooked) is redirected exactly as the handoff requires. */
static const char *redirect_problem(const uint8_t *ivt)
{
    for (unsigned v = 0; v < 256; ++v) {
        uint32_t want = v == 0x1a                                          ? 0xf000e100
                        : v == 0x13 || v == 0x15 || v == 0x16 || v == 0x60 ? 0xf000e400
                        : v == 0x10 || v == 0x6d                           ? 0xc0001234
                        : v == 0x42                                        ? 0xf000f065
                                                                           : 0xf000ff53;
        if (ivt_vector(ivt, v) != want)
            return "vector";
    }
    return NULL;
}
static void test_redirect(void)
{
    static uint8_t ivt[0x400];
    vbios_build_low(low, 0);
    memcpy(ivt, low, sizeof(ivt));
    set_vector(ivt, 0x10, 0xc0001234);
    set_vector(ivt, 0x6d, 0xc0001234); /* POST hooked */
    set_vector(ivt, 0x42, 0xf000f065); /* the image's int 42h target */
    set_vector(ivt, 0x77, 0xf000e000); /* a container-only offset */
    set_vector(ivt, 0x60, 0xf000d054); /* a copy of int 15h's entry: its service */
    unsigned odd = 0, changed = vbios_redirect_ivt(ivt, &odd);
    /* 250 per-vector entries (all but 10h, 1Ah, 42h, 60h, 6Dh, 77h), 60h,
     * 77h and 1Ah. */
    CHECK(changed == 253 && odd == 1);
    const char *problem = redirect_problem(ivt);
    if (problem)
        fprintf(stderr, "redirect: %s\n", problem);
    CHECK(!problem);
    set_vector(ivt, 0x1a, 0xf000fe6e); /* broken: int 1Ah left on the VMMCALL stub */
    CHECK(redirect_problem(ivt));
    set_vector(ivt, 0x1a, 0xf000e100);
    set_vector(ivt, 0x15, 0xf000ff53); /* broken: int 15h not CF=1 */
    CHECK(redirect_problem(ivt));
    set_vector(ivt, 0x15, 0xf000e400);
    set_vector(ivt, 0x08, 0xf000d020); /* broken: a container entry kept */
    CHECK(redirect_problem(ivt));
    /* PCI BIOS advertised topology; the CPU selftest executes the code. */
    static uint8_t segment[VBIOS_STUB_SIZE];
    vbios_build_guest_stub(segment, 7);
    CHECK(segment[0xe0f0] == 7);
}

/* The handoff tail and the guest regions after the scripted POST. */
static uint8_t guest_page0[4096] __attribute__((aligned(4096)));
static const char *tail_problem(const struct ia64vmm_handoff *h)
{
    const uint32_t flags =
        IA64VMM_LEGACY_TEXT_CONSOLE | IA64VMM_LEGACY_IVT | IA64VMM_LEGACY_ROM | IA64VMM_LEGACY_STUB;
    if (h->legacy_version != 1 || h->legacy_flags != flags || h->legacy_bdf != CARD)
        return "version/flags/bdf";
    if (h->legacy_ivt_base || h->legacy_ivt_size != 0x500 || h->legacy_rom_base != 0xc0000 ||
        h->legacy_rom_size != 0x800 || h->legacy_stub_base != 0xf0000 ||
        h->legacy_stub_size != 0x10000)
        return "locations";
    if (h->fb_format || h->fb_base || h->fb_size || h->fb_width || h->fb_pitch)
        return "framebuffer left";
    return legacy_handoff_check(h);
}
/* A protected device page faults on every access. Single-step exactly
 * one instruction, then protect again: an observer reread is measurable. */
static uint8_t *device_page;
static volatile sig_atomic_t device_reads, device_writes;
static void device_access(int signo, siginfo_t *info, void *context)
{
    ucontext_t *u = context;
    if (signo == SIGSEGV) {
        if ((uintptr_t)info->si_addr - (uintptr_t)device_page >= 4096)
            _exit(2);
        if (u->uc_mcontext.gregs[REG_ERR] & 2)
            ++device_writes;
        else
            ++device_reads;
        if (mprotect(device_page, 4096, PROT_READ | PROT_WRITE))
            _exit(2);
        u->uc_mcontext.gregs[REG_EFL] |= 0x100;
    } else {
        if (mprotect(device_page, 4096, PROT_NONE))
            _exit(2);
        u->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    }
}
static void test_trace_observation(void)
{
    static struct vcpu cpu;
    device_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(device_page != MAP_FAILED);
    struct sigaction action = {.sa_sigaction = device_access, .sa_flags = SA_SIGINFO}, old_segv,
                     old_trap;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGSEGV, &action, &old_segv) && !sigaction(SIGTRAP, &action, &old_trap));
    uint64_t *backing = (uint64_t *)device_page;
    const uint64_t base = 0x60000000;
    const struct gm_region region = {base, 8,   GM_DIRECT_MMIO, "trace-test", device_page,
                                     NULL, NULL};
    CHECK(gm_add_region(&region));
    struct pci_device card = {0};
    card.bars[0].base = base;
    card.bars[0].size = 8;
    CHECK(!vbios_guest_trace_init("vbios_trace_guest=2"));
    CHECK(vbios_guest_trace_init("vbios_trace_guest=1"));
    vbios_guest_trace_card(&card);
    uint64_t from = log_get_ring()->write_pos;
    /* Real direct MMIO remains one load/store of the guest's width. The
     * observer sees the returned value, never rereads the device. */
    cpu.s.ip = 0xe000000000001000;
    for (unsigned size = 1; size <= 8; size *= 2) {
        *backing = UINT64_MAX;
        CHECK(!mprotect(device_page, 4096, PROT_NONE));
        device_reads = device_writes = 0;
        CHECK(ia64_physical_write(&cpu, base, size, 0x1234).kind == IA64_STEP_COMPLETE);
        CHECK(device_writes == 1 && device_reads == 0);
        CHECK(!mprotect(device_page, 4096, PROT_READ | PROT_WRITE));
        CHECK(*backing ==
              ((UINT64_MAX & ~(size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1)) |
               (size == 1 ? 0x34 : 0x1234)));
        uint64_t value = 0;
        CHECK(!mprotect(device_page, 4096, PROT_NONE));
        CHECK(ia64_physical_read(&cpu, base, size, &value).kind == IA64_STEP_COMPLETE);
        CHECK(value == (size == 1 ? 0x34 : 0x1234));
        CHECK(device_reads == 1 && device_writes == 1);
        CHECK(!mprotect(device_page, 4096, PROT_READ | PROT_WRITE));
    }
    cpu.cfg.index = 1;
    CHECK(ia64_physical_write(&cpu, base, 8, 0x1234).kind == IA64_STEP_COMPLETE);
    uint64_t second = 0;
    CHECK(ia64_physical_read(&cpu, base, 8, &second).kind == IA64_STEP_COMPLETE &&
          second == 0x1234);
    cpu.cfg.index = 0;
    CHECK(!vbios_guest_trace_port(&cpu, 0x3c4, 2, true, 0x0100));
    CHECK(!vbios_guest_trace_port(&cpu, 0x3da, 1, false, 8));
    CHECK(!vbios_guest_trace_port(&cpu, 0x80, 1, true, 0));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424930));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 1));
    for (unsigned i = 0; i < 10; ++i)
        CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, i == 0 ? 0x4f02 : i));
    vbios_guest_trace_summary();
    CHECK(logged(from, "PLOG media log: not requested"));
    CHECK(logged(from, "BAR0 60000000+8 R=5 W=5"));
    CHECK(logged(from, "OS VGA/BAR ports R=1 W=1"));
    CHECK(logged(from, "INT10 cpu0 in  AX=4f02 BX=0001 CX=0002 DX=0003 F=0009"));
    CHECK(logged(from, "VBE selected BX=0001 result pending"));
    /* VBE 3.0 ModeInfoBlock: attrs +0, pitch +16, X/Y +18/+20,
     * depth +25 and PhysBasePtr +40 (SeaBIOS src/std/vbe.h). */
    uint32_t input[10] = {0x4f01, 0, 0x140};
    uint32_t output[14] = {0x004f, 0, 0x140};
    output[10] = 0x4f01009b;
    output[11] = 640u << 16 | 2560u;
    output[12] = 32u << 16 | 480u;
    output[13] = 0xe0000000;
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424930));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 1));
    for (unsigned i = 0; i < 10; ++i)
        CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, input[i]));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424931));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 2));
    for (unsigned i = 0; i < 14; ++i)
        CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, output[i]));
    /* BX bits 15/14/11 are preserve/LFB/custom CRTC, not mode bits. */
    input[0] = 0x4f02;
    input[1] = 0xc940;
    output[0] = 0x004f;
    output[1] = 0xc940;
    output[10] = 0x4f020000;
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424930));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 1));
    for (unsigned i = 0; i < 10; ++i)
        CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, input[i]));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424931));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 2));
    for (unsigned i = 0; i < 14; ++i)
        CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, output[i]));
    for (unsigned k = 0; k < 10; ++k) {
        CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424930));
        CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, k + 3));
        for (unsigned i = 0; i < 10; ++i)
            CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, i == 0 ? 0x1000 + k : 0));
    }
    from = log_get_ring()->write_pos;
    vbios_guest_trace_summary();
    CHECK(logged(from, "INT10 cpu0 in  AX=4f02 BX=0001")); /* first packet survives */
    CHECK(logged(from, "INT10 cpu0 in  AX=1009"));
    CHECK(logged(from, "VBE selected BX=c940 AX=004f"));
    CHECK(logged(from, "VBE 0140 640x480x32 pitch=2560 attr=009b fb=e0000000"));
    CHECK(!logged(from, "VBE selected mode info not retained"));
    card.vendor = 0x1234;
    card.device = 0x1111;
    vbios_guest_trace_card(&card);
    CHECK(!vbios_guest_trace_port(&cpu, 0x1ce, 2, true, 4));
    CHECK(!vbios_guest_trace_port(&cpu, 0x1cf, 2, false, 1));
    CHECK(!vbios_guest_trace_port(&cpu, 0x1d0, 2, false, 0)); /* crosses owned footprint */
    from = log_get_ring()->write_pos;
    vbios_guest_trace_summary();
    CHECK(logged(from, "OS VGA/BAR ports R=2 W=2"));
    CHECK(vbios_guest_trace_init(""));
    CHECK(!vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424930));
    CHECK(!sigaction(SIGSEGV, &old_segv, NULL) && !sigaction(SIGTRAP, &old_trap, NULL));
    /* Keep the registered region's backing valid until process exit. */
}

/* The diagnostic transport supplies sequences, keeping F0000 read-only. */
static void test_trace_sequence(void)
{
    struct vcpu cpu = {0};
    uint32_t sequence = 0;
    CHECK(vbios_guest_trace_init("vbios_trace_guest=1"));
    CHECK(!vbios_guest_trace_sequence(&cpu, 0x80, 4, &sequence));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424930));
    CHECK(!vbios_guest_trace_sequence(&cpu, 0x80, 1, &sequence));
    CHECK(!vbios_guest_trace_sequence(&cpu, 0x81, 4, &sequence));
    CHECK(vbios_guest_trace_sequence(&cpu, 0x80, 4, &sequence) && sequence == 1);
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, sequence));
    CHECK(!vbios_guest_trace_sequence(&cpu, 0x80, 4, &sequence));
    CHECK(vbios_guest_trace_port(NULL, 0x80, 4, true, 0x56424931));
    CHECK(vbios_guest_trace_sequence(NULL, 0x80, 4, &sequence) && sequence == 2);
    cpu.cfg.index = 1;
    CHECK(!vbios_guest_trace_sequence(&cpu, 0x80, 4, &sequence));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424930));
    CHECK(vbios_guest_trace_sequence(&cpu, 0x80, 4, &sequence) && sequence == 3);
    CHECK(vbios_guest_trace_init(""));
    CHECK(!vbios_guest_trace_sequence(&cpu, 0x80, 4, &sequence));
}

/* Packet framing must recover after a lost tail, migration and a container call. */
static void test_trace_resync(void)
{
    struct vcpu cpu = {0};
    CHECK(vbios_guest_trace_init("vbios_trace_guest=1"));
    uint64_t from = log_get_ring()->write_pos;
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424930));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 10));
    CHECK(vbios_guest_trace_port(NULL, 0x80, 4, true, 0x56424931));
    CHECK(vbios_guest_trace_port(NULL, 0x80, 4, true, 11));
    for (unsigned i = 0; i < 14; ++i)
        CHECK(vbios_guest_trace_port(NULL, 0x80, 4, true, i));
    cpu.cfg.index = 1;
    CHECK(!vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x1234));
    cpu.cfg.index = 0;
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424930));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 12));
    for (unsigned i = 0; i < 10; ++i)
        CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, i == 0 ? 0x1234 : i));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424931));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 13));
    vbios_guest_trace_summary();
    CHECK(logged(from, "INT10 container0 out"));
    CHECK(logged(from, "INT10 cpu0 in  AX=1234"));
    CHECK(logged(from, "discarded=1"));
    CHECK(logged(from, "seq=12"));
    CHECK(logged(from, "1 pending call(s)"));
    CHECK(logged(from, "incomplete packet"));
    CHECK(logged(from, "guest may have stopped inside INT10"));
    CHECK(vbios_guest_trace_init(""));
}

/* A guest thread can migrate during a call: the output packet of a call
 * entered on vCPU 0 may come from vCPU 3. A guest call is pending only when
 * no output packet follows it on any vCPU; the container counts apart.
 * index IA64VMM_MAX_CPUS sends from the container (no vCPU). Words beyond
 * `count` are zero. */
static void trace_packet(unsigned index, bool output, uint32_t sequence, const uint32_t *words,
                         unsigned count)
{
    struct vcpu cpu = {0};
    cpu.cfg.index = index;
    struct vcpu *v = index == IA64VMM_MAX_CPUS ? NULL : &cpu;
    CHECK(vbios_guest_trace_port(v, 0x80, 4, true, output ? 0x56424931 : 0x56424930));
    CHECK(vbios_guest_trace_port(v, 0x80, 4, true, sequence));
    for (unsigned i = 0; i < (output ? 14u : 10u); ++i)
        CHECK(vbios_guest_trace_port(v, 0x80, 4, true, i < count ? words[i] : 0));
}
static void test_trace_migration(void)
{
    /* 4F02 BX=4115 entered on vCPU 0 returns AX=004F on vCPU 3; word 10 of
     * an output packet carries the function in its high half. */
    static const uint32_t set_in[] = {0x4f02, 0x4115};
    static const uint32_t set_out[] = {0x004f, 0x4115, 0, 0, 0, 0, 0, 0, 0, 0, 0x4f020000};
    static const uint32_t mode12[] = {0x0012};
    CHECK(vbios_guest_trace_init("vbios_trace_guest=1"));
    uint64_t from = log_get_ring()->write_pos;
    trace_packet(0, false, 1, set_in, 2);
    trace_packet(3, true, 2, set_out, 11);
    vbios_guest_trace_summary();
    CHECK(logged(from, "INT10 cpu0 in  AX=4f02"));
    CHECK(logged(from, "INT10 cpu3 out AX=004f"));
    CHECK(logged(from, "VBE selected BX=4115 AX=004f"));
    CHECK(!logged(from, "result pending"));
    CHECK(!logged(from, "pending call(s)"));
    from = log_get_ring()->write_pos;
    trace_packet(2, false, 3, mode12, 1);
    vbios_guest_trace_summary();
    CHECK(logged(from, "INT10 guest 1 pending call(s)"));
    CHECK(logged(from, "guest may have stopped inside INT10"));
    CHECK(vbios_guest_trace_init(""));

    /* A container call without a return is the container's, not the guest's. */
    CHECK(vbios_guest_trace_init("vbios_trace_guest=1"));
    from = log_get_ring()->write_pos;
    trace_packet(IA64VMM_MAX_CPUS, false, 1, mode12, 1);
    trace_packet(1, false, 2, mode12, 1);
    trace_packet(1, true, 3, mode12, 1);
    vbios_guest_trace_summary();
    CHECK(logged(from, "INT10 container0 1 pending call(s)"));
    CHECK(!logged(from, "INT10 guest"));
    CHECK(vbios_guest_trace_init(""));

    /* Packets are assembled per vCPU: a migration inside a packet leaves an
     * incomplete one, which alone does not mean the guest stopped. */
    struct vcpu cpu = {0};
    cpu.cfg.index = 1;
    CHECK(vbios_guest_trace_init("vbios_trace_guest=1"));
    from = log_get_ring()->write_pos;
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x56424930));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 1));
    CHECK(vbios_guest_trace_port(&cpu, 0x80, 4, true, 0x0012));
    vbios_guest_trace_summary();
    CHECK(logged(from, "INT10 cpu1 incomplete packet (2/11 words) seq=1"));
    CHECK(!logged(from, "stopped inside INT10"));
    CHECK(vbios_guest_trace_init(""));
}

/* The trace veneer must leave vendor bytes intact and keep int 10h in
 * C0000-CFFFFh. The far target remains the
 * original handler; no room or a vector outside the image is refused. */
static void test_guest_trace(void)
{
    static uint8_t image[0x20000], low[0x500], stub[0x10000];
    memset(image, 0xa5, sizeof(image));
    memset(low, 0, sizeof(low));
    vbios_build_guest_stub(stub, 0);
    low[0x40] = 0x34;
    low[0x41] = 0x12;
    low[0x43] = 0xc0;
    CHECK(vbios_trace_install(low, image, 0xfa00, sizeof(image), stub) == 0xfc00);
    CHECK(ivt_vector(low, 0x10) == 0xc000fa00);
    for (unsigned i = 0; i < 0xfa00; ++i)
        CHECK(image[i] == 0xa5);
    CHECK(stub[0xe7f0] == 0x34 && stub[0xe7f1] == 0x12 && stub[0xe7f3] == 0xc0);
    CHECK(!vbios_trace_install(low, image, 0x10000, sizeof(image), stub));
    CHECK(!vbios_trace_install(low, image, 0xfa00, 0xfa00, stub));
    low[0x43] = 0xf0;
    CHECK(!vbios_trace_install(low, image, 0xfa00, sizeof(image), stub));
}

static void test_guest(void)
{
    run(SCRIPT_POST, "display=vbios", true);
    CHECK(platform_vbios_state()->posted);
    memset(guest_page0, 0xa5, sizeof(guest_page0));
    const struct gm_region ram = {0, sizeof(guest_page0), GM_RAM, "ram", guest_page0, NULL, NULL};
    CHECK(gm_add_region(&ram));
    /* Another card than the POSTed one: nothing changes. */
    struct ia64vmm_handoff h = {0};
    h.flags = IA64VMM_HF_LEGACY_VGA;
    h.vga_bdf = NIC;
    h.vga_base = IA64VMM_VGA_MEMORY_BASE;
    h.vga_size = IA64VMM_VGA_MEMORY_SIZE;
    h.fb_format = IA64VMM_FB_BGRX8888;
    h.fb_base = 0x80000000;
    h.fb_size = 0x300000;
    h.fb_width = 1024;
    CHECK(platform_vbios_handoff(&h) && !h.legacy_version && h.fb_format && guest_page0[0] == 0xa5);
    h.vga_bdf = CARD;
    h.ram_range_count = 1;
    h.ram[0] = (struct ia64vmm_ram_range){0, 0x1000};
    CHECK(platform_vbios_handoff(&h));
    const char *problem = tail_problem(&h);
    if (problem)
        fprintf(stderr, "tail: %s\n", problem);
    CHECK(!problem);
    struct ia64vmm_handoff bad = h;
    bad.legacy_rom_size = 0x801; /* broken variants of the tail check */
    CHECK(tail_problem(&bad));
    bad = h;
    bad.fb_format = IA64VMM_FB_BGRX8888;
    CHECK(tail_problem(&bad));
    /* Page 0: the script's POST hooked int 10h to C000:1234, the
     * container's int 1Ah became the guest's; the BDA says mode 3. */
    CHECK(ivt_vector(guest_page0, 0x10) == 0xc0001234 &&
          ivt_vector(guest_page0, 0x1a) == 0xf000e100 &&
          ivt_vector(guest_page0, 0x16) == 0xf000e400 &&
          ivt_vector(guest_page0, 0x08) == 0xf000ff53);
    CHECK(guest_page0[0x449] == 3 && guest_page0[0x500] == 0xa5);
    /* Regions: C0000 for the runtime size and F0000, read-only; nothing at
     * C0800-EFFFF (the master-abort ranges take it). */
    struct platform_state *ps = calloc(1, sizeof(*ps));
    CHECK(ps && platform_vbios_map(ps));
    const struct gm_region *rom = gm_lookup(0xc0000), *stub = gm_lookup(0xfe100);
    CHECK(rom && rom->kind == GM_EMULATED && rom->base == 0xc0000 && rom->size == 0x800);
    CHECK(stub && stub->kind == GM_EMULATED && stub->base == 0xf0000 && stub->size == 0x10000);
    CHECK(!gm_lookup(0xc0800) && !gm_lookup(0xd0000) && !gm_lookup(0xeffff));
    if (rom && stub) {
        CHECK(rom->ops->read(rom->opaque, NULL, 0, 2) == 0xaa55);
        rom->ops->write(rom->opaque, NULL, 0, 0x1234, 2);
        CHECK(rom->ops->read(rom->opaque, NULL, 0, 2) == 0xaa55);
        uint64_t code = stub->ops->read(stub->opaque, NULL, 0xe100, 4);
        stub->ops->write(stub->opaque, NULL, 0xe100, code ^ UINT32_MAX, 4);
        CHECK(stub->ops->read(stub->opaque, NULL, 0xe100, 4) == code);
        CHECK(stub->ops->read(stub->opaque, NULL, 0xe0f0, 1) ==
              2); /* last bus of the fake topology */
    }
    free(ps);
    run(SCRIPT_POST, "display=vbios vbios_trace_guest=1", true);
    guest_low[0x40] = 0;
    guest_low[0x41] = 1;
    CHECK(platform_vbios_handoff(&h));
    if (stub) {
        uint64_t data = stub->ops->read(stub->opaque, NULL, 0xe7f4, 4);
        stub->ops->write(stub->opaque, NULL, 0xe7f4, data ^ UINT32_MAX, 4);
        CHECK(stub->ops->read(stub->opaque, NULL, 0xe7f4, 4) == data);
    }
    memcpy(reclaim_bda, guest_low + 0x400, sizeof(reclaim_bda));
    memset(guest_page0 + 0x400, 0x5a, 256);
    for (unsigned failure = 0; failure < 3; ++failure) {
        put(fake(CARD), 4, 4, 2);
        put(fake(BRIDGE), 4, 4, 2);
        put(fake(BRIDGE), 0x3e, 0x10, 2);
        put(fake(OTHER_BRIDGE), 0x3e, 8, 2);
        scenario = SCRIPT_RECLAIM;
        step = 0;
        reclaim_unavailable = failure == 1;
        reclaim_call_failed = failure == 2;
        uint64_t from = log_get_ring()->write_pos;
        reclaim_capture = failure == 0;
        platform_vbios_cleanup();
        reclaim_capture = false;
        CHECK(step == (failure == 1 ? 0 : 1));
        CHECK(get(CARD, 4, 2) == 4 && get(BRIDGE, 4, 2) == 4);
        CHECK(get(BRIDGE, 0x3e, 2) == 0x10 && get(OTHER_BRIDGE, 0x3e, 2) == 8);
        CHECK(logged(from, failure == 1   ? "container unavailable"
                           : failure == 2 ? "VBIOS exit mode 3: FAILED"
                                          : "VBIOS exit mode 3: restored"));
    }
    reclaim_unavailable = reclaim_call_failed = false;
}

/* A guest restart repeats what VMM start did for the
 * configured options. vbios_call=post (the ROM-BAR card's default): the
 * same POST and mode set in a new container. vbios_call=mode (the host
 * image of a card without a ROM BAR): only the mode-3 sequence, on the BDA
 * video state the guest left in page 0. Nothing without a POST at VMM start. */
static void test_restart(void)
{
    const struct platform_vbios_state *s = platform_vbios_state();
    /* A failed re-POST: the C0000 region an earlier boot registered reads
     * as master abort in the next boot, never the pages the restart freed. */
    run(SCRIPT_POST, "display=vbios", true);
    struct ia64vmm_handoff h = {0};
    h.flags = IA64VMM_HF_LEGACY_VGA;
    h.vga_bdf = CARD;
    h.vga_base = IA64VMM_VGA_MEMORY_BASE;
    h.vga_size = IA64VMM_VGA_MEMORY_SIZE;
    h.ram_range_count = 1;
    h.ram[0] = (struct ia64vmm_ram_range){0, 0x1000};
    CHECK(platform_vbios_handoff(&h));
    const struct gm_region *rom = gm_lookup(0xc0000);
    CHECK(rom && rom->ops->read(rom->opaque, NULL, 0, 2) == 0xaa55);
    scenario = SCRIPT_FATAL;
    step = creates = destroys = consoles = 0;
    script_error = NULL;
    CHECK(!platform_vbios_restart() && !s->posted);
    struct ia64vmm_handoff next = {0};
    next.flags = IA64VMM_HF_LEGACY_VGA;
    next.vga_bdf = CARD;
    CHECK(platform_vbios_handoff(&next) && !next.legacy_version);
    CHECK(rom && rom->ops->read(rom->opaque, NULL, 0, 2) == 0xffff &&
          rom->ops->read(rom->opaque, NULL, 0x10, 4) == UINT32_MAX);
    run(SCRIPT_POST, "display=vbios", true);
    CHECK(s->posted);
    step = creates = destroys = consoles = 0;
    script_error = NULL;
    uint64_t from = log_get_ring()->write_pos;
    CHECK(platform_vbios_restart());
    if (script_error) {
        fprintf(stderr, "restart script: %s (step %u)\n", script_error, step);
        ++failures;
    }
    CHECK(step == 10 && creates == 1 && destroys == 1 && consoles == 1 && s->posted &&
          s->bdf == CARD);
    CHECK(logged(from, "display=vbios restart: POST of the ROM image again") &&
          logged(from, "display=vbios restart: POST and mode 3 done"));
    const enum scenario failed_modes[] = {SCRIPT_MODE_FAIL, SCRIPT_MODE_NPF};
    for (unsigned i = 0; i < sizeof(failed_modes) / sizeof(failed_modes[0]); ++i) {
        run(SCRIPT_POST, "display=vbios", true);
        scenario = failed_modes[i];
        step = creates = destroys = consoles = 0;
        script_error = NULL;
        CHECK(!platform_vbios_restart() && !s->posted);
        CHECK(!script_error && step == 10 && creates == 1 && destroys == 1 && !consoles);
    }
    run(SCRIPT_POST, "display=vbios", true);
    container_unavailable = true;
    step = creates = destroys = consoles = 0;
    CHECK(!platform_vbios_restart() && !s->posted);
    CHECK(creates == 1 && !destroys && !step && !consoles);
    container_unavailable = false;

    run(SCRIPT_POST, "display=vbios", true);
    console_unavailable = true;
    step = creates = destroys = consoles = 0;
    script_error = NULL;
    CHECK(!platform_vbios_restart() && !s->posted);
    CHECK(!script_error && step == 10 && creates == 1 && destroys == 1 && consoles == 1);
    console_unavailable = false;

    run(SCRIPT_POST, "display=vbios", true);
    backend_unavailable = true;
    step = creates = destroys = consoles = 0;
    CHECK(!platform_vbios_restart());
    CHECK(creates == 1 && !destroys && !step && !consoles);
    backend_unavailable = false;

    run(SCRIPT_POST, "display=vbios", true);
    platform_pci_set_host(NULL, NULL);
    step = creates = destroys = consoles = 0;
    CHECK(!platform_vbios_restart());
    CHECK(!creates && !step && !consoles);

    run_igd(SCRIPT_LIVE, "display=vbios", true);
    CHECK(s->posted && s->bdf == IGD);
    if (!gm_lookup(0)) {
        const struct gm_region ram = {0,   sizeof(guest_page0), GM_RAM, "ram", guest_page0, NULL,
                                      NULL};
        CHECK(gm_add_region(&ram));
    }
    /* The guest's int 10h calls left mode 12h; the live script checks it. */
    guest_page0[0x449] = 0x12;
    scenario = SCRIPT_LIVE;
    step = creates = destroys = consoles = 0;
    script_error = NULL;
    from = log_get_ring()->write_pos;
    CHECK(platform_vbios_restart());
    if (script_error) {
        fprintf(stderr, "restart script: %s (step %u)\n", script_error, step);
        ++failures;
    }
    CHECK(step == 12 && creates == 1 && destroys == 1 && !consoles);
    CHECK(logged(from, "int 10h mode 3 on the state the guest left (no POST)") &&
          logged(from, "display=vbios restart: mode 3 set") && !logged(from, "VBIOS POST"));
    /* Broken variant: without the guest's BDA the live script's first check fails. */
    guest_page0[0x449] = 3;
    scenario = SCRIPT_LIVE;
    step = creates = destroys = consoles = 0;
    script_error = NULL;
    CHECK(platform_vbios_restart() && script_error);
    guest_page0[0x449] = 0x12;
    scenario = SCRIPT_LIVE_FAIL;
    step = creates = destroys = consoles = 0;
    script_error = NULL;
    CHECK(!platform_vbios_restart());
    CHECK(!script_error && step == 13 && creates == 1 && destroys == 1 && !consoles);

    run_igd(SCRIPT_LIVE, "display=vbios", true);
    reclaim_unavailable = true;
    step = creates = destroys = consoles = 0;
    CHECK(!platform_vbios_restart());
    CHECK(!creates && !step && !consoles);
    reclaim_unavailable = false;

    run_igd(SCRIPT_LIVE, "display=vbios", true);
    backend_unavailable = true;
    step = creates = destroys = consoles = 0;
    CHECK(!platform_vbios_restart());
    CHECK(creates == 1 && !destroys && !step && !consoles);
    backend_unavailable = false;

    run(SCRIPT_POST, "display=gop", true);
    step = creates = 0;
    CHECK(platform_vbios_restart() && !creates && !step);
}

static void dump_guest_stub(const char *path)
{
    static uint8_t segment[VBIOS_STUB_SIZE];
    vbios_build_guest_stub(segment, 0);
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(segment, 1, sizeof(segment), f) == sizeof(segment));
    if (f)
        fclose(f);
}

static void dump_stubs(const char *path, const struct x86rm_info *info)
{
    static uint8_t segment[VBIOS_STUB_SIZE];
    vbios_build_stubs(segment, info->hypercall);
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(segment, 1, sizeof(segment), f) == sizeof(segment));
    if (f)
        fclose(f);
}

/* Offline: the product's image search on a ROM dump (e.g. a ROM BAR
 * snapshot or a flash dump kept outside the repository). */
static int find_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    static uint8_t data[1u << 24];
    size_t n = fread(data, 1, sizeof(data), f);
    fclose(f);
    struct vbios_image image;
    if (!vbios_find_legacy(data, n, &image)) {
        printf("%s: no code type 0 image\n", path);
        return 1;
    }
    printf(
        "%s: code type 0 at +%llx, length %u, init size %u, PCIR revision %u, IDs %04x:%04x, checksum %s\n",
        path, (unsigned long long)image.offset, image.length, image.init_size, image.revision,
        image.vendor, image.device, image.checksum_ok ? "ok" : "bad");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "stubs")) {
        dump_stubs(argv[2], &fake_svm);
        return failures != 0;
    }
    if (argc == 3 && !strcmp(argv[1], "stubs-vmx")) {
        dump_stubs(argv[2], &fake_vmx);
        return failures != 0;
    }
    if (argc == 3 && !strcmp(argv[1], "guest-stub")) {
        dump_guest_stub(argv[2]);
        return failures != 0;
    }
    if (argc == 3 && !strcmp(argv[1], "find"))
        return find_file(argv[2]);
    test_find();
    test_layout();
    test_bochs_ports();
    test_mediation();
    test_route();
    test_rom();
    test_rom_isolation();
    test_default_rom();
    test_low();
    test_ticks();
    test_lengths();
    test_int1a();
    test_host_rom();
    test_vmx_services();
    test_flow();
    test_flow_vmx();
    test_setup_failures();
    test_reset();
    test_reset_acs();
    test_reset_ari();
    test_trace();
    test_post_reset();
    test_redirect();
    test_trace_observation();
    test_guest_trace();
    test_trace_sequence();
    test_trace_resync();
    test_trace_migration();
    test_guest();
    test_restart();
    if (failures) {
        fprintf(stderr, "test-vbios: %u failure(s)\n", failures);
        return 1;
    }
    printf(
        "test-vbios: image search, map/IOPM, CF8/CFC restriction, VGA routing, ROM read, IVT/BDA, ticks, "
        "instruction lengths, int 1Ah, scripted POST/mode set/NPF/bound, IVT redirection, guest tail/page 0/"
        "C0000/F0000 regions, GPU reset (SBR/FLR/refusal, order, waits, restore), I/O BAR trace (ring, "
        "forwarding, dump) (each with its broken variant) passed\n");
    return 0;
}
