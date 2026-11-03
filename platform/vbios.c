/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * display=vbios: run the GPU's own legacy VBIOS (PCI ROM code type 0)
 * once at VMM start in the hardware real-mode container, set text mode 3
 * and move the VMM console to VGA text.
 * This file is the policy: the card, the environment contract (a
 * private 1 MiB, the stub segment, the PC BIOS IVT/BDA, int 1Ah, the tick)
 * and the answer to every exit. The HAL (ia64vmm/x86rm.h) is the mechanism.
 * Nothing here models a device: ports and memory the contract passes go to
 * the real card; everything else is denied and logged.
 */
#include "platform/vbios.h"
#include "platform/vbios_trace.h"
#include "core/log_internal.h"
#include "platform/platform.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (unsigned)p[1] << 8);
}
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}
static uint64_t width_mask(unsigned size)
{
    return size >= 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}

/* --- the image -------------------------------------------------------- */

static bool image_header(const uint8_t *rom, uint64_t size, uint64_t off)
{
    if (off >= size || size - off < 0x1a || le16(rom + off) != 0xaa55)
        return false;
    uint16_t at = le16(rom + off + 0x18);
    return (uint64_t)at + 0x18 <= size - off && !memcmp(rom + off + at, "PCIR", 4);
}

static bool walk_chain(const uint8_t *rom, uint64_t size, uint64_t off, struct vbios_image *out)
{
    /* PCI Firmware 3.0 §5.1 (as tools/vbios/pci_rom.py reads it): images
     * start on 512-byte boundaries with 55AAh, +18h points at "PCIR", whose
     * +10h is the image length in 512-byte units, +14h the code type and
     * bit 7 of +15h the last image. NVIDIA's NPDE continuation only
     * matters past that indicator, where no code type 0 image lives. */
    while (image_header(rom, size, off)) {
        const uint8_t *image = rom + off, *pcir = image + le16(image + 0x18);
        uint64_t length = (uint64_t)le16(pcir + 0x10) * 512;
        if (!length || length > size - off)
            return false;
        if (pcir[0x14] == 0) {
            uint32_t init = image[2] * 512u;
            if (!init || init > length)
                return false;
            uint8_t sum = 0;
            for (uint32_t i = 0; i < init; ++i)
                sum = (uint8_t)(sum + image[i]);
            *out = (struct vbios_image){off,
                                        (uint32_t)length,
                                        init,
                                        le16(pcir + 4),
                                        le16(pcir + 6),
                                        pcir[0x0c],
                                        sum == 0,
                                        pcir[0x0d] | (uint32_t)pcir[0x0e] << 8 |
                                            (uint32_t)pcir[0x0f] << 16,
                                        sum};
            return true;
        }
        if (pcir[0x15] & 0x80)
            return false;
        off += length;
    }
    return false;
}

bool vbios_find_legacy(const uint8_t *rom, uint64_t size, struct vbios_image *out)
{
    /* A ROM BAR starts with its first image. NVIDIA flash dumps carry a
     * vendor region before the chain (37200h bytes on one image); whether
     * the ROM BAR window shows it is open. Without an image
     * at 0, the chain starts at the first 512-byte boundary that holds a
     * valid 55AAh/PCIR header, as tools/vbios/pci_rom.py finds it. */
    if (le16(rom) == 0xaa55)
        return walk_chain(rom, size, 0, out);
    for (uint64_t off = 512; off < size; off += 512)
        if (image_header(rom, size, off))
            return walk_chain(rom, size, off, out);
    return false;
}

const char *vbios_host_rom(const uint8_t *option, uint64_t option_size,
                           const struct pci_device *card, struct vbios_image *out)
{
    if (!option || option_size < 0x1a)
        return "no host C0000 snapshot";
    if (le16(option) != 0xaa55)
        return "no option ROM signature at host C0000";
    if (!le16(option + 0x18)) {
        /* A platform VGA ROM need not carry PCI metadata. Its classic
         * header defines the initialization entry at +3 and the length
         * in 512-byte units; an intact image sums to zero. */
        uint32_t length = option[2] * 512u;
        if (!length || length > option_size || length > VBIOS_SHADOW_MAX)
            return "invalid classic option ROM length at host C0000";
        uint8_t sum = 0;
        for (uint32_t i = 0; i < length; ++i)
            sum = (uint8_t)(sum + option[i]);
        if (sum)
            return "bad classic option ROM checksum at host C0000";
        *out = (struct vbios_image){.length = length, .init_size = length, .checksum_ok = true};
        return NULL;
    }
    /* A declared PCI structure must validate; malformed PCI images are
     * not reinterpreted as classic ROMs. One VBIOS can serve a family of
     * device IDs, but its vendor and display class must match. */
    if (!vbios_find_legacy(option, option_size, out) || out->offset)
        return "no code type 0 image at host C0000";
    if (out->vendor != card->vendor)
        return "the image's PCIR vendor is not the card's";
    if (out->class_code >> 16 != 0x03)
        return "the image's PCIR class is not a display controller";
    if (out->init_size > VBIOS_SHADOW_MAX)
        return "the image's runtime size exceeds C0000-DFFFFh";
    return NULL;
}

const char *vbios_host_image(const uint8_t *option, uint64_t option_size, const uint8_t *low,
                             uint64_t low_size, const struct pci_device *card,
                             struct vbios_image *out)
{
    const char *why = vbios_host_rom(option, option_size, card, out);
    if (why)
        return why;
    /* An INT10 compatibility shim alone does not establish the live VGA
     * state this path preserves. Keep its PCI identity requirement. */
    if (!le16(option + 0x18))
        return "the host's initialized image has no PCIR identity";
    if (!low || low_size < 0x500)
        return "no host IVT snapshot";
    uint32_t int10 = (uint32_t)le16(low + 0x42) * 16 + le16(low + 0x40);
    if (int10 < VBIOS_SHADOW_BASE || int10 - VBIOS_SHADOW_BASE >= out->init_size)
        return "the host's int 10h vector does not point into the image";
    return NULL;
}

unsigned vbios_host_ivt(uint8_t *ivt, uint32_t image_size)
{
    /* The CSM's own code (F000h/E000h), data pointers into its tables and
     * null entries have no code behind them in the container: each goes
     * to the container's entry for its vector, as in a fresh IVT. */
    unsigned changed = 0;
    for (unsigned v = 0; v < 256; ++v) {
        uint32_t target = (uint32_t)le16(ivt + v * 4 + 2) * 16 + le16(ivt + v * 4);
        if (target >= VBIOS_SHADOW_BASE && target - VBIOS_SHADOW_BASE < image_size)
            continue;
        put16(ivt + v * 4, (uint16_t)VBIOS_VECTOR_ENTRY(v));
        put16(ivt + v * 4 + 2, VBIOS_STUB_SEGMENT);
        ++changed;
    }
    return changed;
}

/* --- the card and its bridges ------------------------------------------ */

bool vbios_intel_igd(const struct pci_device *d)
{
    return d->vendor == 0x8086 && d->class_code >> 8 == 0x0300 && d->bdf == (2u << 3);
}

static const struct pci_device *device_of(uint16_t bdf)
{
    for (unsigned i = 0; i < platform_pci_count(); ++i)
        if (platform_pci_device(i)->bdf == bdf)
            return platform_pci_device(i);
    return NULL;
}

bool vbios_card_from_inventory(uint16_t bdf, struct vbios_card *card)
{
    memset(card, 0, sizeof(*card));
    const struct pci_device *d = device_of(bdf);
    if (!d)
        return false;
    card->bdf = bdf;
    for (uint16_t parent = d->parent; parent != 0xffff;) {
        const struct pci_device *bridge = device_of(parent);
        if (!bridge || card->bridge_count == VBIOS_MAX_BRIDGES)
            return false;
        card->bridges[card->bridge_count++] = parent;
        parent = bridge->parent;
    }
    /* An IGD has no bridge. The host bridge decodes legacy VGA
     * for it (GGC.IVD) and the LPC bridge's IDs are what an Intel VBIOS
     * checks (QEMU IGD assignment specification): both readable, never written. */
    if (vbios_intel_igd(d)) {
        static const uint16_t readonly[] = {0x0000, 0x1f << 3};
        for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(readonly); ++i)
            if (device_of(readonly[i]))
                card->readonly[card->readonly_count++] = readonly[i];
    }
    /* B101h CL: the highest bus number in the system. */
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *f = platform_pci_device(i);
        uint8_t bus = f->header == 1 && f->subordinate > (f->bdf >> 8) ? f->subordinate
                                                                       : (uint8_t)(f->bdf >> 8);
        if (bus > card->last_bus)
            card->last_bus = bus;
    }
    return true;
}

bool vbios_writable(void *ctx, uint16_t bdf)
{
    const struct vbios_card *card = ctx;
    if (bdf == card->bdf)
        return true;
    for (unsigned i = 0; i < card->bridge_count; ++i)
        if (card->bridges[i] == bdf)
            return true;
    return false;
}

bool vbios_visible(void *ctx, uint16_t bdf)
{
    const struct vbios_card *card = ctx;
    for (unsigned i = 0; i < card->readonly_count; ++i)
        if (card->readonly[i] == bdf)
            return true;
    return vbios_writable(ctx, bdf);
}

static bool readonly_function(const struct vbios_card *card, uint16_t bdf)
{
    for (unsigned i = 0; i < card->readonly_count; ++i)
        if (card->readonly[i] == bdf)
            return true;
    return false;
}

uint64_t vbios_config_read(struct vbios_card *card, uint32_t *latch, uint16_t port, unsigned size)
{
    /* The IGD's read-only functions show the host's own registers.
     * The host bridge is reserved for the VMM (host-system), so the guest's
     * mediation would read it as absent; these are pure reads of the
     * host's configuration space. */
    uint16_t bdf = (uint16_t)((*latch >> 8) & 0xffff);
    if ((*latch & 0x80000000u) && size <= 4 && !(port & (size - 1)) && port >= 0xcfc &&
        port + size <= 0xd00 && readonly_function(card, bdf))
        return platform_pci_host_read(bdf, (*latch & 0xfc) | (port - 0xcfc), size);
    return platform_config_port_read(latch, port, size, vbios_visible, card);
}
void vbios_config_write(struct vbios_card *card, uint32_t *latch, uint16_t port, uint64_t value,
                        unsigned size)
{
    platform_config_port_write(latch, port, value, size, vbios_writable, card);
}

/* --- the guest physical map and the IOPM -------------------------------- */

static void pass_ports(uint8_t *pass, unsigned first, unsigned count)
{
    for (unsigned p = first; p < first + count && p < 65536; ++p)
        pass[p / 8] |= (uint8_t)(1u << (p & 7));
}

bool vbios_build_layout(const struct pci_device *card, uint8_t *low, uint8_t *shadow,
                        uint32_t shadow_size, uint8_t *stub, struct vbios_layout *out)
{
    memset(out, 0, sizeof(*out));
    if (!shadow_size || (shadow_size & 4095) || shadow_size > VBIOS_SHADOW_MAX)
        return false;
    const struct x86rm_mapping fixed[] = {
        {0, VBIOS_LOW_SIZE, low, X86RM_MAP_WRITE},
        {VBIOS_VGA_BASE, VBIOS_VGA_SIZE, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
        {VBIOS_SHADOW_BASE, shadow_size, shadow, X86RM_MAP_WRITE},
        {VBIOS_STUB_BASE, VBIOS_STUB_SIZE, stub, 0},
    };
    memcpy(out->map, fixed, sizeof(fixed));
    out->map_count = IA64VMM_ARRAY_SIZE(fixed);
    /* The VGA register ranges, port 80h (the ROM's I/O delay), PIT
     * channel 2 with the PIT command port and port 61h (the ROM's timer,
     * Intel VBIOS mode-set timing uses these ports) and
     * the card's own I/O BARs reach the hardware; everything else exits.
     * Channels 0 and 1 (40h, 41h) stay out. */
    pass_ports(out->io_pass, 0x3b0, 0x30);
    pass_ports(out->io_pass, 0x80, 1);
    pass_ports(out->io_pass, 0x42, 2);
    pass_ports(out->io_pass, 0x61, 1);
    /* Fixed DISPI ports include the byte footprint of word I/O at 1CFh. */
    if (platform_vga_has_dispi(card))
        pass_ports(out->io_pass, 0x1ce, 3);
    for (unsigned b = 0; b < 6; ++b) {
        const struct pci_bar *bar = &card->bars[b];
        if (!bar->size || bar->rom)
            continue;
        if (bar->io) {
            if (bar->base + bar->size > 65536)
                return false;
            pass_ports(out->io_pass, (unsigned)bar->base, (unsigned)bar->size);
            continue;
        }
        uint64_t base = IA64VMM_ALIGN_DOWN(bar->base, 4096);
        uint64_t end = IA64VMM_ALIGN_UP(bar->base + bar->size, 4096);
        if (base < 0x100000 || end <= base)
            return false;
        /* Two sub-page BARs of the card may share a page: one mapping. */
        bool merged = false;
        for (unsigned i = IA64VMM_ARRAY_SIZE(fixed); i < out->map_count; ++i) {
            struct x86rm_mapping *m = &out->map[i];
            if (base < m->gpa + m->size && m->gpa < end) {
                uint64_t lo = base < m->gpa ? base : m->gpa;
                uint64_t hi = end > m->gpa + m->size ? end : m->gpa + m->size;
                m->gpa = lo;
                m->size = hi - lo;
                merged = true;
            }
        }
        if (merged)
            continue;
        if (out->map_count == VBIOS_MAX_MAPS)
            return false;
        out->map[out->map_count++] =
            (struct x86rm_mapping){base, end - base, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC};
    }
    return true;
}

const char *vbios_opregion_range(const struct boot_info *bi, uint32_t asls, uint64_t *base,
                                 uint64_t *size)
{
    /* The IGD OpRegion is 8 KiB; the platform's ACPI and SMM share it.
     * Never usable RAM, never the VMM's: every page must lie in a range
     * the BootInfo map types ACPI NVS or reserved. */
    if (!asls)
        return "ASLS is 0";
    *base = IA64VMM_ALIGN_DOWN((uint64_t)asls, 4096);
    *size = IA64VMM_ALIGN_UP((uint64_t)asls + 0x2000, 4096) - *base;
    for (uint64_t page = *base; page < *base + *size; page += 4096) {
        bool ok = false;
        for (unsigned i = 0; i < bi->mem_range_count && !ok; ++i) {
            const struct boot_mem_range *r = &bi->mem[i];
            if (page < r->base || page - r->base >= r->size)
                continue;
            if (r->type != BOOT_MEM_ACPI_NVS && r->type != BOOT_MEM_RESERVED)
                return "not ACPI NVS or reserved memory";
            if (r->size - (page - r->base) < 4096)
                return "a page straddles two memory ranges";
            ok = true;
        }
        if (!ok)
            return "outside the BootInfo memory map";
    }
    return NULL;
}

bool vbios_add_mapping(struct vbios_layout *l, uint64_t base, uint64_t size, unsigned flags)
{
    if (l->map_count == VBIOS_MAX_MAPS || !size || ((base | size) & 4095))
        return false;
    for (unsigned i = 0; i < l->map_count; ++i)
        if (base < l->map[i].gpa + l->map[i].size && l->map[i].gpa < base + size)
            return false;
    l->map[l->map_count++] = (struct x86rm_mapping){base, size, NULL, flags};
    return true;
}

/* --- the card's I/O BAR traced (vbios_trace_io=N) ---------------- */

void vbios_trace_ports(const struct pci_device *card, struct vbios_layout *l)
{
    for (unsigned b = 0; b < 6; ++b) {
        const struct pci_bar *bar = &card->bars[b];
        if (!bar->size || !bar->io || bar->rom || bar->base + bar->size > 65536)
            continue;
        for (uint64_t p = bar->base; p < bar->base + bar->size; ++p)
            l->io_pass[p / 8] &= (uint8_t)~(1u << (p & 7));
    }
}

void vbios_trace_start(struct vbios_io_trace *t, unsigned limit)
{
    memset(t, 0, sizeof(*t));
    t->limit = limit > VBIOS_TRACE_MAX ? VBIOS_TRACE_MAX : limit;
}

void vbios_trace_record(struct vbios_io_trace *t, uint16_t cs, uint16_t ip, uint16_t port,
                        unsigned size, bool write, uint32_t value)
{
    if (!t->limit)
        return;
    struct vbios_io_event e = {t->count, value, 0, cs, ip, port, (uint8_t)size, write, false};
    if ((port & 7) == 4)
        for (unsigned i = 0; i < t->latch_count; ++i)
            if (t->latches[i].port == port - 4) {
                e.index = t->latches[i].value;
                e.paired = true;
            }
    if (write) {
        unsigned i = 0;
        while (i < t->latch_count && t->latches[i].port != port)
            ++i;
        if (i == t->latch_count && t->latch_count < VBIOS_TRACE_LATCHES)
            ++t->latch_count;
        if (i < t->latch_count) {
            t->latches[i].port = port;
            t->latches[i].value = value;
        }
    }
    if (t->count < t->limit)
        t->first[t->count] = e;
    t->last[t->count % t->limit] = e;
    ++t->count;
    /* The register table: a paired access by its index, any other by port. */
    struct vbios_io_reg *r = NULL;
    for (unsigned i = 0; i < t->reg_count && !r; ++i) {
        struct vbios_io_reg *c = &t->regs[i];
        if (c->write == write && c->paired == e.paired &&
            (e.paired ? c->index == e.index : c->port == port))
            r = c;
    }
    if (!r && t->reg_count < VBIOS_TRACE_REGS) {
        r = &t->regs[t->reg_count++];
        *r = (struct vbios_io_reg){0, e.index, value, value, port, cs, ip, write, e.paired, false};
    }
    if (!r) {
        ++t->regs_lost;
        return;
    }
    ++r->count;
    r->changed |= value != r->last_value;
    r->last_value = value;
    r->port = port;
    r->last_cs = cs;
    r->last_ip = ip;
}

unsigned vbios_trace_kept(const struct vbios_io_trace *t, const struct vbios_io_event **out,
                          unsigned max)
{
    unsigned n = 0;
    for (uint64_t i = 0; i < t->count && i < t->limit && n < max; ++i)
        out[n++] = &t->first[i];
    uint64_t from = t->count > t->limit ? t->count - t->limit : 0;
    if (from < t->limit)
        from = t->limit;
    for (uint64_t i = from; i < t->count && n < max; ++i)
        out[n++] = &t->last[i % t->limit];
    return n;
}

unsigned vbios_trace_top(const struct vbios_io_trace *t, const struct vbios_io_reg **out,
                         unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; max && i < t->reg_count; ++i) {
        const struct vbios_io_reg *r = &t->regs[i];
        unsigned at;
        if (n < max)
            at = n++;
        else if (out[max - 1]->count >= r->count)
            continue;
        else
            at = max - 1;
        while (at > 0 && out[at - 1]->count < r->count) {
            out[at] = out[at - 1];
            --at;
        }
        out[at] = r;
    }
    return n;
}

/* --- contents: stubs, IVT, BDA ------------------------------------------ */

void vbios_build_stubs(uint8_t *segment, const uint8_t hypercall[3])
{
    /* F000:0000 stays zero: ES:DI of the POST call points at no
     * "$PnP" installation structure (a non-PnP BIOS). Each stub hands the
     * call back with the backend's 3-byte hypercall (VMMCALL on
     * SVM, VMCALL on VMX), so every offset is the same on both. */
    static const uint8_t post[] = {0x9a, 0x03, 0x00, 0x00, 0xc0}; /* call far C000:0003 */
    static const uint8_t mode[] = {0xb8, 0x03, 0x00,              /* mov ax,0003h */
                                   0xcd, 0x10};                   /* int 10h */
    static const uint8_t cli_loop[] = {0xfa, 0xeb, 0xfe};         /* cli; jmp $ */
    memset(segment, 0, VBIOS_STUB_SIZE);
    memcpy(segment + VBIOS_POST_IP, post, sizeof(post));
    memcpy(segment + VBIOS_POST_DONE, hypercall, 3);
    memcpy(segment + VBIOS_MODE_IP, mode, sizeof(mode));
    memcpy(segment + VBIOS_MODE_DONE, hypercall, 3);
    memcpy(segment + VBIOS_CLI_IP, cli_loop, sizeof(cli_loop));
    segment[VBIOS_CALL10_IP] = 0xcd; /* int 10h; hypercall */
    segment[VBIOS_CALL10_IP + 1] = 0x10;
    memcpy(segment + VBIOS_CALL10_DONE, hypercall, 3);
    /* One "hypercall; iret" per vector at D000h + 4n, and the IBM
     * int 10h entry F065h (the image's int 42h target) likewise, so every
     * software interrupt the container serves reaches the platform. */
    for (unsigned v = 0; v < 256; ++v) {
        memcpy(segment + VBIOS_VECTOR_ENTRY(v), hypercall, 3);
        segment[VBIOS_VECTOR_ENTRY(v) + 3] = 0xcf;
    }
    memcpy(segment + VBIOS_INT10_IP, hypercall, 3); /* hypercall; iret */
    segment[VBIOS_INT10_IP + 3] = 0xcf;
    memcpy(segment + VBIOS_INT1A_IP, hypercall, 3); /* hypercall; iret */
    segment[VBIOS_INT1A_IP + 3] = 0xcf;
    segment[VBIOS_IRET_IP] = 0xcf; /* iret */
}

/* Guest BIOS handoff: the F000h segment, assembled from
 * tools/vbios/guest_stub.S. Plain x86 for the guest's own emulators. */
#include "platform/vbios_guest_stub.inc"

void vbios_build_guest_stub(uint8_t *segment, uint8_t last_bus)
{
    memset(segment, 0, VBIOS_STUB_SIZE);
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(vbios_guest_runs); ++i)
        memcpy(segment + vbios_guest_runs[i].offset, vbios_guest_runs[i].bytes,
               vbios_guest_runs[i].length);
    segment[VBIOS_GUEST_LAST_BUS] = last_bus;
#if defined(IA64VMM_VBIOS_TEST_BROKEN_GUEST_STUB)
    /* Test-only (tools/vbios/qemu_check.py --broken gueststub): find
     * device compares the vendor with -12(%bp) instead of the saved DX at
     * -10(%bp); the selftest on the real CPU must report the difference. */
    for (unsigned at = VBIOS_GUEST_INT1A; at + 2 < VBIOS_STUB_SIZE; ++at)
        if (segment[at] == 0x3b && segment[at + 1] == 0x46 && segment[at + 2] == 0xf6) {
            segment[at + 2] = 0xf4;
            break;
        }
#endif
}

unsigned vbios_redirect_ivt(uint8_t *ivt, unsigned *odd)
{
    /* The IVT the calls left points the container's services at its stub
     * segment: int 1Ah at F000:FE6E, the IBM int 10h entry F065h, the
     * per-vector entries F000:D000h + 4m and the bare iret at
     * FF53h. The guest's segment has int 1Ah at its own entry and the
     * "unsupported" handler (CF=1) for int 13h/15h/16h; an entry for
     * vector m is the service of m wherever the ROM copied it. Vectors the
     * ROM hooked (C000:xxxx) stay. A vector into F000h at any other offset
     * has no guest code behind it and gets the default iret (counted in
     * *odd). */
    unsigned changed = 0;
    *odd = 0;
    for (unsigned v = 0; v < 256; ++v) {
        uint16_t ip = le16(ivt + v * 4), cs = le16(ivt + v * 4 + 2), next;
        if (cs != VBIOS_STUB_SEGMENT)
            continue;
        unsigned service = v;
        bool entry = ip >= VBIOS_VECTOR_TABLE && ip < VBIOS_VECTOR_ENTRY(256) && !(ip & 3);
        if (entry)
            service = (unsigned)(ip - VBIOS_VECTOR_TABLE) / 4;
        if (ip == VBIOS_INT1A_IP || (entry && service == 0x1a))
            next = VBIOS_GUEST_INT1A;
        else if (ip == VBIOS_INT10_IP || (entry && service == 0x10))
            next = VBIOS_INT10_IP;
        else if (ip == VBIOS_IRET_IP || entry)
            next = service == 0x13 || service == 0x15 || service == 0x16 ? VBIOS_GUEST_UNSUPPORTED
                                                                         : VBIOS_IRET_IP;
        else {
            next = VBIOS_IRET_IP;
            ++*odd;
        }
        if (next != ip) {
            put16(ivt + v * 4, next);
            ++changed;
        }
    }
    return changed;
}

void vbios_build_low(uint8_t *low, uint32_t ticks)
{
    /* IVT and BDA as a PC BIOS initialises them (IBM PC AT
     * Technical Reference, BIOS data area); no EBDA. */
    memset(low, 0, 0x500);
    for (unsigned v = 0; v < 256; ++v) {
        uint16_t ip = v == 0x10   ? VBIOS_INT10_IP
                      : v == 0x1a ? VBIOS_INT1A_IP
                                  : (uint16_t)VBIOS_VECTOR_ENTRY(v);
        put16(low + v * 4, ip);
        put16(low + v * 4 + 2, VBIOS_STUB_SEGMENT);
    }
    put16(low + 0x410, 0x0002); /* equipment: x87; video bits 5:4 = EGA or later */
    put16(low + 0x413, VBIOS_LOW_SIZE / 1024);
    put16(low + 0x41a, 0x001e); /* keyboard buffer head */
    put16(low + 0x41c, 0x001e); /* keyboard buffer tail */
    put16(low + 0x480, 0x001e); /* keyboard buffer start */
    put16(low + 0x482, 0x003e); /* keyboard buffer end */
    put32(low + 0x46c, ticks);
}

/* --- time -------------------------------------------------------------- */

/* PIT input clock 1193182 Hz divided by 65536: the PC BIOS IRQ0 rate. */
uint64_t vbios_ticks(uint64_t elapsed, uint64_t hz)
{
    if (!hz)
        return 0;
    uint64_t whole = elapsed / hz, part = elapsed % hz;
    return (whole * 1193182 + part * 1193182 / hz) / 65536;
}

uint64_t vbios_tick_time(uint64_t tick, uint64_t hz)
{
    uint64_t scaled = tick * 65536;
    return scaled / 1193182 * hz + ((scaled % 1193182) * hz + 1193181) / 1193182;
}

uint32_t vbios_ticks_of_day(uint64_t seconds)
{
    return (uint32_t)(seconds % 86400 * 1193182 / 65536);
}

void vbios_store_ticks(uint8_t *low, uint32_t start, uint64_t elapsed_ticks, uint64_t *days)
{
    /* The IRQ0 handler wraps the count at 1800B0h and sets the midnight
     * flag at 0040:0070 (once per wrap; the reader clears it). */
    uint64_t total = start + elapsed_ticks;
    if (total / VBIOS_TICKS_PER_DAY > *days) {
        *days = total / VBIOS_TICKS_PER_DAY;
        low[0x470] = 1;
    }
    put32(low + 0x46c, (uint32_t)(total % VBIOS_TICKS_PER_DAY));
}

/* --- instruction lengths without NRIPS ----------------------------------- */

bool vbios_insn_length(const uint8_t *code, unsigned avail, enum x86rm_exit_reason reason,
                       unsigned *length)
{
    static const struct {
        enum x86rm_exit_reason reason;
        uint8_t n, bytes[3];
    } opcodes[] = {
        {X86RM_EXIT_CPUID, 2, {0x0f, 0xa2}},         {X86RM_EXIT_HLT, 1, {0xf4}},
        {X86RM_EXIT_VMMCALL, 3, {0x0f, 0x01, 0xd9}}, {X86RM_EXIT_INVD, 2, {0x0f, 0x08}},
        {X86RM_EXIT_WBINVD, 2, {0x0f, 0x09}},        {X86RM_EXIT_MSR, 2, {0x0f, 0x32}},
        {X86RM_EXIT_MSR, 2, {0x0f, 0x30}},
    };
    /* Legacy prefixes only: real mode has no REX, and the 15-byte limit
     * bounds the instruction (Intel SDM Vol.2 §2.1.1, AMD APM Vol.3 §1.2). */
    unsigned i = 0;
    while (i < avail && i < 14) {
        uint8_t b = code[i];
        if (b != 0x26 && b != 0x2e && b != 0x36 && b != 0x3e && b != 0x64 && b != 0x65 &&
            b != 0x66 && b != 0x67 && b != 0xf0 && b != 0xf2 && b != 0xf3)
            break;
        ++i;
    }
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(opcodes); ++k) {
        if (opcodes[k].reason != reason || i + opcodes[k].n > avail || i + opcodes[k].n > 15)
            continue;
        if (!memcmp(code + i, opcodes[k].bytes, opcodes[k].n)) {
            *length = i + opcodes[k].n;
            return true;
        }
    }
    return false;
}

/* --- int 1Ah PCI BIOS ---------------------------------------------------- */

static void set_ah(struct x86rm_regs *r, uint8_t ah)
{
    r->rax = (r->rax & ~UINT64_C(0xff00)) | (uint64_t)ah << 8;
}

/* The visible functions in ascending BDF order (find device/class). */
static unsigned visible_list(const struct vbios_card *card, const struct pci_device **out)
{
    unsigned n = 0;
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (vbios_visible((void *)card, d->bdf))
            out[n++] = d;
    }
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j && out[j - 1]->bdf > out[j]->bdf; --j) {
            const struct pci_device *t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

unsigned vbios_int1a(const struct vbios_card *card, struct x86rm_regs *r)
{
    /* PCI BIOS Specification 2.1 function numbers; status codes in AH:
     * 81h unsupported, 83h bad vendor, 86h not found, 87h bad register. */
    if (((r->rax >> 8) & 0xff) != 0xb1)
        return 0;
    unsigned function = r->rax & 0xff;
    uint16_t bdf = (uint16_t)(r->rbx & 0xffff);
    unsigned reg = (unsigned)(r->rdi & 0xffff);
    const struct pci_device *list[VBIOS_MAX_BRIDGES + 1];
    switch (function) {
    case 0x01:
        r->rax = (r->rax & ~UINT64_C(0xffff)) | 0x0001; /* AH=0, AL=1: mechanism #1 */
        r->rbx = (r->rbx & ~UINT64_C(0xffff)) | 0x0300; /* version 3.00 */
        r->rcx = (r->rcx & ~UINT64_C(0xff)) | card->last_bus;
        r->rdx = (r->rdx & ~UINT64_C(0xffffffff)) | 0x20494350; /* "PCI " */
        r->rdi &= ~UINT64_C(0xffffffff);                        /* no protected-mode entry */
        return 1;
    case 0x02:
    case 0x03: {
        uint16_t vendor = (uint16_t)r->rdx, device = (uint16_t)r->rcx;
        uint32_t cls = (uint32_t)r->rcx & 0xffffff;
        if (function == 0x02 && vendor == 0xffff) {
            set_ah(r, 0x83);
            return 2;
        }
        unsigned index = (unsigned)(r->rsi & 0xffff), n = visible_list(card, list);
        for (unsigned i = 0; i < n; ++i) {
            bool match = function == 0x02 ? list[i]->vendor == vendor && list[i]->device == device
                                          : list[i]->class_code == cls;
            if (!match || index--)
                continue;
            r->rbx = (r->rbx & ~UINT64_C(0xffff)) | list[i]->bdf;
            set_ah(r, 0);
            return 1;
        }
        set_ah(r, 0x86);
        return 2;
    }
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0b:
    case 0x0c:
    case 0x0d: {
        unsigned size = function == 0x08 || function == 0x0b   ? 1
                        : function == 0x09 || function == 0x0c ? 2
                                                               : 4;
        if (reg > 0xff || (reg & (size - 1))) {
            set_ah(r, 0x87);
            return 2;
        }
        bool visible = vbios_visible((void *)card, bdf);
        uint64_t off = ((uint64_t)bdf << 12) | reg;
        if (function <= 0x0a) {
            uint64_t value = !visible                       ? width_mask(size)
                             : readonly_function(card, bdf) ? platform_pci_host_read(bdf, reg, size)
                                                            : platform_pci_read(off, size);
            r->rcx = (r->rcx & ~width_mask(size)) | (value & width_mask(size));
        } else if (vbios_writable((void *)card, bdf))
            platform_pci_write(off, r->rcx & width_mask(size), size);
        set_ah(r, 0);
        return 1;
    }
    default:
        set_ah(r, 0x81);
        return 2;
    }
}

/* --- the container's other vectors -------------------------------- */

unsigned vbios_vector(unsigned vector, bool post_hooks, const uint8_t *low, struct x86rm_regs *r)
{
    unsigned ax = (unsigned)(r->rax & 0xffff), ch = (unsigned)(r->rcx >> 8) & 0xff;
    switch (vector) {
    case 0x15:
        if ((ax >> 8) != 0x5f) {
            /* IBM PC AT Technical Reference, int 15h: AH = 86h with CF set
             * for a function the system BIOS does not provide. */
            set_ah(r, 0x86);
            return 2;
        }
        /* Intel GMA hooks (coreboot drivers/intel/gma/int15.c): AX = 005Fh
         * and the answer in CX; coreboot's dispatcher clears CF for a
         * handled call and sets it for any other. Only vbios_call=post
         * answers, with coreboot's Intel GMA board defaults: panel fit
         * and boot display "VBIOS default" (0), active LFP 01h (integrated LVDS), mux/SG queries 0. Panel
         * type (5F40h) indexes the board's own VBT and stays unanswered. */
        if (!post_hooks)
            return 2;
        if (ax == 0x5f34 || ax == 0x5f35 || (ax == 0x5f70 && ch <= 2)) {
            r->rax = (r->rax & ~UINT64_C(0xffff)) | 0x005f;
            r->rcx &= ~UINT64_C(0xffff);
            return 1;
        }
        if (ax == 0x5f51) {
            r->rax = (r->rax & ~UINT64_C(0xffff)) | 0x005f;
            r->rcx = (r->rcx & ~UINT64_C(0xffff)) | 0x0001;
            return 1;
        }
        return 2;
    case 0x11:
    case 0x12:
        /* Equipment list and base memory size from the BDA (0410h, 0413h),
         * as a PC BIOS answers them; FLAGS as the caller left them. */
        r->rax = (r->rax & ~UINT64_C(0xffff)) | le16(low + (vector == 0x11 ? 0x410 : 0x413));
        return 0;
    case 0x13:
    case 0x14:
    case 0x16:
    case 0x17:
        return 2; /* disk, serial, keyboard, printer: none (the guest stub's E400h) */
    default:
        return 0; /* what the bare IRET did, now logged */
    }
}

/* --- the session ---------------------------------------------------------- */

#define EVENT_RING 32u
#define PORT_STATS 64u
#define DETAIL_LIMIT 32u

enum event_kind {
    EV_IN,
    EV_OUT,
    EV_CONFIG_IN,
    EV_CONFIG_OUT,
    EV_MSR_READ,
    EV_MSR_WRITE,
    EV_CPUID,
    EV_INT1A,
    EV_HLT,
    EV_CACHE,
    EV_INT
};
static const char *const event_names[] = {"in",    "out",    "config-in", "config-out",
                                          "rdmsr", "wrmsr",  "cpuid",     "int1a",
                                          "hlt",   "wbinvd", "int"};
struct event {
    uint8_t kind, size;
    uint16_t cs;
    uint32_t ip;
    uint64_t a, b;
};
struct port_stat {
    uint16_t port;
    uint32_t reads, writes;
};

static struct session {
    struct vbios_card card;
    struct vbios_layout layout;
    uint8_t *low, *shadow, *stub;
    uint32_t shadow_size;
    struct x86rm *rm;
    uint8_t hypercall[3]; /* the backend's */
    struct x86rm_regs regs;
    uint32_t latch, tick_start;
    uint64_t hz, start, days;
    struct port_stat ports[PORT_STATS];
    unsigned port_count;
    uint64_t ports_lost;
    struct event events[EVENT_RING];
    uint64_t event_count;
    uint64_t exits[X86RM_EXIT_OTHER + 1];
    uint64_t config_accesses, int1a_calls, hlt_waits;
    unsigned config_logged, detail_logged;
    bool post_hooks;            /* vbios_call=post: the int 15h 5Fxx answers */
    unsigned text_hold;         /* vbios_text_hold=N seconds (mode set only), 0 = off */
    bool text_keep;             /* vbios_text_hold=N,keep: mode 3 stays after the hold */
    uint64_t vector_calls[256]; /* software INTs served by the container's entries */
    unsigned vector_logged;
    bool reset, reset_done, reset_failed; /* vbios_reset=sbr */
    struct vbios_io_trace trace;          /* vbios_trace_io=N */
    uint16_t trace_base[6], trace_size[6];
    unsigned trace_ranges;
} session;
static struct platform_vbios_state state;
const struct platform_vbios_state *platform_vbios_state(void)
{
    return &state;
}
enum selftest { SELFTEST_NONE, SELFTEST_CLI_LOOP, SELFTEST_GUEST_STUB, SELFTEST_GUEST_TRACE };
/* vbios_image= and vbios_call=. */
enum image_source { IMAGE_ROM, IMAGE_HOST };
enum call_kind { CALL_KIND_POST, CALL_KIND_MODE };
/* What a guest restart repeats: VMM start's
 * successful call and its options. */
struct restart_call {
    bool valid;
    const struct boot_info *bi;
    uint16_t bdf;
    enum image_source source;
    enum call_kind call;
    uint64_t post_ms, mode_ms;
};
static struct restart_call again;

static const char *const exit_names[] = {
    [X86RM_EXIT_IO] = "IO",           [X86RM_EXIT_MSR] = "MSR",
    [X86RM_EXIT_CPUID] = "CPUID",     [X86RM_EXIT_HLT] = "HLT",
    [X86RM_EXIT_VMMCALL] = "VMMCALL", [X86RM_EXIT_INVD] = "INVD",
    [X86RM_EXIT_WBINVD] = "WBINVD",   [X86RM_EXIT_INTR] = "INTR",
    [X86RM_EXIT_NMI] = "NMI",         [X86RM_EXIT_EXCEPTION] = "exception",
    [X86RM_EXIT_NPF] = "NPF",         [X86RM_EXIT_SHUTDOWN] = "shutdown",
    [X86RM_EXIT_INVALID] = "invalid", [X86RM_EXIT_OTHER] = "other",
};

static uint8_t *guest(struct session *s, uint64_t gpa, uint64_t length)
{
    for (unsigned i = 0; i < s->layout.map_count; ++i) {
        const struct x86rm_mapping *m = &s->layout.map[i];
        if (m->hva && gpa >= m->gpa && length <= m->size && gpa - m->gpa <= m->size - length)
            return m->hva + (gpa - m->gpa);
    }
    return NULL;
}
static unsigned fetch(struct session *s, uint8_t *code, unsigned max)
{
    unsigned n = 0;
    for (; n < max; ++n) {
        uint64_t ip = s->regs.rip + n;
        if (!(s->regs.cs.attrib & 0x400))
            ip &= 0xffff;
        const uint8_t *p = guest(s, s->regs.cs.base + ip, 1);
        if (!p)
            break;
        code[n] = *p;
    }
    return n;
}
static void record(struct session *s, enum event_kind kind, unsigned size, uint64_t a, uint64_t b)
{
    struct event *e = &s->events[s->event_count++ % EVENT_RING];
    *e = (struct event){
        (uint8_t)kind, (uint8_t)size, s->regs.cs.selector, (uint32_t)s->regs.rip, a, b};
}
static void tick(struct session *s)
{
    vbios_store_ticks(s->low, s->tick_start, vbios_ticks(hal_counter() - s->start, s->hz),
                      &s->days);
}

/* The kept accesses (in the failure dump), or the registers by
 * access count (after the exit summary, so that the screen replay of the
 * attempt's last 40 lines shows them). */
static void trace_dump(struct session *s, bool events)
{
    const struct vbios_io_trace *t = &s->trace;
    if (!t->limit)
        return;
    if (events) {
        static const struct vbios_io_event *kept[2 * VBIOS_TRACE_MAX];
        unsigned n = vbios_trace_kept(t, kept, IA64VMM_ARRAY_SIZE(kept));
        LOGE(
            "VBIOS trace: %llu access(es) to the card's I/O BAR; the first %u and the last %u (seq CS:IP dir "
            "port size value [index]):",
            (unsigned long long)t->count, t->limit, t->limit);
        for (unsigned i = 0; i < n; ++i) {
            const struct vbios_io_event *e = kept[i];
            if (i && e->seq != kept[i - 1]->seq + 1)
                LOGE("VBIOS trace   ... %llu access(es) not kept",
                     (unsigned long long)(e->seq - kept[i - 1]->seq - 1));
            if (e->paired)
                LOGE("VBIOS trace %llu %04x:%04x %s %04x/%u %08x reg %08x",
                     (unsigned long long)e->seq, e->cs, e->ip, e->write ? "out" : "in ", e->port,
                     e->size, e->value, e->index);
            else
                LOGE("VBIOS trace %llu %04x:%04x %s %04x/%u %08x", (unsigned long long)e->seq,
                     e->cs, e->ip, e->write ? "out" : "in ", e->port, e->size, e->value);
        }
        return;
    }
    static const struct vbios_io_reg *top[12];
    unsigned n = vbios_trace_top(t, top, IA64VMM_ARRAY_SIZE(top));
    /* The first accesses on one line: NVIDIA's window unlock (key at +0,
     * enable at +4) and the first register read show up here. */
    char first[160];
    int used = 0;
    for (unsigned i = 0; i < 6 && i < t->count && i < t->limit && used < (int)sizeof(first); ++i)
        used +=
            vmm_snprintf(first + used, sizeof(first) - (size_t)used, "%s%s %04x %x", i ? ", " : "",
                         t->first[i].write ? "out" : "in", t->first[i].port, t->first[i].value);
    LOGI("VBIOS trace summary: %llu access(es), %u register(s)%s; first: %s",
         (unsigned long long)t->count, t->reg_count, t->regs_lost ? " (table full)" : "",
         t->count ? first : "none");
    for (unsigned i = 0; i < n; ++i) {
        const struct vbios_io_reg *r = top[i];
        if (r->paired)
            LOGI(
                "VBIOS trace summary: reg %08x %s %llux at port %04x, first %08x last %08x%s, last at %04x:%04x",
                r->index, r->write ? "written" : "read", (unsigned long long)r->count, r->port,
                r->first_value, r->last_value, r->changed ? "" : " (never changed)", r->last_cs,
                r->last_ip);
        else
            LOGI(
                "VBIOS trace summary: port %04x %s %llux, first %08x last %08x%s, last at %04x:%04x",
                r->port, r->write ? "written" : "read", (unsigned long long)r->count,
                r->first_value, r->last_value, r->changed ? "" : " (never changed)", r->last_cs,
                r->last_ip);
    }
}

static void dump(struct session *s, const char *phase, const char *why)
{
    const struct x86rm_regs *r = &s->regs;
    LOGE("VBIOS %s failed: %s", phase, why);
    LOGE("VBIOS CS:IP=%04x:%04llx (linear %llx) SS:SP=%04x:%04llx FLAGS=%04llx CR0=%llx",
         r->cs.selector, (unsigned long long)r->rip, (unsigned long long)(r->cs.base + r->rip),
         r->ss.selector, (unsigned long long)r->rsp, (unsigned long long)r->rflags,
         (unsigned long long)r->cr0);
    LOGE("VBIOS EAX=%08llx EBX=%08llx ECX=%08llx EDX=%08llx ESI=%08llx EDI=%08llx EBP=%08llx "
         "DS=%04x ES=%04x FS=%04x GS=%04x",
         (unsigned long long)(r->rax & 0xffffffff), (unsigned long long)(r->rbx & 0xffffffff),
         (unsigned long long)(r->rcx & 0xffffffff), (unsigned long long)(r->rdx & 0xffffffff),
         (unsigned long long)(r->rsi & 0xffffffff), (unsigned long long)(r->rdi & 0xffffffff),
         (unsigned long long)(r->rbp & 0xffffffff), r->ds.selector, r->es.selector, r->fs.selector,
         r->gs.selector);
    uint8_t code[16];
    unsigned n = fetch(s, code, sizeof(code));
    char text[16 * 3 + 1];
    for (unsigned i = 0; i < n; ++i)
        vmm_snprintf(text + i * 3, 4, " %02x", code[i]);
    text[n * 3] = 0;
    LOGE("VBIOS code at CS:IP:%s", n ? text : " (not in VMM-backed memory)");
    uint64_t first = s->event_count > EVENT_RING ? s->event_count - EVENT_RING : 0;
    LOGE("VBIOS last %llu of %llu logged events:", (unsigned long long)(s->event_count - first),
         (unsigned long long)s->event_count);
    for (uint64_t i = first; i < s->event_count; ++i) {
        const struct event *e = &s->events[i % EVENT_RING];
        LOGE("VBIOS   %04x:%04x %s %llx size=%u value=%llx", e->cs, e->ip, event_names[e->kind],
             (unsigned long long)e->a, e->size, (unsigned long long)e->b);
    }
    trace_dump(s, true);
}

static void summary(struct session *s)
{
    char text[256];
    int at = 0;
    for (unsigned k = 0; k <= X86RM_EXIT_OTHER; ++k)
        if (s->exits[k] && at < (int)sizeof(text))
            at += vmm_snprintf(text + at, sizeof(text) - (size_t)at, " %s=%llu", exit_names[k],
                               (unsigned long long)s->exits[k]);
    LOGI("VBIOS exits:%s; config accesses %llu, int 1Ah calls %llu, HLT waits %llu",
         at ? text : " none", (unsigned long long)s->config_accesses,
         (unsigned long long)s->int1a_calls, (unsigned long long)s->hlt_waits);
    at = 0;
    for (unsigned v = 0; v < 256; ++v)
        if (s->vector_calls[v] && at < (int)sizeof(text))
            at += vmm_snprintf(text + at, sizeof(text) - (size_t)at, " %02xh=%llu", v,
                               (unsigned long long)s->vector_calls[v]);
    if (at)
        LOGI("VBIOS software interrupts served by the container:%s", text);
    for (unsigned i = 0; i < s->port_count; ++i)
        LOGI("VBIOS port %04x denied: %u reads, %u writes", s->ports[i].port, s->ports[i].reads,
             s->ports[i].writes);
    if (s->ports_lost)
        LOGI("VBIOS %llu further denied accesses on unlisted ports",
             (unsigned long long)s->ports_lost);
}

static bool advance(struct session *s, const struct x86rm_exit *e, const char **why)
{
    if (e->next_rip_valid) {
        s->regs.rip = e->next_rip;
        return true;
    }
    /* No NRIPS (QEMU TCG): decode the length of the intercepted opcode. */
    uint8_t code[15];
    unsigned length, n = fetch(s, code, sizeof(code));
    if (!vbios_insn_length(code, n, e->reason, &length)) {
        *why = "the instruction at CS:IP is not the one the exit names";
        return false;
    }
    uint64_t next = s->regs.rip + length;
    s->regs.rip = s->regs.cs.attrib & 0x400 ? next : next & 0xffff;
    return true;
}

static bool traced(const struct session *s, uint16_t port, unsigned size)
{
    for (unsigned i = 0; i < s->trace_ranges; ++i)
        if (port >= s->trace_base[i] &&
            (unsigned)port + size <= (unsigned)s->trace_base[i] + s->trace_size[i])
            return true;
    return false;
}

static bool port_io(struct session *s, const struct x86rm_exit *e, const char **why)
{
    if (e->string) {
        *why = traced(s, e->port, e->size ? e->size : 1)
                   ? "string I/O (INS/OUTS) on the card's I/O BAR, which vbios_trace_io intercepts"
                   : "string I/O (INS/OUTS) on an intercepted port";
        return false;
    }
    if (!e->size) {
        *why = "I/O exit without an operand size";
        return false;
    }
    uint64_t mask = width_mask(e->size), value = e->in ? mask : s->regs.rax & mask;
    uint32_t sequence;
    if (e->in && vbios_guest_trace_sequence(NULL, e->port, e->size, &sequence)) {
        value = sequence;
    } else if (!e->in && vbios_guest_trace_port(NULL, e->port, e->size, true, (uint32_t)value)) {
        /* The diagnostic observer's packet, not a hardware port write. */
    } else if (traced(s, e->port, e->size)) {
        /* The card's own port, forwarded unchanged, then recorded. */
        if (e->in)
            value = hal_port_in(e->port, e->size) & mask;
        else
            hal_port_out(e->port, (uint32_t)value, e->size);
        vbios_trace_record(&s->trace, s->regs.cs.selector, (uint16_t)s->regs.rip, e->port, e->size,
                           !e->in, (uint32_t)value);
    } else if (platform_config_ports(e->port, e->size)) {
        /* The guest's own mediation, restricted to the card's
         * function and its bridges. */
        uint16_t bdf = (uint16_t)((s->latch >> 8) & 0xffff);
        unsigned reg = (s->latch & 0xfc) | (e->port & 3);
        bool data = e->port >= 0xcfc, enabled = s->latch & 0x80000000u;
        bool reached =
            enabled && (e->in ? vbios_visible(&s->card, bdf) : vbios_writable(&s->card, bdf));
        bool readonly = enabled && !e->in && !reached && vbios_visible(&s->card, bdf);
        if (e->in)
            value = vbios_config_read(&s->card, &s->latch, e->port, e->size);
        else
            vbios_config_write(&s->card, &s->latch, e->port, value, e->size);
        ++s->config_accesses;
        record(s, e->in ? EV_CONFIG_IN : EV_CONFIG_OUT, e->size,
               data ? (uint64_t)bdf << 12 | reg : e->port, value);
        if (data && s->config_logged < DETAIL_LIMIT) {
            ++s->config_logged;
            LOGI("VBIOS config %s %02x:%02x.%u reg %02x size %u value %llx%s",
                 e->in ? "read" : "write", bdf >> 8, (bdf >> 3) & 31, bdf & 7, reg, e->size,
                 (unsigned long long)value,
                 reached    ? ""
                 : readonly ? " (read-only to the VBIOS: dropped)"
                            : " (not the card or its bridge: all-ones/dropped)");
        }
    } else {
        /* Every intercepted port is denied and reads all-ones. */
        record(s, e->in ? EV_IN : EV_OUT, e->size, e->port, value);
        struct port_stat *stat = NULL;
        for (unsigned i = 0; i < s->port_count; ++i)
            if (s->ports[i].port == e->port)
                stat = &s->ports[i];
        if (!stat && s->port_count < PORT_STATS) {
            stat = &s->ports[s->port_count++];
            *stat = (struct port_stat){e->port, 0, 0};
            LOGI("VBIOS port %04x first %s size %u value %llx at %04x:%04llx: %s", e->port,
                 e->in ? "read" : "write", e->size, (unsigned long long)value, s->regs.cs.selector,
                 (unsigned long long)s->regs.rip, "denied (reads all-ones, writes dropped)");
        }
        if (!stat)
            ++s->ports_lost;
        else if (e->in)
            ++stat->reads;
        else
            ++stat->writes;
    }
    if (e->in)
        s->regs.rax = (s->regs.rax & ~mask) | (value & mask);
    s->regs.rip = e->next_rip;
    return true;
}

static bool int1a(struct session *s, const char **why)
{
    struct x86rm_regs before = s->regs;
    unsigned result = vbios_int1a(&s->card, &s->regs);
    ++s->int1a_calls;
    record(s, EV_INT1A, 2, before.rax & 0xffff, s->regs.rax & 0xffff);
    if (s->detail_logged < DETAIL_LIMIT) {
        ++s->detail_logged;
        LOGI(
            "VBIOS int 1Ah AX=%04llx BX=%04llx CX=%04llx DX=%04llx SI=%04llx DI=%04llx -> %s AH=%02llx "
            "BX=%04llx ECX=%08llx EDX=%08llx",
            (unsigned long long)(before.rax & 0xffff), (unsigned long long)(before.rbx & 0xffff),
            (unsigned long long)(before.rcx & 0xffff), (unsigned long long)(before.rdx & 0xffff),
            (unsigned long long)(before.rsi & 0xffff), (unsigned long long)(before.rdi & 0xffff),
            result == 0   ? "not provided (registers unchanged)"
            : result == 1 ? "CF=0"
                          : "CF=1",
            (unsigned long long)((s->regs.rax >> 8) & 0xff),
            (unsigned long long)(s->regs.rbx & 0xffff),
            (unsigned long long)(s->regs.rcx & 0xffffffff),
            (unsigned long long)(s->regs.rdx & 0xffffffff));
    }
    if (result) {
        /* The stub's IRET reloads FLAGS from SS:SP+4 (IP, CS, FLAGS). */
        uint8_t *flags = guest(s, s->regs.ss.base + ((s->regs.rsp + 4) & 0xffff), 2);
        if (!flags) {
            *why = "int 1Ah stack outside VMM-backed memory";
            return false;
        }
        put16(flags, (uint16_t)(result == 2 ? le16(flags) | 1 : le16(flags) & ~1u));
    }
    s->regs.rip += 3;
    return true;
}

/* A software INT n that reached the container's entry for n. The
 * INT pushed FLAGS, CS, IP: the caller is at SS:SP. */
static bool vector_call(struct session *s, unsigned vector, const char **why)
{
    uint8_t *frame = guest(s, s->regs.ss.base + (s->regs.rsp & 0xffff), 6);
    if (!frame) {
        *why = "software interrupt stack outside VMM-backed memory";
        return false;
    }
    struct x86rm_regs before = s->regs;
    unsigned result = vbios_vector(vector, s->post_hooks, s->low, &s->regs);
    ++s->vector_calls[vector];
    record(s, EV_INT, 2, vector, before.rax & 0xffff);
    if (s->vector_logged < DETAIL_LIMIT) {
        ++s->vector_logged;
        LOGI(
            "VBIOS int %02xh AX=%04llx BX=%04llx CX=%04llx DX=%04llx from %04x:%04x (call %llu) -> %s, AX=%04llx "
            "CX=%04llx",
            vector, (unsigned long long)(before.rax & 0xffff),
            (unsigned long long)(before.rbx & 0xffff), (unsigned long long)(before.rcx & 0xffff),
            (unsigned long long)(before.rdx & 0xffff), le16(frame + 2), le16(frame),
            (unsigned long long)s->vector_calls[vector],
            result == 0   ? "registers and FLAGS unchanged"
            : result == 1 ? "CF=0"
                          : "CF=1",
            (unsigned long long)(s->regs.rax & 0xffff), (unsigned long long)(s->regs.rcx & 0xffff));
    }
    if (result)
        put16(frame + 4, (uint16_t)(result == 2 ? le16(frame + 4) | 1 : le16(frame + 4) & ~1u));
    s->regs.rip += 3;
    return true;
}

static void set_segment(struct x86rm_segment *seg, uint16_t selector, uint16_t attrib)
{
    *seg = (struct x86rm_segment){selector, attrib, 0xffff, (uint64_t)selector << 4};
}

/* A fresh real-mode call from the stub segment. */
static void call_registers(struct x86rm_regs *r, uint16_t ip)
{
    memset(r, 0, sizeof(*r));
    set_segment(&r->cs, VBIOS_STUB_SEGMENT, 0x9b);
    set_segment(&r->ds, 0, 0x93);
    set_segment(&r->es, 0, 0x93);
    set_segment(&r->ss, 0, 0x93);
    set_segment(&r->fs, 0, 0x93);
    set_segment(&r->gs, 0, 0x93);
    r->gdtr = (struct x86rm_segment){0, 0, 0xffff, 0};
    r->idtr = (struct x86rm_segment){0, 0, 0x3ff, 0};
    r->rip = ip;
    r->rsp = VBIOS_STACK_TOP;
    r->rflags = 0x202;
    r->cr0 = 0x10;
}

enum outcome { CALL_DONE, CALL_FAILED, CALL_BOUND };

static enum outcome run_call(struct session *s, const char *phase, uint16_t done_ip,
                             uint64_t bound_ms, uint64_t *elapsed_ms)
{
    uint64_t begin = hal_counter();
    uint64_t bound = bound_ms / 1000 * s->hz + bound_ms % 1000 * s->hz / 1000;
    const char *why = NULL;
    for (;;) {
        tick(s);
        uint64_t now = hal_counter();
        *elapsed_ms = s->hz ? (now - begin) / (s->hz / 1000 ? s->hz / 1000 : 1) : 0;
        if (now - begin >= bound)
            return CALL_BOUND;
        struct x86rm_exit e;
        if (!hal_x86rm_run(s->rm, &s->regs, &e)) {
            why = "the container backend failed";
            break;
        }
        tick(s);
        if ((unsigned)e.reason <= X86RM_EXIT_OTHER)
            ++s->exits[e.reason];
        bool ok = true;
        switch (e.reason) {
        case X86RM_EXIT_IO:
            ok = port_io(s, &e, &why);
            break;
        case X86RM_EXIT_VMMCALL: {
            uint64_t linear = s->regs.cs.base + s->regs.rip;
            if (done_ip && linear == VBIOS_STUB_BASE + done_ip) {
                *elapsed_ms = (hal_counter() - begin) / (s->hz / 1000 ? s->hz / 1000 : 1);
                return CALL_DONE;
            }
            uint64_t table = VBIOS_STUB_BASE + VBIOS_VECTOR_TABLE;
            unsigned vector = (unsigned)((linear - table) / 4);
            bool entry = linear >= table && linear < table + 0x400 && !((linear - table) & 3);
            if (linear == VBIOS_STUB_BASE + VBIOS_INT1A_IP || (entry && vector == 0x1a))
                ok = int1a(s, &why);
            else if (entry)
                ok = vector_call(s, vector, &why);
            else if (linear == VBIOS_STUB_BASE + VBIOS_INT10_IP)
                ok = vector_call(s, 0x10, &why);
            else {
                why = "hypercall outside the stub addresses";
                ok = false;
            }
            break;
        }
        case X86RM_EXIT_CPUID:
            record(s, EV_CPUID, 4, s->regs.rax & 0xffffffff, e.cpuid[0]);
            if (s->detail_logged < DETAIL_LIMIT) {
                ++s->detail_logged;
                LOGI("VBIOS CPUID %08llx/%08llx -> %08x %08x %08x %08x (host values)",
                     (unsigned long long)(s->regs.rax & 0xffffffff),
                     (unsigned long long)(s->regs.rcx & 0xffffffff), e.cpuid[0], e.cpuid[1],
                     e.cpuid[2], e.cpuid[3]);
            }
            s->regs.rax = e.cpuid[0];
            s->regs.rbx = e.cpuid[1];
            s->regs.rcx = e.cpuid[2];
            s->regs.rdx = e.cpuid[3];
            ok = advance(s, &e, &why);
            break;
        case X86RM_EXIT_MSR: {
            uint32_t msr = (uint32_t)s->regs.rcx;
            uint64_t value = (s->regs.rdx & 0xffffffff) << 32 | (s->regs.rax & 0xffffffff);
            record(s, e.msr_write ? EV_MSR_WRITE : EV_MSR_READ, 8, msr, e.msr_write ? value : 0);
            if (s->detail_logged < DETAIL_LIMIT) {
                ++s->detail_logged;
                LOGI("VBIOS %s MSR %08x value %llx denied: %s", e.msr_write ? "WRMSR" : "RDMSR",
                     msr, (unsigned long long)(e.msr_write ? value : 0),
                     e.msr_write ? "dropped" : "reads 0");
            }
            if (!e.msr_write) {
                s->regs.rax = 0;
                s->regs.rdx = 0;
            }
            ok = advance(s, &e, &why);
            break;
        }
        case X86RM_EXIT_HLT: {
            if (!(s->regs.rflags & 0x200)) {
                why = "HLT with interrupts disabled: the ROM stopped itself";
                ok = false;
                break;
            }
            /* A PC wakes from HLT at its next IRQ0, the BIOS tick. */
            uint64_t current = vbios_ticks(hal_counter() - s->start, s->hz);
            uint64_t wake = s->start + vbios_tick_time(current + 1, s->hz);
            while (hal_counter() < wake)
                hal_idle_until(wake);
            ++s->hlt_waits;
            record(s, EV_HLT, 0, current + 1, 0);
            ok = advance(s, &e, &why);
            break;
        }
        case X86RM_EXIT_INVD:
        case X86RM_EXIT_WBINVD:
            record(s, EV_CACHE, 0, e.reason == X86RM_EXIT_INVD, 0);
            if (s->detail_logged < DETAIL_LIMIT) {
                ++s->detail_logged;
                LOGI("VBIOS %s at %04x:%04llx: the host executed WBINVD",
                     e.reason == X86RM_EXIT_INVD ? "INVD" : "WBINVD", s->regs.cs.selector,
                     (unsigned long long)s->regs.rip);
            }
            ok = advance(s, &e, &why);
            break;
        case X86RM_EXIT_INTR:
            break;
        case X86RM_EXIT_NMI:
            LOGW("VBIOS: host NMI during %s", phase);
            break;
        case X86RM_EXIT_EXCEPTION: {
            static char text[96];
            vmm_snprintf(text, sizeof(text), "exception vector %u, error code %llx", e.vector,
                         (unsigned long long)e.info1);
            why = text;
            ok = false;
            break;
        }
        case X86RM_EXIT_NPF: {
            static char text[128];
            vmm_snprintf(text, sizeof(text),
                         "nested page fault: %s %s guest physical %llx, outside the map",
                         e.npf_fetch   ? "fetch"
                         : e.npf_write ? "write"
                                       : "read",
                         e.npf_present ? "(read-only)" : "at", (unsigned long long)e.gpa);
            why = text;
            ok = false;
            break;
        }
        case X86RM_EXIT_SHUTDOWN:
            why = "shutdown (triple fault)";
            ok = false;
            break;
        case X86RM_EXIT_INVALID: {
            static char text[96];
            vmm_snprintf(text, sizeof(text),
                         "the hardware refused the guest state (exit code %llx, info %llx)",
                         (unsigned long long)e.code, (unsigned long long)e.info1);
            why = text;
            ok = false;
            break;
        }
        default: {
            static char text[64];
            vmm_snprintf(text, sizeof(text), "unexpected exit code %llx",
                         (unsigned long long)e.code);
            why = text;
            ok = false;
            break;
        }
        }
        if (!ok)
            break;
    }
    dump(s, phase, why);
    return CALL_FAILED;
}

/* --- selection and the flow ------------------------------------------------ */

static const struct pci_device *select_card(const struct boot_framebuffer *fb)
{
    /* The VGA controller whose memory BAR holds the inherited GOP
     * framebuffer (as platform/vga.c), or the only VGA controller when
     * there is no framebuffer to match. An expansion ROM BAR is not a
     * condition (an IGD has none); it picks the default image. */
    const struct pci_device *gop = NULL, *only = NULL;
    unsigned candidates = 0;
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (d->owner != PCI_OWNER_GUEST || d->class_code >> 8 != 0x0300)
            continue;
        ++candidates;
        only = d;
        for (unsigned b = 0; fb->size && b < 6; ++b) {
            const struct pci_bar *bar = &d->bars[b];
            if (bar->io || !bar->size || fb->base < bar->base || fb->size > bar->size ||
                fb->base - bar->base > bar->size - fb->size)
                continue;
            if (gop && gop != d) {
                LOGE("VBIOS: two VGA controllers hold the GOP framebuffer");
                return NULL;
            }
            gop = d;
        }
    }
    if (gop)
        return gop;
    if (candidates == 1)
        return only;
    LOGE("VBIOS: %u VGA controllers and none holds the GOP framebuffer", candidates);
    return NULL;
}

static void fabric_diagnostic(void)
{
    /* Read only. coreboot's Turin DF_VGA_EN layout; unverified
     * for any other AMD data fabric, so nothing is written. */
    uint32_t id = platform_pci_host_read(0x18 << 3, 0, 4);
    if ((id & 0xffff) != 0x1022)
        return;
    uint32_t value = platform_pci_host_read(0x18 << 3, 0xc08, 4);
    if (value == UINT32_MAX)
        LOGI(
            "VBIOS: AMD data fabric 00:18.0 1022:%04x, D18F0x0C08 unreadable (no ECAM); not written",
            id >> 16);
    else
        LOGI(
            "VBIOS: AMD data fabric 00:18.0 1022:%04x D18F0x0C08=%08x; as Turin DF_VGA_EN: VE=%u NP=%u "
            "CpuDis=%u DstFabricID=%03x (layout unverified for this device; not written)",
            id >> 16, value, value & 1, (value >> 1) & 1, (value >> 2) & 1, (value >> 4) & 0xfff);
}

static bool option_ms(const char *line, const char *key, uint64_t *ms)
{
    const char *value;
    size_t n;
    if (!platform_cmdline_value(line, key, &value, &n))
        return true;
    uint64_t v = 0;
    bool ok = n && n <= 6;
    for (size_t i = 0; ok && i < n; ++i) {
        ok = value[i] >= '0' && value[i] <= '9';
        v = v * 10 + (uint64_t)(value[i] - '0');
    }
    if (!ok || !v || v > 600000) {
        LOGE("%s takes 1..600000 (milliseconds)", key);
        return false;
    }
    *ms = v;
    return true;
}

static void release_pages(struct session *s)
{
    if (s->low)
        hal_free_pages(s->low, VBIOS_LOW_SIZE / 4096);
    if (s->shadow)
        hal_free_pages(s->shadow, s->shadow_size / 4096);
    if (s->stub)
        hal_free_pages(s->stub, VBIOS_STUB_SIZE / 4096);
    s->low = s->shadow = s->stub = NULL;
}

static bool post(struct session *s, const struct boot_info *bi, const struct pci_device *card,
                 enum image_source source, enum call_kind call, uint64_t post_ms, uint64_t mode_ms,
                 enum selftest selftest);
bool platform_vbios_post(const struct boot_info *bi)
{
    again.valid = false;
    memset(&state, 0, sizeof(state));
    if (!vbios_guest_trace_init(bi->cmdline))
        return false;
    platform_pci_boot_framebuffer(&bi->fb);
    const char *value;
    size_t n;
    bool vbios = false;
    enum selftest selftest = SELFTEST_NONE;
    if (platform_cmdline_value(bi->cmdline, "display", &value, &n)) {
        if (n == 5 && !strncmp(value, "vbios", n))
            vbios = true;
        else if (n != 3 || strncmp(value, "gop", n)) {
            LOGE("display takes gop or vbios");
            return false;
        }
    }
    uint64_t post_ms = 30000, mode_ms = 10000;
    if (!option_ms(bi->cmdline, "vbios_post_ms", &post_ms) ||
        !option_ms(bi->cmdline, "vbios_mode_ms", &mode_ms))
        return false;
    if (platform_cmdline_value(bi->cmdline, "vbios_selftest", &value, &n)) {
        if (n == 8 && !strncmp(value, "cli-loop", n))
            selftest = SELFTEST_CLI_LOOP;
        else if (n == 10 && !strncmp(value, "guest-stub", n))
            selftest = SELFTEST_GUEST_STUB;
        else if (n == 11 && !strncmp(value, "guest-trace", n))
            selftest = SELFTEST_GUEST_TRACE;
        else {
            LOGE("vbios_selftest takes cli-loop, guest-stub or guest-trace");
            return false;
        }
    }
    bool reset = false;
    unsigned trace = 0;
    if (platform_cmdline_value(bi->cmdline, "vbios_reset", &value, &n)) {
        if (n == 3 && !strncmp(value, "sbr", n))
            reset = true;
        else {
            LOGE("vbios_reset takes sbr");
            return false;
        }
    }
    if (platform_cmdline_value(bi->cmdline, "vbios_trace_io", &value, &n)) {
        bool ok = n && n <= 4;
        for (size_t i = 0; ok && i < n; ++i) {
            ok = value[i] >= '0' && value[i] <= '9';
            trace = trace * 10 + (unsigned)(value[i] - '0');
        }
        if (!ok || !trace || trace > VBIOS_TRACE_MAX) {
            LOGE("vbios_trace_io takes 1..%u", VBIOS_TRACE_MAX);
            return false;
        }
        /* Every access to the card's I/O BAR exits now: a longer default
         * bound without requiring another command-line token. */
        if (!platform_cmdline_value(bi->cmdline, "vbios_post_ms", &value, &n))
            post_ms = 120000;
    }
    /* vbios_text_hold=N[,keep]: with ",keep" mode 3 stays after the hold
     * and the run goes on as without the option. */
    unsigned hold = 0;
    bool keep = false;
    if (platform_cmdline_value(bi->cmdline, "vbios_text_hold", &value, &n)) {
        if (n > 5 && !strncmp(value + n - 5, ",keep", 5)) {
            keep = true;
            n -= 5;
        }
        bool ok = n && n <= 3;
        for (size_t i = 0; ok && i < n; ++i) {
            ok = value[i] >= '0' && value[i] <= '9';
            hold = hold * 10 + (unsigned)(value[i] - '0');
        }
        if (!ok || !hold || hold > 600) {
            LOGE("vbios_text_hold takes 1..600 seconds, optionally ,keep");
            return false;
        }
    }
    /* The image (ROM BAR or the host's C0000) and the call (POST
     * then mode set, or the mode set on the live state); defaults follow
     * the card. */
    int image_opt = -1, call_opt = -1;
    if (platform_cmdline_value(bi->cmdline, "vbios_image", &value, &n)) {
        if (n == 3 && !strncmp(value, "rom", n))
            image_opt = IMAGE_ROM;
        else if (n == 4 && !strncmp(value, "host", n))
            image_opt = IMAGE_HOST;
        else {
            LOGE("vbios_image takes rom or host");
            return false;
        }
    }
    if (platform_cmdline_value(bi->cmdline, "vbios_call", &value, &n)) {
        if (n == 4 && !strncmp(value, "post", n))
            call_opt = CALL_KIND_POST;
        else if (n == 4 && !strncmp(value, "mode", n))
            call_opt = CALL_KIND_MODE;
        else {
            LOGE("vbios_call takes post or mode");
            return false;
        }
    }
    if (image_opt == IMAGE_ROM && call_opt == CALL_KIND_MODE) {
        LOGE("vbios_image=rom with vbios_call=mode: an image that never ran has no runtime state");
        return false;
    }
    if (!vbios && vbios_guest_trace_enabled()) {
        LOGE("vbios_trace_guest=1 requires display=vbios");
        return false;
    }
    if (!vbios)
        return true;
    LOGI("display=vbios: POST the GPU's legacy VBIOS in a hardware real-mode container "
         "(bounds: POST %llu ms, mode set %llu ms)",
         (unsigned long long)post_ms, (unsigned long long)mode_ms);
    const char *why;
    if (!hal_x86rm_probe(&why)) {
        LOGE("display=vbios: no %s: %s; boot stopped", hal_x86rm_info()->name, why);
        return false;
    }
    if (!platform_pci_inventory(bi->cmdline))
        return false;
    struct session *s = &session;
    memset(s, 0, sizeof(*s));
    s->reset = reset;
    s->text_hold = hold;
    s->text_keep = keep;
    vbios_trace_start(&s->trace, trace);
    const struct pci_device *card = select_card(&bi->fb);
    if (!card) {
        LOGE("display=vbios: no card to POST; boot stopped");
        return false;
    }
    enum image_source source = image_opt >= 0       ? (enum image_source)image_opt
                               : card->bars[6].size ? IMAGE_ROM
                                                    : IMAGE_HOST;
    enum call_kind call = call_opt >= 0         ? (enum call_kind)call_opt
                          : source == IMAGE_ROM ? CALL_KIND_POST
                                                : CALL_KIND_MODE;
    if (source == IMAGE_HOST && call_opt < 0 && bi->legacy.low && bi->legacy.low_size >= 0x44 &&
        !le16(bi->legacy.low + 0x40) && !le16(bi->legacy.low + 0x42)) {
        struct vbios_image image;
        if (!vbios_host_rom(bi->legacy.option, bi->legacy.option_size, card, &image) &&
            image.checksum_ok)
            call = CALL_KIND_POST;
    }
    LOGI("display=vbios: %02x:%02x.%u %04x:%04x%s: vbios_image=%s vbios_call=%s", card->bdf >> 8,
         (card->bdf >> 3) & 31, card->bdf & 7, card->vendor, card->device,
         card->bars[6].size ? "" : " (no expansion ROM BAR)", source == IMAGE_ROM ? "rom" : "host",
         call == CALL_KIND_POST ? "post" : "mode");
    if (source == IMAGE_ROM && call == CALL_KIND_MODE) {
        LOGE(
            "display=vbios: vbios_call=mode needs the host image (an image that never ran has no runtime state); "
            "boot stopped");
        return false;
    }
    if (reset && call == CALL_KIND_MODE) {
        LOGE(
            "display=vbios: vbios_reset=sbr with vbios_call=mode would discard the state the mode set runs on; "
            "boot stopped");
        return false;
    }
    /* From the ROM read on, the card's registers move and then it
     * reinitialises: the VMM draws nothing on its framebuffer until the
     * text console takes over or the attempt has ended. */
    uint64_t from = log_get_ring()->write_pos;
    vmm_log_fb_enable(false);
    bool ok = post(s, bi, card, source, call, post_ms, mode_ms, selftest);
    if (ok)
        again = (struct restart_call){true, bi, card->bdf, source, call, post_ms, mode_ms};
    else {
        vmm_log_fb_enable(true);
        if (s->reset_failed)
            LOGE(
                "display=vbios: the GPU was reset and did not come back as saved; the machine must be "
                "restarted before the GPU is usable again; VMM setup stops here");
        else if (s->reset_done)
            LOGW(
                "display=vbios: the GPU was reset and its POST failed: nothing scans out the GOP framebuffer "
                "any more, so this log is not on that screen");
        LOGE("display=vbios: initialization failed; boot stopped");
    }
    /* Replay the outcome after display access resumes. */
    static char lines[40][LOG_MATCH_COLUMNS + 1];
    size_t total = 0, kept = log_ring_since(log_get_ring(), from, lines, 40, &total);
    LOGI("display=vbios: %zu line(s) logged during the attempt%s:", total,
         total > kept ? " (last 40)" : "");
    for (size_t i = 0; i < kept; ++i)
        LOGI("  | %s", lines[i]);
    return ok;
}

/* Selftest (vbios_selftest=guest-stub): the guest's
 * int 1Ah and int 13h code executed by the real CPU in the container, its
 * port I/O through the same restricted mediation, each result compared
 * with vbios_int1a, the specification the stub implements. */
#define VBIOS_TEST_INT1A_IP 0xe030u /* int 1Ah; vmmcall */
#define VBIOS_TEST_INT13_IP 0xe038u /* int 13h; vmmcall */
static bool guest_stub_selftest(struct session *s, uint64_t bound_ms)
{
    const struct pci_device *card = device_of(s->card.bdf);
    if (!card)
        return false;
    static uint8_t ivt[0x400];
    memcpy(ivt, s->low, sizeof(ivt));
    vbios_build_guest_stub(s->stub, s->card.last_bus);
    static const uint8_t call1a[] = {0xcd, 0x1a},
                         call13[] = {0xcd, 0x13}; /* int 1Ah / int 13h; hypercall */
    memcpy(s->stub + VBIOS_TEST_INT1A_IP, call1a, sizeof(call1a));
    memcpy(s->stub + VBIOS_TEST_INT1A_IP + 2, s->hypercall, 3);
    memcpy(s->stub + VBIOS_TEST_INT13_IP, call13, sizeof(call13));
    memcpy(s->stub + VBIOS_TEST_INT13_IP + 2, s->hypercall, 3);
    put16(s->low + 0x1a * 4, VBIOS_GUEST_INT1A);
    put16(s->low + 0x1a * 4 + 2, VBIOS_STUB_SEGMENT);
    put16(s->low + 0x13 * 4, VBIOS_GUEST_UNSUPPORTED);
    put16(s->low + 0x13 * 4 + 2, VBIOS_STUB_SEGMENT);
    const struct {
        uint32_t ax, bx, cx, dx, si, di;
        bool int13;
    } cases[] = {
        {0xb101, 0x5555, 0xff00, 0, 0, 0xffffffff, false},
        {0xb102, 0, card->device, card->vendor, 0, 0, false},
        {0xb102, 0x1234, card->device, card->vendor, 1, 0, false},
        {0xb102, 0, 0x1234, 0xffff, 0, 0, false},
        {0xb103, 0, card->class_code, 0, 0, 0, false},
        {0xb108, s->card.bdf, 0xffffff00, 0, 0, 0x02, false},
        {0xb109, s->card.bdf, 0xffff0000, 0, 0, 0x02, false},
        {0xb10a, s->card.bdf, 0, 0, 0, 0x00, false},
        {0xb10a, s->card.bdf, 0, 0, 0, 0x10, false},
        {0xb10a, 0xffff, 0, 0, 0, 0x00, false},
        {0xb109, s->card.bdf, 0, 0, 0, 0x01, false},
        {0xb10a, s->card.bdf, 0, 0, 0, 0x100, false},
        {0xb1ff, 0, 0, 0, 0, 0, false},
        {0x0001, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, false},
        {0x0201, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, true},
    };
    unsigned failed = 0;
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(cases); ++i) {
        struct x86rm_regs expect;
        call_registers(&expect, cases[i].int13 ? VBIOS_TEST_INT13_IP : VBIOS_TEST_INT1A_IP);
        expect.rax = cases[i].ax | UINT64_C(0xabcd0000);
        expect.rbx = cases[i].bx;
        expect.rcx = cases[i].cx;
        expect.rdx = cases[i].dx;
        expect.rsi = cases[i].si;
        expect.rdi = cases[i].di;
        s->regs = expect;
        unsigned result = cases[i].int13 ? 2 : vbios_int1a(&s->card, &expect);
        /* Start with the carry the call must change, or set when it must
         * be left alone: a stub that never touches CF fails either way. */
        s->regs.rflags |= result == 1 || result == 0 ? 1 : 0;
        expect.rflags =
            result == 0 ? s->regs.rflags : (s->regs.rflags & ~UINT64_C(1)) | (result == 2);
        uint64_t elapsed;
        enum outcome o = run_call(s, "guest stub selftest",
                                  (cases[i].int13 ? VBIOS_TEST_INT13_IP : VBIOS_TEST_INT1A_IP) + 2,
                                  bound_ms, &elapsed);
        const struct x86rm_regs *r = &s->regs;
        bool same =
            o == CALL_DONE && (uint32_t)r->rax == (uint32_t)expect.rax &&
            (uint32_t)r->rbx == (uint32_t)expect.rbx && (uint32_t)r->rcx == (uint32_t)expect.rcx &&
            (uint32_t)r->rdx == (uint32_t)expect.rdx && (uint32_t)r->rsi == (uint32_t)expect.rsi &&
            (uint32_t)r->rdi == (uint32_t)expect.rdi && (r->rflags & 1) == (expect.rflags & 1);
        failed += !same;
        vmm_log(
            same ? LOG_INFO : LOG_ERROR,
            "VBIOS guest stub %s AX=%04x: EAX=%08llx EBX=%08llx ECX=%08llx EDX=%08llx EDI=%08llx CF=%llu %s "
            "(expected EAX=%08llx EBX=%08llx ECX=%08llx EDX=%08llx EDI=%08llx CF=%llu)",
            cases[i].int13 ? "int 13h" : "int 1Ah", cases[i].ax,
            (unsigned long long)(r->rax & 0xffffffff), (unsigned long long)(r->rbx & 0xffffffff),
            (unsigned long long)(r->rcx & 0xffffffff), (unsigned long long)(r->rdx & 0xffffffff),
            (unsigned long long)(r->rdi & 0xffffffff), (unsigned long long)(r->rflags & 1),
            same ? "matches" : "DIFFERS", (unsigned long long)(expect.rax & 0xffffffff),
            (unsigned long long)(expect.rbx & 0xffffffff),
            (unsigned long long)(expect.rcx & 0xffffffff),
            (unsigned long long)(expect.rdx & 0xffffffff),
            (unsigned long long)(expect.rdi & 0xffffffff), (unsigned long long)(expect.rflags & 1));
        if (o != CALL_DONE)
            break;
    }
    memcpy(s->low, ivt, sizeof(ivt));
    vbios_build_stubs(s->stub, s->hypercall);
    if (failed)
        LOGE("VBIOS selftest guest-stub FAILED: %u of %u calls differ", failed,
             (unsigned)IA64VMM_ARRAY_SIZE(cases));
    else
        LOGI("VBIOS selftest guest-stub: %u calls on the real CPU match the PCI BIOS specification",
             (unsigned)IA64VMM_ARRAY_SIZE(cases));
    return !failed;
}

/* Intel SDM PUSHAD/PUSHF/CALL/IRET: compare the observer with a direct
 * INT, including high register halves, segment registers, CF/DF/IF and
 * the exact restored stack. The synthetic BIOS either returns unchanged,
 * or supplies VBE success and changes CF/DF in the interrupt frame. */
static void guest_trace_selftest(struct session *s, uint64_t bound_ms)
{
    if (!vbios_guest_trace_enabled()) {
        LOGE("guest-trace selftest needs vbios_trace_guest=1");
        return;
    }
    static const uint8_t handlers[][32] = {
        {0xcf},
        {0x55, 0x89, 0xe5, 0x81, 0x76, 0x06, 0x01, 0x04, 0x5d, 0xb8, 0x4f, 0x00, 0xbf, 0xef, 0xbe,
         0xcf},
        {0xca, 0x02, 0x00}, /* RETF 2 */
        /* One nested INT10; the inner call sees AX=1234 and returns. */
        {0x3d, 0x34, 0x12, 0x74, 0x07, 0x50, 0xb8, 0x34, 0x12, 0xcd, 0x10, 0x58, 0xcf},
        /* Read FLAGS from the handler's interrupt frame into AX. */
        {0x55, 0x89, 0xe5, 0x8b, 0x46, 0x06, 0x5d, 0xcf},
    };
    uint8_t ivt[4], saved[sizeof(handlers[0])];
    memcpy(ivt, s->low + 0x40, sizeof(ivt));
    memcpy(saved, s->low + 0x8000, sizeof(saved));
    uint8_t saved_info[44];
    uint8_t *info = s->low + 0x56780 + 0x5678;
    memcpy(saved_info, info, sizeof(saved_info));
    put16(info, 0x009b);
    put16(info + 16, 2560);
    put16(info + 18, 640);
    put16(info + 20, 480);
    info[25] = 32;
    put32(info + 40, 0xe0000000);
    vbios_build_guest_stub(s->stub, s->card.last_bus);
    s->stub[VBIOS_TEST_INT1A_IP] = 0xcd;
    s->stub[VBIOS_TEST_INT1A_IP + 1] = 0x10;
    memcpy(s->stub + VBIOS_TEST_INT1A_IP + 2, s->hypercall, 3);
    put16(s->stub + VBIOS_GUEST_TRACE_TARGET, 0x8000);
    put16(s->stub + VBIOS_GUEST_TRACE_TARGET + 2, 0);
    /* Port 80h normally passes natively; the test observes every packet. */
    s->layout.io_pass[0x80 / 8] &= (uint8_t)~1u;
    unsigned failed = 0;
    for (unsigned i = 0; i < 10; ++i) {
        memcpy(s->low + 0x8000, handlers[i / 2], sizeof(handlers[0]));
        if (i == 2)
            put16(s->low + 0x8000 + 13, 0x5678); /* VBE preserves the buffer pointer */
        struct x86rm_regs direct = {0};
        for (unsigned trace = 0; trace < 2; ++trace) {
            put16(s->low + 0x40, trace ? VBIOS_GUEST_TRACE : 0x8000);
            put16(s->low + 0x42, trace ? VBIOS_STUB_SEGMENT : 0);
            call_registers(&s->regs, VBIOS_TEST_INT1A_IP);
            s->regs.rax = i == 2 ? 0xabcd4f01 : 0xabcd4f02;
            s->regs.rbx = 0x11221234;
            s->regs.rcx = i == 2 ? 0x33440140 : 0x33445678;
            s->regs.rdx = 0x55669abc;
            s->regs.rsi = 0x77881234;
            s->regs.rdi = 0x99aa5678;
            s->regs.rbp = 0xbbcc1111;
            s->regs.rflags = i & 1 ? 0x603 : 0x202;
            set_segment(&s->regs.ds, 0x1234, 0x93);
            set_segment(&s->regs.es, 0x5678, 0x93);
            put32(s->low + VBIOS_STACK_TOP, 0x12345678);
            uint64_t elapsed;
            enum outcome o =
                run_call(s, "guest trace selftest", VBIOS_TEST_INT1A_IP + 2, bound_ms, &elapsed);
            if (!trace)
                direct = s->regs;
            bool same = o == CALL_DONE && !memcmp(&direct, &s->regs, sizeof(direct)) &&
                        s->regs.rsp == VBIOS_STACK_TOP &&
                        le16(s->low + VBIOS_STACK_TOP) == 0x5678 &&
                        le16(s->low + VBIOS_STACK_TOP + 2) == 0x1234;
            if (i / 2 == 0 || i / 2 == 2 || i / 2 == 3)
                same &= s->regs.rax == 0xabcd4f02 && s->regs.rbx == 0x11221234 &&
                        s->regs.rcx == 0x33445678 && s->regs.rdx == 0x55669abc &&
                        s->regs.rsi == 0x77881234 && s->regs.rdi == 0x99aa5678 &&
                        s->regs.rbp == 0xbbcc1111 && s->regs.ds.selector == 0x1234 &&
                        s->regs.es.selector == 0x5678;
            uint64_t flags = i & 1 ? 0x603 : 0x202;
            if (i / 2 == 2)
                flags &= ~UINT64_C(0x300); /* RETF does not restore IF/TF */
            if (i / 2 != 1)
                same &= s->regs.rflags == flags;
            if (i / 2 == 4)
                same &= s->regs.rax == (0xabcd0000 | flags);
            if (!same) {
                ++failed;
                LOGE("guest trace selftest case %u %s differs", i, trace ? "wrapped" : "direct");
            }
        }
    }
    memcpy(s->low + 0x40, ivt, sizeof(ivt));
    memcpy(s->low + 0x8000, saved, sizeof(saved));
    memcpy(info, saved_info, sizeof(saved_info));
    vbios_build_stubs(s->stub, s->hypercall);
    s->layout.io_pass[0x80 / 8] |= 1u;
    if (failed)
        LOGE("VBIOS selftest guest-trace FAILED: %u calls differ", failed);
    else
        LOGI(
            "VBIOS selftest guest-trace: 10 calls preserve registers, FLAGS and stack (IRET, RETF 2, nested INT10, frame read)");
}

/* The code type 0 image of the card's ROM-BAR snapshot. */
static const uint8_t *rom_image(const struct pci_device *card, struct vbios_image *image)
{
    uint64_t rom_size = 0;
    const uint8_t *rom = platform_pci_rom(card->bdf, &rom_size);
    if (!rom || !vbios_find_legacy(rom, rom_size, image)) {
        LOGE("display=vbios: %02x:%02x.%u %04x:%04x: %s", card->bdf >> 8, (card->bdf >> 3) & 31,
             card->bdf & 7, card->vendor, card->device,
             rom ? "no code type 0 image in the ROM BAR" : "no expansion ROM snapshot");
        if (rom) {
            char text[16 * 3 + 1];
            for (unsigned i = 0; i < 16 && i < rom_size; ++i)
                vmm_snprintf(text + i * 3, 4, " %02x", rom[i]);
            text[(rom_size < 16 ? rom_size : 16) * 3] = 0;
            LOGE("VBIOS ROM snapshot (%llu bytes) starts:%s", (unsigned long long)rom_size, text);
        }
        return NULL;
    }
    LOGI(
        "VBIOS card %02x:%02x.%u %04x:%04x class %06x ROM BAR %llx size %llx: code type 0 image at +%llx, "
        "length %u, init size %u, PCIR revision %u, IDs %04x:%04x%s, checksum %s",
        card->bdf >> 8, (card->bdf >> 3) & 31, card->bdf & 7, card->vendor, card->device,
        card->class_code, (unsigned long long)card->bars[6].base, (unsigned long long)rom_size,
        (unsigned long long)image->offset, image->length, image->init_size, image->revision,
        image->vendor, image->device,
        image->vendor == card->vendor && image->device == card->device ? ""
                                                                       : " (differ from the card)",
        image->checksum_ok ? "ok" : "bad");
    return rom;
}

/* The platform's C0000 image, snapshotted at VMM start. */
static const uint8_t *host_image(const struct boot_info *bi, const struct pci_device *card,
                                 enum call_kind call, struct vbios_image *image)
{
    const struct boot_legacy *l = &bi->legacy;
    const char *why =
        call == CALL_KIND_POST
            ? vbios_host_rom(l->option, l->option_size, card, image)
            : vbios_host_image(l->option, l->option_size, l->low, l->low_size, card, image);
    uint16_t ggc = 0;
    if (!why && platform_vga_intel_gen6(card) && !platform_vga_intel_gen6_decodes(card, &ggc))
        why = "the IGD does not decode legacy VGA (memory decode off or GGC.IVD set)";
    if (why) {
        LOGE("display=vbios: %02x:%02x.%u %04x:%04x: host C0000 image refused: %s", card->bdf >> 8,
             (card->bdf >> 3) & 31, card->bdf & 7, card->vendor, card->device, why);
        return NULL;
    }
    if (!le16(l->option + 0x18)) {
        LOGI("VBIOS card %02x:%02x.%u %04x:%04x: classic host C0000 ROM, init size %u, checksum ok",
             card->bdf >> 8, (card->bdf >> 3) & 31, card->bdf & 7, card->vendor, card->device,
             image->init_size);
        return l->option;
    }
    LOGI(
        "VBIOS card %02x:%02x.%u %04x:%04x class %06x: host C0000 image %04x:%04x class %06x%s, runtime size %u, "
        "PCIR revision %u, checksum %02x%s, int 10h -> %04x:%04x%s",
        card->bdf >> 8, (card->bdf >> 3) & 31, card->bdf & 7, card->vendor, card->device,
        card->class_code, image->vendor, image->device, image->class_code,
        image->device == card->device ? "" : " (device ID differs from the card)", image->init_size,
        image->revision, image->checksum,
        image->checksum ? " (a POSTed image need not sum to zero)" : "",
        l->low && l->low_size >= 0x44 ? le16(l->low + 0x42) : 0,
        l->low && l->low_size >= 0x44 ? le16(l->low + 0x40) : 0,
        call == CALL_KIND_MODE ? " in the image" : "");
    return l->option;
}

/* An Intel IGD's OpRegion, mapped identity where the BootInfo map
 * says it is the platform's (ACPI NVS or reserved). */
static void opregion(struct session *s, const struct boot_info *bi, const struct pci_device *card)
{
    uint32_t asls = (uint32_t)platform_pci_read(((uint64_t)card->bdf << 12) | 0xfc, 4);
    uint64_t base = 0, size = 0;
    const char *why = vbios_opregion_range(bi, asls, &base, &size);
    if (!why && !vbios_add_mapping(&s->layout, base, size, X86RM_MAP_WRITE))
        why = "the container map is full or the range overlaps another mapping";
    if (why) {
        LOGW("VBIOS OpRegion: ASLS %08x: %s; not mapped", asls, why);
        return;
    }
    /* The header (OpRegion: signature "IntelGraphicsMem" at +0, size in
     * KiB at +10h) through the VMM's view of that range. */
    char signature[17] = "(not readable)";
    uint32_t kib = 0;
    for (unsigned i = 0; i < bi->mem_range_count; ++i) {
        const struct boot_mem_range *r = &bi->mem[i];
        if (!r->hva || asls < r->base || asls - r->base > r->size ||
            r->size - (asls - r->base) < 0x14)
            continue;
        const uint8_t *p = r->hva + (asls - r->base);
        for (unsigned k = 0; k < 16; ++k)
            signature[k] = p[k] >= 0x20 && p[k] < 0x7f ? (char)p[k] : '.';
        signature[16] = 0;
        kib = (uint32_t)le16(p + 0x10) | (uint32_t)le16(p + 0x12) << 16;
    }
    LOGI(
        "VBIOS OpRegion: ASLS %08x, signature \"%s\", size field %u KiB; %llx-%llx mapped read/write, write-back",
        asls, signature, kib, (unsigned long long)base, (unsigned long long)(base + size - 1));
}

/* The live-state sequence on a VBIOS the platform POSTed: query,
 * mode 3, and the GOP's VBE mode put back through the VBIOS itself when
 * mode 3 fails. True when mode 3 was reached. */
/* int 10h AX=4F02h through the VBIOS itself: the GOP's VBE mode back. */
static bool vbe_restore(struct session *s, uint16_t bx, uint64_t mode_ms)
{
    uint64_t elapsed = 0;
    call_registers(&s->regs, VBIOS_CALL10_IP);
    s->regs.rax = 0x4f02;
    s->regs.rbx = bx;
    enum outcome o =
        run_call(s, "restore (int 10h AX=4F02h)", VBIOS_CALL10_DONE, mode_ms, &elapsed);
    if (o == CALL_BOUND)
        dump(s, "restore (int 10h AX=4F02h)", "the wall-clock bound expired");
    LOGW("VBIOS restore: int 10h AX=4F02h BX=%04x -> %s AX=%04llx", bx,
         o == CALL_DONE    ? "returned,"
         : o == CALL_BOUND ? "bounded,"
                           : "failed,",
         (unsigned long long)(s->regs.rax & 0xffff));
    return o == CALL_DONE && (s->regs.rax & 0xffff) == 0x004f;
}

/* restore_bx: the 4F02h BX that puts the GOP's VBE mode back (0: none). */
static bool live_mode_set(struct session *s, uint64_t mode_ms, uint64_t *mode_elapsed,
                          uint16_t *restore_bx)
{
    uint64_t elapsed = 0;
    call_registers(&s->regs, VBIOS_CALL10_IP);
    s->regs.rax = 0x0f00;
    enum outcome o =
        run_call(s, "live-state query (int 10h AH=0Fh)", VBIOS_CALL10_DONE, mode_ms, &elapsed);
    if (o == CALL_BOUND)
        dump(s, "live-state query (int 10h AH=0Fh)", "the wall-clock bound expired");
    if (o != CALL_DONE) {
        LOGE(
            "VBIOS live state: int 10h AH=0Fh did not return; nothing was changed, the GOP picture stays");
        return false;
    }
    unsigned mode = s->regs.rax & 0xff, columns = (s->regs.rax >> 8) & 0xff,
             page = (s->regs.rbx >> 8) & 0xff;
    call_registers(&s->regs, VBIOS_CALL10_IP);
    s->regs.rax = 0x4f03;
    o = run_call(s, "live-state query (int 10h AX=4F03h)", VBIOS_CALL10_DONE, mode_ms, &elapsed);
    if (o == CALL_BOUND)
        dump(s, "live-state query (int 10h AX=4F03h)", "the wall-clock bound expired");
    if (o != CALL_DONE) {
        LOGE(
            "VBIOS live state: int 10h AX=4F03h did not return; nothing was changed, the GOP picture stays");
        return false;
    }
    uint16_t vbe_ax = (uint16_t)s->regs.rax, vbe_mode = (uint16_t)s->regs.rbx;
    bool vbe = vbe_ax == 0x004f;
    LOGI(
        "VBIOS live state: int 10h AH=0Fh -> mode %02x, %u columns, page %u; AX=4F03h -> AX=%04x BX=%04x%s",
        mode, columns, page, vbe_ax, vbe_mode, vbe ? "" : " (no VBE mode to restore)");
    /* Bit 15: do not clear the display memory; bit 14 (linear frame
     * buffer) as the query returned it. */
    *restore_bx = vbe ? (uint16_t)(vbe_mode | 0x8000u) : 0;
    call_registers(&s->regs, VBIOS_MODE_IP);
    o = run_call(s, "mode set", VBIOS_MODE_DONE, mode_ms, mode_elapsed);
    if (o == CALL_BOUND)
        dump(s, "mode set", "the wall-clock bound expired");
    bool ok = false;
    if (o == CALL_DONE) {
        uint8_t now = s->low[0x449];
        LOGI("VBIOS mode set returned in %llu ms: BDA video mode %02x, %u columns, %u rows",
             (unsigned long long)*mode_elapsed, now, le16(s->low + 0x44a), s->low[0x484] + 1u);
        ok = now == 3;
        if (!ok)
            LOGE("VBIOS: int 10h AX=0003h left video mode %02x", now);
    }
    if (ok)
        return true;
    if (!vbe) {
        LOGW("VBIOS restore: no VBE mode was saved; nothing attempted");
        return false;
    }
    (void)vbe_restore(s, *restore_bx, mode_ms);
    return false;
}

/* vbios_text_hold=N: a diagnostic for a mode set that returns
 * but leaves the panel dark. The IGD's display registers as the VBIOS left
 * them (BAR0; Sandy Bridge PRM Vol.3 and i915 offsets: VGACNTRL, pipe
 * config and source, timings, panel fitter, planes, PCH transcoders, the
 * LVDS/CRT/HDMI/DP ports, panel power, backlight, DPLLs, FDI), a test page
 * in the text buffer for N seconds, then the GOP's VBE mode back through
 * the VBIOS so the log can be read on the GOP framebuffer. */
static void igd_registers(const struct pci_device *card)
{
    static const struct {
        const char *name;
        uint32_t off;
    } regs[] = {
        {"VGACNTRL", 0x41000},   {"PIPEACONF", 0x70008},  {"PIPEBCONF", 0x71008},
        {"PIPEASRC", 0x6001c},   {"PIPEBSRC", 0x6101c},   {"HTOTAL_A", 0x60000},
        {"VTOTAL_A", 0x6000c},   {"PF_A_CTL", 0x68080},   {"PF_A_WSZ", 0x68074},
        {"PF_B_CTL", 0x68880},   {"DSPACNTR", 0x70180},   {"DSPBCNTR", 0x71180},
        {"TRANSACONF", 0xf0008}, {"TRANSBCONF", 0xf1008}, {"PCH_LVDS", 0xe1180},
        {"PCH_ADPA", 0xe1100},   {"PCH_HDMIB", 0xe1140},  {"PCH_DP_B", 0xe4100},
        {"PP_STATUS", 0xc7200},  {"PP_CONTROL", 0xc7204}, {"BLC_CPU2", 0x48250},
        {"BLC_PCH1", 0xc8250},   {"DPLL_SEL", 0xc7000},   {"DPLL_A", 0xc6014},
        {"FDI_TX_A", 0x60100},   {"FDI_RX_A", 0xf000c},
    };
    if (!vbios_intel_igd(card) || card->bars[0].io || card->bars[0].size < 0x100000)
        return;
    const volatile uint8_t *mmio = platform_pci_map_physical(card->bars[0].base, 0x100000);
    if (!mmio) {
        LOGW("VBIOS IGD: BAR0 %llx not mapped", (unsigned long long)card->bars[0].base);
        return;
    }
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(regs); i += 3) {
        char line[96];
        size_t used = (size_t)vmm_snprintf(line, sizeof(line), "VBIOS IGD:");
        for (unsigned j = i; j < i + 3 && j < IA64VMM_ARRAY_SIZE(regs); ++j)
            used += (size_t)vmm_snprintf(line + used, sizeof(line) - used, " %s=%08x", regs[j].name,
                                         *(const volatile uint32_t *)(mmio + regs[j].off));
        LOGI("%s", line);
    }
}
static void text_hold(struct session *s, const struct pci_device *card, uint64_t mode_ms,
                      uint16_t restore_bx)
{
    LOGI("VBIOS text hold: mode 3 reached; IGD registers, then a test page for %u s", s->text_hold);
    igd_registers(card);
    volatile uint16_t *cell = (volatile uint16_t *)platform_pci_map_physical(0xb8000, 0x8000);
    if (cell) {
        char title[81];
        vmm_snprintf(title, sizeof(title),
                     "IA-64 VMM: VGA text after the VBIOS mode set (vbios_text_hold=%u s)",
                     s->text_hold);
        for (unsigned i = 0; i < 80 * 25; ++i)
            cell[i] = 0x0720;
        for (unsigned i = 0; title[i]; ++i)
            cell[i] = (uint16_t)(0x1f00 | (uint8_t)title[i]);
        for (unsigned g = 0; g < 256; ++g) /* every glyph, 32 per row, rows 2-9 */
            cell[(2 + g / 32) * 80 + (g % 32) * 2] = (uint16_t)(0x0700 | g);
        for (unsigned c = 0; c < 16; ++c) { /* the 16 background colours, row 11 */
            cell[11 * 80 + c * 4] = (uint16_t)((c << 12) | 0x0f00 | "0123456789abcdef"[c]);
            cell[11 * 80 + c * 4 + 1] = (uint16_t)((c << 12) | 0x0f00 | ' ');
        }
        static const char tail[] = "Then int 10h AX=4F02h puts the GOP's VBE mode back.";
        for (unsigned i = 0; tail[i]; ++i)
            cell[13 * 80 + i] = (uint16_t)(0x0e00 | (uint8_t)tail[i]);
    } else
        LOGW("VBIOS text hold: b8000 not mapped; no test page");
    uint64_t until = hal_counter() + (uint64_t)s->text_hold * hal_counter_hz();
    while (hal_counter() < until)
        hal_idle_until(until);
    if (s->text_keep) {
        LOGI(
            "VBIOS text hold: over; mode 3 stays (,keep) and the VMM console moves to VGA text next");
        return;
    }
    if (!restore_bx)
        LOGW("VBIOS text hold: no VBE mode was saved; the display stays in mode 3");
    bool back = restore_bx && vbe_restore(s, restore_bx, mode_ms);
    LOGI("display=vbios: vbios_text_hold diagnostic over%s; boot stopped",
         back ? ", the GOP's VBE mode is back" : "");
}

/* True when the VGA text console has taken over. */
static bool post(struct session *s, const struct boot_info *bi, const struct pci_device *card,
                 enum image_source source, enum call_kind call, uint64_t post_ms, uint64_t mode_ms,
                 enum selftest selftest)
{
    bool cli_loop = selftest == SELFTEST_CLI_LOOP, held = false;
    const char *why;
    /* vbios_reset=sbr: the GPU as a machine without UEFI drivers
     * shows it to its legacy VBIOS, fresh out of reset. Before the ROM is
     * read; the GOP display is gone from here on. */
    enum pci_reset_result reset =
        s->reset ? platform_pci_reset_function(card->bdf) : PCI_RESET_DONE;
    if (reset != PCI_RESET_DONE) {
        LOGE("display=vbios: vbios_reset=sbr did not reset %02x:%02x.%u; no POST", card->bdf >> 8,
             (card->bdf >> 3) & 31, card->bdf & 7);
        /* A reset asserted without the GPU back as saved: the hardware no
         * longer matches the PCI inventory the guest would be built on. */
        s->reset_failed = reset == PCI_RESET_FAILED;
        return false;
    }
    s->reset_done = s->reset;
    struct vbios_image image;
    const uint8_t *bytes =
        source == IMAGE_ROM ? rom_image(card, &image) : host_image(bi, card, call, &image);
    if (!bytes)
        return false;
    if (image.init_size > VBIOS_SHADOW_MAX) {
        LOGE("display=vbios: init size %u exceeds C0000-DFFFFh", image.init_size);
        return false;
    }
    if (!vbios_card_from_inventory(card->bdf, &s->card) || !platform_pci_route_vga(card->bdf)) {
        LOGE("display=vbios: cannot route legacy VGA to the card");
        return false;
    }
    if (s->card.readonly_count) {
        char text[32];
        int at = 0;
        for (unsigned i = 0; i < s->card.readonly_count; ++i)
            at += vmm_snprintf(text + at, sizeof(text) - (size_t)at, " %02x:%02x.%u",
                               s->card.readonly[i] >> 8, (s->card.readonly[i] >> 3) & 31,
                               s->card.readonly[i] & 7);
        LOGI("VBIOS visibility: %02x:%02x.%u; read-only for the Intel IGD:%s; nothing else",
             card->bdf >> 8, (card->bdf >> 3) & 31, card->bdf & 7, text);
    }
    fabric_diagnostic();
    /* The coreboot int 15h 5Fxx answers are board configuration
     * for an Intel VBIOS that POSTs; a mode set on a POSTed one gets none. */
    s->post_hooks = call == CALL_KIND_POST && card->vendor == 0x8086;
    s->shadow_size = (uint32_t)IA64VMM_ALIGN_UP(image.init_size, 4096u);
    s->low = hal_alloc_pages(VBIOS_LOW_SIZE / 4096, HAL_PAGE_ZERO);
    s->shadow = hal_alloc_pages(s->shadow_size / 4096, 0);
    s->stub = hal_alloc_pages(VBIOS_STUB_SIZE / 4096, HAL_PAGE_ZERO);
    if (!s->low || !s->shadow || !s->stub ||
        !vbios_build_layout(card, s->low, s->shadow, s->shadow_size, s->stub, &s->layout)) {
        LOGE("display=vbios: cannot build the environment");
        release_pages(s);
        return false;
    }
    unsigned card_maps = s->layout.map_count - 4;
    if (vbios_intel_igd(card))
        opregion(s, bi, card);
    for (unsigned b = 0; b < 6; ++b)
        if (card->bars[b].size && !card->bars[b].io && card->bars[b].size < 4096)
            LOGW("VBIOS: BAR%u is %llu bytes; its whole page is mapped", b,
                 (unsigned long long)card->bars[b].size);
    if (s->trace.limit) {
        s->trace_ranges = 0;
        for (unsigned b = 0; b < 6; ++b) {
            const struct pci_bar *bar = &card->bars[b];
            if (!bar->size || !bar->io || bar->rom || bar->base + bar->size > 65536)
                continue;
            s->trace_base[s->trace_ranges] = (uint16_t)bar->base;
            s->trace_size[s->trace_ranges++] = (uint16_t)bar->size;
        }
        vbios_trace_ports(card, &s->layout);
        if (s->trace_ranges)
            LOGI(
                "VBIOS trace: %u I/O BAR(s) of the card intercepted and forwarded (first at %04x, %u ports); "
                "the first and last %u accesses kept",
                s->trace_ranges, s->trace_base[0], s->trace_size[0], s->trace.limit);
        else
            LOGI("VBIOS trace: the card has no I/O BAR; nothing to trace");
    }
    /* The shadow: the init (or runtime) size from the snapshot, FFh to the
     * page end; the ROM writes into it. */
    memcpy(s->shadow, bytes + image.offset, image.init_size);
    memset(s->shadow + image.init_size, 0xff, s->shadow_size - image.init_size);
    s->hz = hal_counter_hz();
    s->tick_start = vbios_ticks_of_day(platform_rtc_seconds());
    s->start = hal_counter();
    if (source == IMAGE_HOST && call == CALL_KIND_MODE) {
        /* Conventional memory as the platform left it;
         * IVT entries outside the image go to the container's entries.
         * The tick at 0040:006C is the container's from the first exit on. */
        memcpy(s->low, bi->legacy.low, VBIOS_LOW_SIZE);
        unsigned changed = vbios_host_ivt(s->low, image.init_size);
        vbios_store_ticks(s->low, s->tick_start, 0, &s->days);
        LOGI(
            "VBIOS environment from the host: 0-9ffff as the platform left it (EBDA segment %04x), %u of 256 IVT "
            "entries outside the image pointed at the container's entries",
            le16(s->low + 0x40e), changed);
    } else
        vbios_build_low(s->low, s->tick_start);
    memcpy(s->hypercall, hal_x86rm_info()->hypercall, sizeof(s->hypercall));
    vbios_build_stubs(s->stub, s->hypercall);
    LOGI(
        "VBIOS environment: RAM 0-9ffff, VGA a0000-bffff (UC), shadow c0000-%x, stubs f0000-fffff (read only), "
        "%u card BAR mapping(s)%s; ports 3b0-3df, 80 and the card's I/O BARs pass",
        VBIOS_SHADOW_BASE + s->shadow_size - 1, card_maps,
        s->layout.map_count - 4 > card_maps ? ", the OpRegion" : "");
    if (selftest == SELFTEST_GUEST_TRACE)
        s->layout.io_pass[0x80 / 8] &= (uint8_t)~1u;
    const struct x86rm_config config = {s->layout.map, s->layout.map_count, s->layout.io_pass};
    if (!hal_x86rm_create(&config, &s->rm, &why)) {
        LOGE("display=vbios: container: %s", why);
        release_pages(s);
        return false;
    }
    uint64_t post_elapsed = 0, mode_elapsed = 0;
    bool ok = false;
    if (cli_loop) {
        call_registers(&s->regs, VBIOS_CLI_IP);
        enum outcome o = run_call(s, "cli-loop selftest", 0, post_ms, &post_elapsed);
        if (o == CALL_BOUND && s->exits[X86RM_EXIT_INTR])
            LOGI(
                "VBIOS selftest cli-loop: aborted by the %llu ms bound after %llu exits (%llu INTR): "
                "host interrupts exit the guest after CLI",
                (unsigned long long)post_ms,
                (unsigned long long)s->exits[X86RM_EXIT_INTR] + s->exits[X86RM_EXIT_IO],
                (unsigned long long)s->exits[X86RM_EXIT_INTR]);
        else
            LOGE("VBIOS selftest cli-loop failed (outcome %u, %llu INTR exits)", (unsigned)o,
                 (unsigned long long)s->exits[X86RM_EXIT_INTR]);
    } else if (call == CALL_KIND_MODE) {
        uint16_t restore_bx = 0;
        ok = live_mode_set(s, mode_ms, &mode_elapsed, &restore_bx);
        state.runtime_size = s->shadow[2] * 512u;
        if (ok && selftest == SELFTEST_GUEST_STUB)
            (void)guest_stub_selftest(s, mode_ms);
        if (ok && s->text_hold) {
            text_hold(s, card, mode_ms, restore_bx);
            if (!s->text_keep) {
                ok = false;
                held = true;
            }
        }
    } else {
        /* vbios_text_hold needs the GOP's VBE mode, which only the live
         * state has. */
        if (s->text_hold)
            LOGW("VBIOS: vbios_text_hold applies to vbios_call=mode only; ignored");
        /* AX = bus/device/function; BX the PCI 3.0 runtime segment
         * (FFFFh before revision 3); DX FFFFh; ES:DI no $PnP structure. */
        call_registers(&s->regs, VBIOS_POST_IP);
        s->regs.rax = card->bdf;
        s->regs.rbx = image.revision >= 3 ? VBIOS_SHADOW_BASE >> 4 : 0xffff;
        s->regs.rdx = 0xffff;
        set_segment(&s->regs.es, VBIOS_STUB_SEGMENT, 0x93);
        /* SeaBIOS __callrom uses farcall16big; INT10 uses farcall16.
         * SeaVGABIOS POST reads EDID through a 32-bit BAR address. */
        struct x86rm_segment *segments[] = {&s->regs.cs, &s->regs.ds, &s->regs.es,
                                            &s->regs.ss, &s->regs.fs, &s->regs.gs};
        for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(segments); ++i) {
            segments[i]->limit = UINT32_MAX;
            segments[i]->attrib |= 0x800;
        }
        LOGI(
            "VBIOS POST: call far C000:0003 AX=%04llx BX=%04llx DX=FFFF ES:DI=F000:0000 SS:SP=0000:%04x",
            (unsigned long long)s->regs.rax, (unsigned long long)s->regs.rbx, VBIOS_STACK_TOP);
        enum outcome o = run_call(s, "POST", VBIOS_POST_DONE, post_ms, &post_elapsed);
        if (o == CALL_BOUND)
            dump(s, "POST", "the wall-clock bound expired");
        if (o == CALL_DONE) {
            uint16_t vector_ip = le16(s->low + 0x40), vector_cs = le16(s->low + 0x42);
            uint32_t vector = (uint32_t)vector_cs * 16 + vector_ip;
            state.runtime_size = s->shadow[2] * 512u;
            LOGI("VBIOS POST returned in %llu ms: C0000 signature %04x, size byte %u (%u bytes), "
                 "int 10h -> %04x:%04x",
                 (unsigned long long)post_elapsed, le16(s->shadow), s->shadow[2],
                 state.runtime_size, vector_cs, vector_ip);
            if (vector < 0xc0000 || vector > 0xcffff)
                LOGW("VBIOS: int 10h vector outside C0000-CFFFFh");
            call_registers(&s->regs, VBIOS_MODE_IP);
            o = run_call(s, "mode set", VBIOS_MODE_DONE, mode_ms, &mode_elapsed);
            if (o == CALL_BOUND)
                dump(s, "mode set", "the wall-clock bound expired");
            if (o == CALL_DONE) {
                uint8_t mode = s->low[0x449];
                LOGI("VBIOS mode set returned in %llu ms: BDA video mode %02x, %u columns, %u rows",
                     (unsigned long long)mode_elapsed, mode, le16(s->low + 0x44a),
                     s->low[0x484] + 1u);
                ok = mode == 3;
                if (!ok)
                    LOGE("VBIOS: int 10h AX=0003h left video mode %02x", mode);
                if (ok && selftest == SELFTEST_GUEST_STUB)
                    (void)guest_stub_selftest(s, mode_ms);
            }
        }
    }
    if (ok && selftest == SELFTEST_GUEST_TRACE)
        guest_trace_selftest(s, mode_ms);
    hal_x86rm_destroy(s->rm);
    s->rm = NULL;
    summary(s);
    trace_dump(s, false);
    if (!ok) {
        if (!cli_loop && !held)
            LOGE("display=vbios: no text mode");
        release_pages(s);
        return false;
    }
    if (!hal_console_vga_text()) {
        LOGE("display=vbios: cannot activate the VGA text console");
        release_pages(s);
        return false;
    }
    state.posted = true;
    state.bdf = card->bdf;
    state.last_bus = s->card.last_bus;
    state.low = s->low;
    state.shadow = s->shadow;
    state.shadow_size = s->shadow_size;
    state.stub = s->stub;
    if (call == CALL_KIND_MODE)
        LOGI(
            "VMM console: VGA text 80x25 at b8000 (no POST: the platform's VBIOS state, mode set %llu ms)",
            (unsigned long long)mode_elapsed);
    else
        LOGI("VMM console: VGA text 80x25 at b8000 (POST %llu ms, mode set %llu ms)",
             (unsigned long long)post_elapsed, (unsigned long long)mode_elapsed);
    return true;
}

bool platform_vbios_restart(void)
{
    if (!again.valid)
        return true;
    struct session *s = &session;
    const struct pci_device *card = NULL;
    for (unsigned i = 0; i < platform_pci_count(); ++i)
        if (platform_pci_device(i)->bdf == again.bdf)
            card = platform_pci_device(i);
    if (!card) {
        LOGE("display=vbios restart: card unavailable; boot stopped");
        return false;
    }
    if (again.call == CALL_KIND_POST) {
        LOGI(
            "display=vbios restart: POST of the %s image again, then the mode set, as at VMM start (a POST after a "
            "guest used the card is not qualified on every GPU)",
            again.source == IMAGE_ROM ? "ROM" : "host");
        bool reset = s->reset;
        unsigned limit = s->trace.limit;
        release_pages(s);
        memset(s, 0, sizeof(*s));
        memset(&state, 0, sizeof(state));
        s->reset = reset;
        vbios_trace_start(&s->trace, limit);
        bool ok = post(s, again.bi, card, again.source, again.call, again.post_ms, again.mode_ms,
                       SELFTEST_NONE);
        if (s->reset_failed)
            LOGE(
                "display=vbios restart: the GPU was reset and did not come back as saved; the run ends");
        else
            LOGI("display=vbios restart: %s",
                 ok ? "POST and mode 3 done" : "POST or mode set FAILED; boot stopped");
        return ok;
    }
    LOGI(
        "display=vbios restart: int 10h mode 3 on the state the guest left (no POST), as at VMM start");
    const struct x86rm_config config = {s->layout.map, s->layout.map_count, s->layout.io_pass};
    const char *why;
    if (!platform_pci_route_vga(card->bdf) || !hal_x86rm_reclaim_create(&config, &s->rm, &why)) {
        LOGE("display=vbios restart: no VGA route or container; boot stopped");
        return false;
    }
    /* IBM BDA video state as the guest's int 10h calls left it. */
    (void)gm_read(0x449, s->low + 0x449, 0x466 - 0x449 + 1);
    (void)gm_read(0x484, s->low + 0x484, 0x48a - 0x484 + 1);
    s->start = hal_counter();
    s->days = 0;
    uint64_t elapsed = 0;
    uint16_t restore_bx = 0;
    bool ok = live_mode_set(s, again.mode_ms, &elapsed, &restore_bx);
    hal_x86rm_destroy(s->rm);
    s->rm = NULL;
    LOGI("display=vbios restart: mode 3 %s", ok ? "set" : "FAILED; boot stopped");
    return ok;
}

uint32_t vbios_trace_install(uint8_t *low, uint8_t *image, uint32_t runtime, uint32_t capacity,
                             uint8_t *stub)
{
    uint32_t entry = (uint32_t)le16(low + 0x42) * 16 + le16(low + 0x40);
    if (!runtime || runtime % 512 || runtime > 0xfe00 || capacity < runtime + 512 ||
        entry < 0xc0000 || entry - 0xc0000 >= runtime)
        return 0;
    memcpy(stub + VBIOS_GUEST_TRACE_TARGET, low + 0x40, 4);
    memset(image + runtime, 0xff, 512);
    image[runtime] = 0xea; /* far JMP F000:int10_trace */
    put16(image + runtime + 1, VBIOS_GUEST_TRACE);
    put16(image + runtime + 3, 0xf000);
    put16(low + 0x40, (uint16_t)runtime);
    put16(low + 0x42, 0xc000);
    return runtime + 512;
}

/* --- guest BIOS handoff ---------------------------------- */

/* A read-only VMM-backed range: the post-POST image at C0000 (a CSM
 * write-protects the shadow after POST, PAM; SeaBIOS's make_bios_readonly)
 * and the stub segment at F0000. Guest writes are dropped and counted; the
 * first read and write are logged with the guest IP. */
struct legacy_region {
    const char *name;
    uint64_t base;
    const uint8_t *bytes;
    uint64_t size, reads, writes;
    bool read_logged, write_logged;
    uint64_t entry; /* offset of the code entry whose fetch is logged */
    const char *entry_name;
    bool entry_logged;
};
static struct legacy_region legacy_regions[2];
static bool guest_policy;
static uint8_t *guest_stub, *guest_trace_image;

static void legacy_note(struct legacy_region *r, struct vcpu *v, uint64_t off, unsigned size,
                        bool write, uint64_t value)
{
    bool *logged = write ? &r->write_logged : &r->read_logged;
    if (__atomic_exchange_n(logged, true, __ATOMIC_RELAXED))
        return;
    unsigned slot = 0;
    uint64_t ip = v ? vcpu_instruction_pointer(v, &slot) : 0;
    LOGI("legacy %s: first guest %s at %llx size %u%s%llx ip=%llx slot=%u", r->name,
         write ? "write (dropped)" : "read", (unsigned long long)(r->base + off), size,
         write ? " value=" : " ", (unsigned long long)value, (unsigned long long)ip, slot);
}
static uint64_t legacy_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    struct legacy_region *r = opaque;
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint64_t)(off + i < r->size ? r->bytes[off + i] : 0xff) << (i * 8);
    __atomic_fetch_add(&r->reads, 1, __ATOMIC_RELAXED);
    legacy_note(r, v, off, size, false, value);
    /* A read of the entry's first byte is a guest x86 emulator fetching
     * that code (the HAL's int 10h path), not the signature check. */
    if (r->entry_name && off <= r->entry && r->entry - off < size &&
        !__atomic_exchange_n(&r->entry_logged, true, __ATOMIC_RELAXED)) {
        unsigned slot = 0;
        uint64_t ip = v ? vcpu_instruction_pointer(v, &slot) : 0;
        LOGI("legacy %s: first guest read of the %s entry %llx ip=%llx slot=%u", r->name,
             r->entry_name, (unsigned long long)(r->base + r->entry), (unsigned long long)ip, slot);
    }
    return value;
}
static void legacy_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    struct legacy_region *r = opaque;
    __atomic_fetch_add(&r->writes, 1, __ATOMIC_RELAXED);
    legacy_note(r, v, off, size, true, value);
}

static bool give_guest(struct ia64vmm_handoff *h);
bool platform_vbios_handoff(struct ia64vmm_handoff *h)
{
    guest_policy = false;
    /* Until give_guest fills them for this boot, the regions an earlier
     * boot registered read as master abort: a failed re-POST at a restart
     * freed the pages they named. */
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(legacy_regions); ++i) {
        legacy_regions[i].bytes = NULL;
        legacy_regions[i].size = 0;
    }
    if (!state.posted)
        return true;
    return give_guest(h);
}
static bool give_guest(struct ia64vmm_handoff *h)
{
    if (!(h->flags & IA64VMM_HF_LEGACY_VGA) || h->vga_bdf != state.bdf) {
        LOGE("display=vbios: the POSTed card %02x:%02x.%u is not the assigned legacy VGA; "
             "the guest gets no legacy BIOS state",
             state.bdf >> 8, (state.bdf >> 3) & 31, state.bdf & 7);
        return true;
    }
    if (!guest_stub && !(guest_stub = hal_alloc_pages(VBIOS_STUB_SIZE / 4096, 0)))
        return false;
    /* A host image's conventional memory may name an EBDA; the
     * guest gets page 0 only, as the handoff contract specifies. */
    if (le16(state.low + 0x40e))
        LOGW(
            "display=vbios: BDA 040Eh names an EBDA at segment %04x; the guest does not get it (page 0 only)",
            le16(state.low + 0x40e));
    vbios_build_guest_stub(guest_stub, state.last_bus);
    static uint8_t low[IA64VMM_LEGACY_IVT_SIZE];
    memcpy(low, state.low, sizeof(low));
    unsigned odd = 0, changed = vbios_redirect_ivt(low, &odd);
    uint32_t trace_size = 0;
    if (vbios_guest_trace_enabled()) {
        if (state.runtime_size > 0xfe00 || state.runtime_size > state.shadow_size) {
            LOGE("VBIOS: no room for INT10 trace veneer below D0000");
            return false;
        }
        if (!guest_trace_image && !(guest_trace_image = hal_alloc_pages(16, 0)))
            return false;
        memcpy(guest_trace_image, state.shadow, state.runtime_size);
        trace_size =
            vbios_trace_install(low, guest_trace_image, state.runtime_size, 0x10000, guest_stub);
        if (!trace_size) {
            LOGE("VBIOS: cannot install INT10 trace veneer");
            return false;
        }
        for (unsigned i = 0; i < platform_pci_count(); ++i)
            if (platform_pci_device(i)->bdf == state.bdf)
                vbios_guest_trace_card(platform_pci_device(i));
        LOGI("VBIOS: INT10 trace veneer C000:%04x -> F000:%04x; vendor image unchanged",
             state.runtime_size, VBIOS_GUEST_TRACE);
    }
    uint32_t flags = IA64VMM_LEGACY_TEXT_CONSOLE | IA64VMM_LEGACY_STUB;
    /* Guest page 0 is identity RAM the firmware keeps reserved: the HAL
     * finds int 10h at 0000:0040 and the BDA at 0040:0000 there. */
    if (platform_ram_contains(IA64VMM_LEGACY_IVT_BASE, sizeof(low)) &&
        platform_ram_write(IA64VMM_LEGACY_IVT_BASE, low, sizeof(low)))
        flags |= IA64VMM_LEGACY_IVT;
    else
        LOGW(
            "display=vbios: guest page 0 is not RAM; no IVT/BDA for the guest (the HAL int 10h path stays off)");
    uint64_t rom = trace_size ? trace_size : state.runtime_size;
    if (!trace_size && rom > state.shadow_size) {
        LOGW("display=vbios: runtime size %llu beyond the %u-byte shadow; truncated",
             (unsigned long long)rom, state.shadow_size);
        rom = state.shadow_size;
    }
    if (rom)
        flags |= IA64VMM_LEGACY_ROM;
    /* The inherited GOP mode is gone after mode 3: no framebuffer for the
     * firmware, which then installs no GOP and uses VGA text. */
    h->fb_base = h->fb_size = 0;
    h->fb_width = h->fb_height = h->fb_pitch = 0;
    h->fb_format = IA64VMM_FB_NONE;
    h->fb_red_mask = h->fb_green_mask = h->fb_blue_mask = h->fb_reserved_mask = 0;
    h->legacy_version = IA64VMM_HANDOFF_LEGACY_VERSION;
    h->legacy_flags = flags;
    h->legacy_ivt_base = IA64VMM_LEGACY_IVT_BASE;
    h->legacy_ivt_size = flags & IA64VMM_LEGACY_IVT ? IA64VMM_LEGACY_IVT_SIZE : 0;
    h->legacy_rom_base = rom ? IA64VMM_LEGACY_ROM_BASE : 0;
    h->legacy_rom_size = rom;
    h->legacy_stub_base = IA64VMM_LEGACY_STUB_BASE;
    h->legacy_stub_size = IA64VMM_LEGACY_STUB_SIZE;
    h->legacy_bdf = state.bdf;
    /* The fetch of int 10h's entry in the image and of int 1Ah's in the
     * stub are logged once each: evidence that the guest runs that code. */
    uint32_t int10 = (uint32_t)le16(low + 0x42) * 16 + le16(low + 0x40);
    bool int10_in_rom = int10 >= IA64VMM_LEGACY_ROM_BASE && int10 - IA64VMM_LEGACY_ROM_BASE < rom;
    if (!int10_in_rom)
        LOGW("display=vbios: int 10h -> %05x lies outside the %llu-byte runtime image at c0000; "
             "the guest reads master abort there",
             int10, (unsigned long long)rom);
    legacy_regions[0] = (struct legacy_region){"rom",
                                               IA64VMM_LEGACY_ROM_BASE,
                                               trace_size ? guest_trace_image : state.shadow,
                                               rom,
                                               0,
                                               0,
                                               false,
                                               false,
                                               int10_in_rom ? int10 - IA64VMM_LEGACY_ROM_BASE : 0,
                                               int10_in_rom ? "int 10h" : NULL,
                                               false};
    legacy_regions[1] = (struct legacy_region){"stub",
                                               IA64VMM_LEGACY_STUB_BASE,
                                               guest_stub,
                                               IA64VMM_LEGACY_STUB_SIZE,
                                               0,
                                               0,
                                               false,
                                               false,
                                               VBIOS_GUEST_INT1A,
                                               "int 1Ah",
                                               false};
    guest_policy = true;
    LOGI(
        "display=vbios guest: VGA text console, no GOP; IVT/BDA %s (%u vectors redirected to the guest stub, "
        "%u without a counterpart), C0000 image %llu bytes read-only, F0000 stub segment read-only; "
        "int 10h -> %04x:%04x",
        flags & IA64VMM_LEGACY_IVT ? "in page 0" : "absent", changed, odd, (unsigned long long)rom,
        le16(low + 0x42), le16(low + 0x40));
    return true;
}

bool platform_vbios_map(struct platform_state *s)
{
    (void)s;
    if (!guest_policy)
        return true;
    static const struct gm_mmio_ops ops = {legacy_read, legacy_write};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(legacy_regions); ++i) {
        struct legacy_region *r = &legacy_regions[i];
        if (!r->size)
            continue;
        const struct gm_region region = {
            r->base, r->size, GM_EMULATED, i ? "legacy-bios-stub" : "legacy-option-rom", NULL,
            &ops,    r};
        if (!gm_add_region(&region))
            return false;
    }
    return true;
}

static void legacy_summaries(void);

/* Explicit diagnostic shutdown only, after every guest executor has
 * stopped. Ask the card's own BIOS for mode 3 so photographs show the
 * summaries even when the OS left graphics enabled. No POST or reset. */
static void trace_restore_text(void)
{
    struct session *s = &session;
    const struct x86rm_config config = {s->layout.map, s->layout.map_count, s->layout.io_pass};
    const char *why;
    /* Save live configuration, not the pre-guest inventory. Routing may
     * touch every bridge, including VGA Enable on competing paths. */
    uint16_t command[PCI_MAX_DEVICES], control[PCI_MAX_DEVICES];
    unsigned devices = platform_pci_count();
    for (unsigned i = 0; i < devices; ++i) {
        const struct pci_device *d = platform_pci_device(i);
        command[i] = (uint16_t)platform_pci_host_read(d->bdf, 4, 2);
        control[i] = d->header == 1 ? (uint16_t)platform_pci_host_read(d->bdf, 0x3e, 2) : 0;
    }
    vmm_log_fb_enable(false);
    if (!platform_pci_route_vga(s->card.bdf)) {
        LOGE("VBIOS exit mode 3: VGA routing unavailable");
        goto restore;
    }
    if (!hal_x86rm_reclaim_create(&config, &s->rm, &why)) {
        LOGE("VBIOS exit mode 3: container unavailable: %s", why);
        goto restore;
    }
    /* IBM BDA video state only. In particular, preserve the container's
     * 4A8h video save table pointer instead of importing a guest pointer. */
    (void)gm_read(0x449, s->low + 0x449, 0x466 - 0x449 + 1);
    (void)gm_read(0x484, s->low + 0x484, 0x48a - 0x484 + 1);
    s->start = hal_counter();
    s->days = 0;
    call_registers(&s->regs, VBIOS_MODE_IP);
    uint64_t elapsed;
    enum outcome o = run_call(s, "VBIOS exit mode 3", VBIOS_MODE_DONE, 10000, &elapsed);
    hal_x86rm_destroy(s->rm);
    s->rm = NULL;
    bool ok = o == CALL_DONE && s->low[0x449] == 3;
    if (ok)
        ok = hal_console_vga_text();
    if (ok)
        vmm_log_fb_enable(true);
    LOGI("VBIOS exit mode 3: %s (%llu ms)", ok ? "restored" : "FAILED; use RAM/media log",
         (unsigned long long)elapsed);
restore:
    /* The text console still needs decode while the capture pages are
     * drawn and held. Restore the borrowed routing after the last page. */
    legacy_summaries();
    for (unsigned i = 0; i < devices; ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (d->header == 1)
            platform_pci_host_write(d->bdf, 0x3e, control[i], 2);
        if (d->header == 1 || d->bdf == s->card.bdf)
            platform_pci_host_write(d->bdf, 4, command[i], 2);
    }
}

static void legacy_summaries(void)
{
    if (vbios_guest_trace_enabled())
        vbios_guest_trace_summary();
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(legacy_regions); ++i) {
        const struct legacy_region *r = &legacy_regions[i];
        LOGI("legacy %s %llx+%llx: %llu guest reads, %llu guest writes (dropped)", r->name,
             (unsigned long long)r->base, (unsigned long long)r->size,
             (unsigned long long)__atomic_load_n(&r->reads, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&r->writes, __ATOMIC_RELAXED));
    }
    if (vbios_guest_trace_enabled())
        hal_checkpoint("vbios-trace-legacy");
}

void platform_vbios_cleanup(void)
{
    if (!guest_policy)
        return;
    if (vbios_guest_trace_enabled())
        trace_restore_text();
    else
        legacy_summaries();
}
