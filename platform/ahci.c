/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AHCI 1.3.1 sections 3, 4 and 10; ATA disks and ATAPI optical drives. */
#include "platform/ahci.h"
#include "platform/lock.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"

#define AHCI_BOUNCE (64u * 1024)
#define AHCI_IE_MASK 0xfdc000ffu
#define AHCI_DHRS 1u
#define AHCI_PSS 2u
#define AHCI_HBFS (1u << 29)
#define AHCI_TFES (1u << 30)
#define AHCI_CAP (0x80000000u | 1u << 24 | 1u << 20 | 1u << 18 | 31u << 8)

struct ahci_port {
    struct platform_block block;
    uint64_t generation, clb, fb;
    uint32_t pis, pie, cmd, tfd, sctl, serr, ci, sact;
    uint8_t transfer_mode, sense_key, sense_asc, sense_ascq;
    bool soft_reset, write_cache, loaded, prevented;
};
static struct {
    struct ahci_port port[PLATFORM_AHCI_MAX_PORTS];
    unsigned count;
    void (*irq)(void *, bool), (*notify)(void *);
    void *irq_context, *notify_context;
    uint8_t *bounce;
    uint32_t base, ghc, is;
    uint16_t command;
    uint8_t irq_line;
    bool present, probe, level;
} hba;
static unsigned ahci_lock;

struct command {
    struct ahci_port *port;
    uint64_t generation, header, fb, lba;
    uint8_t fis[64], cdb[16], response[256];
    uint8_t *prd;
    size_t pages;
    uint32_t bytes, transferred;
    unsigned slot, prds, flags, sectors, op;
    uint8_t sense_key, sense_asc, sense_ascq;
    bool write, pio, identify, control, header_valid, packet, response_data;
};

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}
static void put16(uint8_t *p, unsigned v)
{
    p[0] = v;
    p[1] = v >> 8;
}
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = v >> (8 * i);
}
static unsigned be16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put_be16(uint8_t *p, unsigned v)
{
    p[0] = v >> 8;
    p[1] = v;
}
static void put_be32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = v >> (24 - 8 * i);
}
static bool optical(const struct ahci_port *s)
{
    return s->block.sector_bytes == 2048;
}
static unsigned sector_bytes(const struct ahci_port *s)
{
    return s->block.sector_bytes ? s->block.sector_bytes : 512;
}
static uint64_t mask(unsigned size)
{
    return size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}

/* Device DMA never reaches firmware, MMIO, or the VMM reservation. */
static bool ram_range(uint64_t address, uint64_t bytes)
{
    if (!bytes || bytes > UINT64_MAX - address)
        return false;
    while (bytes) {
        const struct gm_region *r = gm_lookup(address);
        if (!r || r->kind != GM_RAM)
            return false;
        uint64_t n = r->size - (address - r->base);
        if (n > bytes)
            n = bytes;
        bytes -= n;
        address += n;
    }
    return true;
}
static bool ram_copy(uint64_t address, void *buffer, uint64_t bytes, bool write)
{
    uint8_t *p = buffer;
    while (bytes) {
        const struct gm_region *r = gm_lookup(address);
        if (!r || r->kind != GM_RAM)
            return false;
        uint64_t n = r->size - (address - r->base);
        if (n > bytes)
            n = bytes;
        if (!(write ? gm_write(address, p, n) : gm_read(address, p, n)))
            return false;
        bytes -= n;
        address += n;
        p += n;
    }
    return true;
}
static void irq_update(void)
{
    for (unsigned i = 0; i < hba.count; ++i)
        if (hba.port[i].pis & hba.port[i].pie)
            hba.is |= 1u << i;
    bool high = (hba.ghc & 2) && hba.is && !(hba.command & 0x400);
    if (high != hba.level) {
        hba.level = high;
        if (hba.irq)
            hba.irq(hba.irq_context, high);
    }
}
static void port_reset(struct ahci_port *s)
{
    ++s->generation;
    s->pis = s->pie = s->cmd = s->sctl = s->serr = s->ci = s->sact = 0;
    s->clb = s->fb = 0;
    s->tfd = 0x50;
    s->soft_reset = false;
    s->write_cache = true;
    s->transfer_mode = 0x45;
    s->sense_key = s->sense_asc = s->sense_ascq = 0;
    s->loaded = true;
    s->prevented = false;
}
static void controller_reset(void)
{
    hba.ghc = 0x80000000u;
    hba.is = 0;
    for (unsigned i = 0; i < hba.count; ++i)
        port_reset(hba.port + i);
    irq_update();
}
static bool valid_block(const struct platform_block *block)
{
    return block->sectors && block->sectors <= (UINT64_C(1) << 48) &&
           (!block->sector_bytes || block->sector_bytes == 512 || block->sector_bytes == 2048 ||
            block->sector_bytes == 4096) &&
           (block->sector_bytes != 2048 ||
            (block->read_only && block->sectors <= (UINT64_C(1) << 32))) &&
           block->read && (block->read_only || (block->write && block->flush));
}
bool platform_ahci_init(const struct platform_block *blocks, unsigned count, uint64_t base,
                        unsigned irq_line, void (*irq)(void *, bool), void *irq_context,
                        void (*notify)(void *), void *notify_context)
{
    if (hba.present || !blocks || !count || count > PLATFORM_AHCI_MAX_PORTS || !base ||
        base > UINT32_MAX - PLATFORM_AHCI_SIZE + 1u || (base & (PLATFORM_AHCI_SIZE - 1)) ||
        irq_line > 255)
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (!valid_block(blocks + i))
            return false;
    hba.bounce = hal_alloc_pages(AHCI_BOUNCE / 4096, 0);
    if (!hba.bounce)
        return false;
    hba.count = count;
    for (unsigned i = 0; i < count; ++i)
        hba.port[i].block = blocks[i];
    hba.base = base;
    hba.irq_line = irq_line;
    hba.irq = irq;
    hba.irq_context = irq_context;
    hba.notify = notify;
    hba.notify_context = notify_context;
    hba.present = true;
    platform_ahci_reset();
    return true;
}
bool platform_ahci_present(void)
{
    return hba.present;
}
static bool commands_pending(void)
{
    for (unsigned i = 0; i < hba.count; ++i)
        if (hba.port[i].ci)
            return true;
    return false;
}
void platform_ahci_reset(void)
{
    platform_lock(&ahci_lock);
    hba.command = 0;
    hba.probe = false;
    controller_reset();
    platform_unlock(&ahci_lock);
}
void platform_ahci_cleanup(void)
{
    if (!hba.present)
        return;
    platform_ahci_reset();
    hal_free_pages(hba.bounce, AHCI_BOUNCE / 4096);
    memset(&hba, 0, sizeof(hba));
}

static bool config_access(unsigned off, unsigned size)
{
    return (size == 1 || size == 2 || size == 4) && !(off & (size - 1)) && off <= 4096 - size;
}
uint64_t platform_ahci_cfg_read(unsigned off, unsigned size)
{
    if (!config_access(off, size))
        return UINT64_MAX;
    platform_lock(&ahci_lock);
    uint32_t value = 0;
    if (!hba.present)
        value = UINT32_MAX;
    else
        switch (off & ~3u) {
        case 0:
            value = 0x29228086u;
            break;
        case 4:
            value = hba.command | (uint32_t)(hba.level ? 8 : 0) << 16;
            break;
        case 8:
            value = 0x01060101u;
            break;
        case 0x24:
            value = hba.probe ? 0xffffe000u : hba.base;
            break;
        case 0x2c:
            value = 0x29228086u;
            break;
        case 0x3c:
            value = hba.irq_line | 0x100u;
            break;
        default:
            break;
        }
    uint64_t result = (value >> ((off & 3) * 8)) & mask(size);
    platform_unlock(&ahci_lock);
    return result;
}
void platform_ahci_cfg_write(unsigned off, uint64_t value, unsigned size)
{
    if (!config_access(off, size))
        return;
    platform_lock(&ahci_lock);
    if (hba.present) {
        if ((off >= 4 && off + size <= 6) || (off == 4 && size == 4)) {
            unsigned shift = (off - 4) * 8;
            uint16_t next = (hba.command & ~(mask(size) << shift)) | (value << shift);
            if ((hba.command & 4) && !(next & 4)) {
                for (unsigned i = 0; i < hba.count; ++i) {
                    ++hba.port[i].generation;
                    hba.port[i].ci = hba.port[i].sact = 0;
                }
            }
            hba.command = next & 0x406;
            irq_update();
        } else if (off >= 0x24 && off + size <= 0x28)
            hba.probe = size == 4 && value == UINT32_MAX;
    }
    bool pending = commands_pending();
    void (*callback)(void *) = hba.notify;
    void *context = hba.notify_context;
    platform_unlock(&ahci_lock);
    if (pending && callback)
        callback(context);
}

static uint32_t read_register(unsigned off)
{
    if (off >= 0x100) {
        unsigned port = (off - 0x100) / 0x80;
        if (port >= hba.count)
            return 0;
        const struct ahci_port *s = hba.port + port;
        switch (off & 0x7f) {
        case 0:
            return s->clb;
        case 4:
            return s->clb >> 32;
        case 8:
            return s->fb;
        case 0xc:
            return s->fb >> 32;
        case 0x10:
            return s->pis;
        case 0x14:
            return s->pie;
        case 0x18:
            return s->cmd | ((s->cmd & 1) ? 1u << 15 : 0) | ((s->cmd & 16) ? 1u << 14 : 0);
        case 0x20:
            return s->tfd;
        case 0x24:
            return optical(s) ? 0xeb140101u : 0x101;
        case 0x28:
            return (s->sctl & 15) == 1 ? 1 : (s->sctl & 15) == 4 ? 0 : 0x113;
        case 0x2c:
            return s->sctl;
        case 0x30:
            return s->serr;
        case 0x34:
            return s->sact;
        case 0x38:
            return s->ci;
        default:
            return 0;
        }
    }
    switch (off) {
    case 0:
        return AHCI_CAP | (hba.count - 1);
    case 4:
        return hba.ghc;
    case 8:
        return hba.is;
    case 0xc:
        return hba.count == 32 ? UINT32_MAX : (1u << hba.count) - 1;
    case 0x10:
        return 0x10000;
    default:
        return 0;
    }
}
static uint64_t mmio_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    (void)opaque;
    (void)v;
    if ((size != 1 && size != 2 && size != 4 && size != 8) || (off & (size - 1)) ||
        off > PLATFORM_AHCI_SIZE - size)
        return UINT64_MAX;
    platform_lock(&ahci_lock);
    uint64_t value = UINT64_MAX;
    if (hba.present && (hba.command & 2)) {
        value = read_register(off & ~3u) >> ((off & 3) * 8);
        if (size == 8)
            value |= (uint64_t)read_register(off + 4) << 32;
    }
    platform_unlock(&ahci_lock);
    return value & mask(size);
}
static void write_register(unsigned off, uint32_t value, uint32_t enabled)
{
    uint32_t merged = (read_register(off) & ~enabled) | (value & enabled);
    if (off < 0x100)
        switch (off) {
        case 4:
            if (value & enabled & 1)
                controller_reset();
            else
                hba.ghc = 0x80000000u | (merged & 2);
            break;
        case 8:
            hba.is &= ~(value & enabled);
            break;
        default:
            break;
        }
    else {
        unsigned port = (off - 0x100) / 0x80;
        if (port >= hba.count)
            return;
        struct ahci_port *s = hba.port + port;
        switch (off & 0x7f) {
        case 0:
            if (!(s->cmd & 1))
                s->clb = (s->clb & ~UINT64_C(0xffffffff)) | (merged & ~1023u);
            break;
        case 4:
            if (!(s->cmd & 1))
                s->clb = (s->clb & UINT32_MAX) | (uint64_t)merged << 32;
            break;
        case 8:
            if (!(s->cmd & 16))
                s->fb = (s->fb & ~UINT64_C(0xffffffff)) | (merged & ~255u);
            break;
        case 0xc:
            if (!(s->cmd & 16))
                s->fb = (s->fb & UINT32_MAX) | (uint64_t)merged << 32;
            break;
        case 0x10:
            s->pis &= ~(value & enabled);
            break;
        case 0x14:
            s->pie = merged & AHCI_IE_MASK;
            break;
        case 0x18:
            if ((s->cmd & 1) && !(merged & 1)) {
                ++s->generation;
                s->ci = s->sact = 0;
            }
            s->cmd = merged & 0x03000017u;
            if (merged & 8)
                s->tfd = 0x50;
            break;
        case 0x2c: {
            unsigned old = s->sctl & 15;
            s->sctl = merged & 0xfff;
            if ((s->sctl & 15) == 1 || (old == 1 && !(s->sctl & 15))) {
                ++s->generation;
                s->ci = s->sact = 0;
                s->tfd = 0x50;
                s->soft_reset = false;
                s->serr |= 1u << 26;
                s->pis |= 1u << 6;
            }
            break;
        }
        case 0x30:
            s->serr &= ~(value & enabled);
            break;
        case 0x34:
            s->sact |= value & enabled;
            break;
        case 0x38:
            s->ci |= value & enabled;
            break;
        default:
            break;
        }
    }
    irq_update();
}
static void mmio_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    (void)opaque;
    (void)v;
    if ((size != 1 && size != 2 && size != 4 && size != 8) || (off & (size - 1)) ||
        off > PLATFORM_AHCI_SIZE - size)
        return;
    platform_lock(&ahci_lock);
    if (hba.present && (hba.command & 2)) {
        unsigned shift = (off & 3) * 8;
        write_register(off & ~3u, (uint32_t)value << shift,
                       (uint32_t)mask(size < 4 ? size : 4) << shift);
        if (size == 8)
            write_register(off + 4, value >> 32, UINT32_MAX);
    }
    bool notify = commands_pending();
    void (*callback)(void *) = hba.notify;
    void *context = hba.notify_context;
    platform_unlock(&ahci_lock);
    if (notify && callback)
        callback(context);
}
const struct gm_mmio_ops platform_ahci_ops = {mmio_read, mmio_write};

static bool packet_error(struct command *c, unsigned key, unsigned asc)
{
    c->sense_key = key;
    c->sense_asc = asc;
    return false;
}
static bool packet_response(struct command *c, unsigned length, unsigned allocation)
{
    c->response_data = true;
    c->bytes = length < allocation ? length : allocation;
    return true;
}
static void toc_address(uint8_t *p, uint32_t lba, bool msf)
{
    if (!msf) {
        put_be32(p, lba);
        return;
    }
    uint64_t frames = (uint64_t)lba + 150;
    p[0] = 0;
    p[1] = frames / UINT64_C(4500);
    p[2] = frames / 75 % 60;
    p[3] = frames % 75;
}
/* SPC/MMC responses describe a single finalized, read-only data session. */
static bool decode_packet(struct command *c)
{
    struct ahci_port *s = c->port;
    const uint8_t *d = c->cdb;
    uint8_t *p = c->response;
    unsigned allocation;
    c->packet = true;
    switch (d[0]) {
    case 0x03: /* REQUEST SENSE */
        if (d[1] & 1) {
            p[0] = 0x72;
            p[1] = s->sense_key;
            p[2] = s->sense_asc;
            p[3] = s->sense_ascq;
            return packet_response(c, 8, d[4]);
        }
        p[0] = 0x70;
        p[2] = s->sense_key;
        p[7] = 10;
        p[12] = s->sense_asc;
        p[13] = s->sense_ascq;
        return packet_response(c, 18, d[4]);
    case 0x12: /* INQUIRY */
        if (d[1] & 2)
            return packet_error(c, 5, 0x24);
        if (d[1] & 1) {
            p[0] = 5;
            p[1] = d[2];
            if (!d[2]) {
                p[3] = 2;
                p[4] = 0;
                p[5] = 0x80;
                return packet_response(c, 6, be16(d + 3));
            }
            if (d[2] == 0x80) {
                unsigned port = (unsigned)(s - hba.port);
                p[3] = 12;
                memcpy(p + 4, "IA64VMM-CD00", 12);
                p[14] = '0' + port / 10;
                p[15] = '0' + port % 10;
                return packet_response(c, 16, be16(d + 3));
            }
            return packet_error(c, 5, 0x24);
        }
        if (d[2])
            return packet_error(c, 5, 0x24);
        p[0] = 5;
        p[1] = 0x80;
        p[2] = 5;
        p[3] = 2;
        p[4] = 31;
        memcpy(p + 8, "IA64VMM ", 8);
        memcpy(p + 16, "Virtual DVD-ROM ", 16);
        memcpy(p + 32, "1.0 ", 4);
        return packet_response(c, 36, be16(d + 3));
    case 0x1a:
    case 0x5a: { /* MODE SENSE */
        unsigned page = d[2] & 63, pc = d[2] >> 6;
        if (d[3] || pc == 3 || (page != 0x2a && page != 0x3f && page != 1))
            return packet_error(c, 5, 0x24);
        unsigned head = d[0] == 0x5a ? 8 : 4, length = page == 1 ? 12 : page == 0x3f ? 42 : 30;
        unsigned cap = head + (page == 0x3f ? 12 : 0);
        if (page == 1 || page == 0x3f) {
            p[head] = 1;
            p[head + 1] = 10;
        }
        if (page != 1) {
            p[cap] = 0x2a;
            p[cap + 1] = 28;
        }
        if (page != 1 && pc != 1) {
            p[cap + 2] = 8; /* DVD-ROM read */
            p[cap + 6] = 0x29 | (s->prevented ? 2 : 0);
            put_be16(p + cap + 8, 3600);
            put_be16(p + cap + 12, 64);
            put_be16(p + cap + 14, 3600);
        }
        if (head == 8) {
            put_be16(p, head + length - 2);
            p[3] = 0x80;
            allocation = be16(d + 7);
        } else {
            p[0] = head + length - 1;
            p[2] = 0x80;
            allocation = d[4];
        }
        return packet_response(c, head + length, allocation);
    }
    case 0x46: /* GET CONFIGURATION: profile list */
        if ((d[1] & 3) == 3)
            return packet_error(c, 5, 0x24);
        put_be16(p + 6, s->loaded ? 0x10 : 0);
        allocation = be16(d + 7);
        if (be16(d + 2)) {
            put_be32(p, 4);
            return packet_response(c, 8, allocation);
        }
        put_be32(p, 16);
        p[10] = 3;
        p[11] = 8;
        put_be16(p + 12, 8);
        put_be16(p + 16, 0x10);
        p[18] = s->loaded ? 1 : 0;
        return packet_response(c, 20, allocation);
    case 0x4a: /* GET EVENT STATUS NOTIFICATION: polled media status */
        if (!(d[1] & 1))
            return packet_error(c, 5, 0x24);
        if (!(d[4] & 0x10)) {
            put_be16(p, 2);
            p[2] = 0x80;
            p[3] = 0x10;
            return packet_response(c, 4, be16(d + 7));
        }
        put_be16(p, 6);
        p[2] = 4;
        p[3] = 0x10;
        p[5] = s->loaded ? 2 : 1;
        return packet_response(c, 8, be16(d + 7));
    case 0x1e: /* PREVENT/ALLOW MEDIUM REMOVAL */
        if (d[4] & ~1u)
            return packet_error(c, 5, 0x24);
        return true;
    case 0x1b: /* START STOP UNIT */
        if (d[4] & 0xf0)
            return packet_error(c, 5, 0x24);
        if ((d[4] & 3) == 2 && s->prevented) {
            c->sense_ascq = 2;
            return packet_error(c, 5, 0x53);
        }
        return true;
    default:
        break;
    }
    if (!s->loaded)
        return packet_error(c, 2, 0x3a);
    switch (d[0]) {
    case 0:
    case 0x35:
    case 0xbb:
        return true; /* ready, synchronize, speed */
    case 0x25:       /* READ CAPACITY(10) */
        if (be32(d + 2) && !(d[8] & 1))
            return packet_error(c, 5, 0x24);
        put_be32(p, s->block.sectors - 1);
        put_be32(p + 4, 2048);
        return packet_response(c, 8, 8);
    case 0x28:
    case 0xa8: /* READ(10/12) */
        c->lba = be32(d + 2);
        c->sectors = d[0] == 0x28 ? be16(d + 7) : be32(d + 6);
        if (c->sectors && (c->lba >= s->block.sectors || c->sectors > s->block.sectors - c->lba))
            return packet_error(c, 5, 0x21);
        if (c->sectors > UINT32_MAX / 2048)
            return packet_error(c, 5, 0x24);
        c->bytes = c->sectors * 2048;
        return true;
    case 0x43: { /* READ TOC, formatted TOC or last-session information */
        unsigned format = d[2] & 15;
        if (format > 1 || (d[6] > 1 && d[6] != 0xaa))
            return packet_error(c, 5, 0x24);
        bool lead_only = d[6] == 0xaa && !format;
        unsigned length = format || lead_only ? 12 : 20;
        put_be16(p, length - 2);
        p[2] = p[3] = 1;
        p[5] = 0x14;
        p[6] = lead_only ? 0xaa : 1;
        toc_address(p + 8, lead_only ? s->block.sectors : 0, d[1] & 2);
        if (length == 20) {
            p[13] = 0x14;
            p[14] = 0xaa;
            toc_address(p + 16, s->block.sectors, d[1] & 2);
        }
        return packet_response(c, length, be16(d + 7));
    }
    case 0x51: /* READ DISC INFORMATION */
        if (d[1] & 7)
            return packet_error(c, 5, 0x24);
        put_be16(p, 32);
        p[2] = 0x0e;
        p[3] = p[4] = p[5] = p[6] = 1;
        return packet_response(c, 34, be16(d + 7));
    case 0x2a:
    case 0xaa:
    case 0x2e:
    case 0x04:
        return packet_error(c, 7, 0x27);
    default:
        return packet_error(c, 5, 0x20);
    }
}
static bool decode_command(struct command *c)
{
    struct ahci_port *s = c->port;
    c->op = c->fis[2];
    if (!(c->fis[1] & 0x80)) {
        c->control = true;
        return true;
    }
    if (c->op == 0xa0) {
        if (!optical(s) || !(c->flags & 0x20))
            return false;
        if (!decode_packet(c))
            return false;
        return !(c->flags & 0x40) || !c->bytes || packet_error(c, 5, 0x24);
    }
    if (c->flags & 0x20)
        return false;
    if (optical(s) && c->op != 0xa1 && c->op != 0xef && c->op != 0xe5 && c->op != 0x90 &&
        c->op != 8)
        return false;
    switch (c->op) {
    case 0xa1:
        if (!optical(s))
            return false;
        /* fall through */
    case 0xec:
        c->identify = c->pio = true;
        c->bytes = 512;
        break;
    case 0x20:
    case 0x24:
    case 0xc4:
    case 0x29:
        c->pio = true;
        break;
    case 0x30:
    case 0x34:
    case 0xc5:
    case 0x39:
        c->pio = c->write = true;
        break;
    case 0xc8:
    case 0xc9:
    case 0x25:
        break;
    case 0xca:
    case 0xcb:
    case 0x35:
        c->write = true;
        break;
    case 0xef:
        switch (c->fis[3]) {
        case 2:
        case 0x82:
        case 0x55:
        case 0xaa:
            return true;
        case 3:
            return (c->fis[12] >= 8 && c->fis[12] <= 12) ||
                   (c->fis[12] >= 0x20 && c->fis[12] <= 0x22) ||
                   (c->fis[12] >= 0x40 && c->fis[12] <= 0x45);
        default:
            return false;
        }
    case 8:
        return optical(s);
    case 0xe7:
    case 0xea:
    case 0xe0:
    case 0xe1:
    case 0xe2:
    case 0xe3:
    case 0xe5:
    case 0xe6:
    case 0x40:
    case 0x42:
    case 0x90:
    case 0x91:
    case 0xc6:
        return true;
    default:
        return false;
    }
    if (!c->identify) {
        bool ext = c->op == 0x24 || c->op == 0x25 || c->op == 0x29 || c->op == 0x34 ||
                   c->op == 0x35 || c->op == 0x39;
        if (!(c->fis[7] & 0x40))
            return false;
        c->lba = c->fis[4] | (uint64_t)c->fis[5] << 8 | (uint64_t)c->fis[6] << 16;
        c->sectors = c->fis[12];
        if (ext) {
            c->lba |=
                (uint64_t)c->fis[8] << 24 | (uint64_t)c->fis[9] << 32 | (uint64_t)c->fis[10] << 40;
            c->sectors |= (unsigned)c->fis[13] << 8;
            if (!c->sectors)
                c->sectors = 65536;
        } else {
            c->lba |= (uint64_t)(c->fis[7] & 15) << 24;
            if (!c->sectors)
                c->sectors = 256;
        }
        if (c->lba >= s->block.sectors || c->sectors > s->block.sectors - c->lba ||
            (c->write && s->block.read_only))
            return false;
        c->bytes = c->sectors * sector_bytes(s);
    }
    return c->write == !!(c->flags & 0x40);
}

/* Returns an AHCI error bit, or zero after capturing every DMA descriptor. */
static uint32_t snapshot(struct command *c)
{
    struct ahci_port *s = c->port;
    uint8_t header[32];
    c->header = s->clb + (uint64_t)c->slot * 32;
    c->fb = s->fb;
    c->generation = s->generation;
    if (!(hba.command & 4) || !ram_range(s->clb, 1024) || !ram_range(c->fb, 256) ||
        !ram_copy(c->header, header, 32, false))
        return AHCI_HBFS;
    c->header_valid = true;
    c->flags = get32(header);
    c->prds = c->flags >> 16;
    uint64_t table = get64(header + 8), bytes = 128 + (uint64_t)c->prds * 16;
    if ((table & 127) || !ram_range(table, bytes) || !ram_copy(table, c->fis, 64, false) ||
        !ram_copy(table + 64, c->cdb, 16, false))
        return AHCI_HBFS;
    if ((c->flags & 31) < 5 || (c->flags & 31) > 16 || (c->flags & 0xf000) || c->fis[0] != 0x27 ||
        (c->fis[1] & 15) || !decode_command(c))
        return AHCI_TFES;
    if (c->packet && c->bytes && (c->flags & 0x40))
        return AHCI_TFES;
    if (!c->prds)
        return c->bytes ? AHCI_HBFS : 0;
    c->pages = ((size_t)c->prds * 16 + 4095) / 4096;
    c->prd = hal_alloc_pages(c->pages, 0);
    if (!c->prd || !ram_copy(table + 128, c->prd, (uint64_t)c->prds * 16, false))
        return AHCI_HBFS;
    uint64_t total = 0;
    for (unsigned i = 0; i < c->prds; ++i) {
        const uint8_t *p = c->prd + (size_t)i * 16;
        uint64_t addr = get64(p);
        uint32_t dbc = get32(p + 12), length = (dbc & 0x3fffff) + 1;
        if ((addr & 1) || (length & 1) || get32(p + 8) || (dbc & 0x7fc00000) ||
            !ram_range(addr, length))
            return AHCI_HBFS;
        total += length;
    }
    return total < c->bytes ? AHCI_HBFS : 0;
}
static void identify(const struct ahci_port *s)
{
    uint8_t *p = hba.bounce;
    memset(p, 0, 512);
    put16(p, optical(s) ? 0x8580 : 0x0040);
    if (!optical(s)) {
        put16(p + 2, 16383);
        put16(p + 6, 16);
        put16(p + 12, 63);
    }
    const char serial[] = "IA64VMM-GUEST-DISK   ",
               model[] = "IA64VMM virtual SATA disk                ";
    for (unsigned i = 0; i < 20; ++i)
        p[20 + (i ^ 1)] = serial[i];
    unsigned port = (unsigned)(s - hba.port);
    p[20 + (18 ^ 1)] = '0' + port / 10;
    p[20 + (19 ^ 1)] = '0' + port % 10;
    for (unsigned i = 0; i < 40; ++i)
        p[54 + (i ^ 1)] = model[i];
    p[46] = '1';
    p[47] = '0';
    put16(p + 47 * 2, 0x8010);
    put16(p + 49 * 2, 0x0300);
    put16(p + 53 * 2, 6);
    unsigned mode = s->transfer_mode;
    put16(p + 63 * 2, 7 | (mode >= 0x20 && mode <= 0x22 ? 1u << (8 + mode - 0x20) : 0));
    put16(p + 64 * 2, 3);
    put16(p + 75 * 2, 0);
    put16(p + 76 * 2, 2);
    put16(p + 80 * 2, 0x01f0);
    put16(p + 82 * 2, 0x0020);
    put16(p + 83 * 2, 0x7400);
    put16(p + 84 * 2, 0x4000);
    put16(p + 85 * 2, s->write_cache ? 0x0020 : 0);
    put16(p + 86 * 2, 0x3400);
    put16(p + 87 * 2, 0x4000);
    put16(p + 88 * 2, 0x003f | (mode >= 0x40 && mode <= 0x45 ? 1u << (8 + mode - 0x40) : 0));
    put16(p + 106 * 2, 0x4000);
    if (optical(s)) {
        put16(p + 49 * 2, 0x0100);
        put16(p + 82 * 2, 0);
        put16(p + 83 * 2, 0x4000);
        put16(p + 85 * 2, 0);
        put16(p + 86 * 2, 0);
        return;
    }
    if (sector_bytes(s) == 4096) {
        put16(p + 106 * 2, 0x5000);
        put32(p + 117 * 2, 2048);
    }
    uint64_t sectors = s->block.sectors, lba28 = sectors > 0x0fffffff ? 0x0fffffff : sectors;
    put32(p + 60 * 2, lba28);
    for (unsigned i = 0; i < 4; ++i)
        put16(p + 200 + (size_t)i * 2, sectors >> (i * 16));
}
static bool current(const struct command *c)
{
    return c->port->generation == c->generation;
}
static bool scatter(struct command *c, uint32_t offset, uint32_t bytes, bool write)
{
    uint32_t skip = offset, done = 0;
    for (unsigned i = 0; i < c->prds && done < bytes; ++i) {
        const uint8_t *p = c->prd + (size_t)i * 16;
        uint32_t length = (get32(p + 12) & 0x3fffff) + 1;
        if (skip >= length) {
            skip -= length;
            continue;
        }
        unsigned n = length - skip;
        if (n > bytes - done)
            n = bytes - done;
        if (!ram_copy(get64(p) + skip, hba.bounce + done, n, write))
            return false;
        skip = 0;
        done += n;
    }
    return done == bytes;
}
static uint32_t transfer(struct command *c)
{
    struct ahci_port *s = c->port;
    if (!c->bytes) {
        if (c->op == 0xe7 || c->op == 0xea || (c->op == 0xef && c->fis[3] == 0x82))
            return s->block.read_only || s->block.flush(s->block.context) ? 0 : AHCI_TFES;
        return 0;
    }
    if (c->identify) {
        platform_lock(&ahci_lock);
        identify(s);
        platform_unlock(&ahci_lock);
    }
    if (c->response_data)
        memcpy(hba.bounce, c->response, c->bytes);
    while (c->transferred < c->bytes) {
        unsigned n = c->bytes - c->transferred;
        if (n > AHCI_BOUNCE)
            n = AHCI_BOUNCE;
        platform_lock(&ahci_lock);
        bool valid = current(c);
        bool copied = valid && (!c->write || scatter(c, c->transferred, n, false));
        platform_unlock(&ahci_lock);
        if (!valid)
            return 0;
        if (!copied)
            return AHCI_HBFS;
        if (!c->identify && !c->response_data) {
            unsigned sector = sector_bytes(s);
            bool ok = c->write ? s->block.write(s->block.context, c->lba + c->transferred / sector,
                                                n / sector, hba.bounce)
                               : s->block.read(s->block.context, c->lba + c->transferred / sector,
                                               n / sector, hba.bounce);
            if (!ok) {
                if (c->packet)
                    packet_error(c, 3, 0x11);
                return AHCI_TFES;
            }
            platform_lock(&ahci_lock);
            bool flush = c->write && !s->write_cache;
            platform_unlock(&ahci_lock);
            if (flush && !s->block.flush(s->block.context))
                return AHCI_TFES;
        }
        platform_lock(&ahci_lock);
        valid = current(c);
        copied = valid && (c->write || scatter(c, c->transferred, n, true));
        platform_unlock(&ahci_lock);
        if (!valid)
            return 0;
        if (!copied)
            return AHCI_HBFS;
        c->transferred += n;
    }
    return 0;
}
static void complete(struct command *c, uint32_t error)
{
    struct ahci_port *s = c->port;
    if (!current(c))
        return;
    uint8_t fis[20] = {0x34, 0x40, 0x50};
    memcpy(fis + 4, c->fis + 4, 10);
    if (c->op == 0xe5)
        fis[12] = 0xff;
    if (c->packet) {
        fis[12] = 3;
        fis[5] = fis[6] = 0;
        if (!error) {
            if (c->cdb[0] == 3)
                s->sense_key = s->sense_asc = s->sense_ascq = 0;
            if (c->cdb[0] == 0x1e)
                s->prevented = c->cdb[4] & 1;
            if (c->cdb[0] == 0x1b && (c->cdb[4] & 2))
                s->loaded = c->cdb[4] & 1;
        } else if (error == AHCI_TFES) {
            s->sense_key = c->sense_key ? c->sense_key : 5;
            s->sense_asc = c->sense_asc;
            s->sense_ascq = c->sense_ascq;
        }
    }
    if (c->control) {
        s->soft_reset = c->fis[15] & 4;
        s->tfd = s->soft_reset ? 0x80 : 0x50;
        fis[2] = s->tfd;
        fis[4] = fis[12] = 1;
    } else {
        s->tfd = 0x50;
        if (!error && c->op == 0xef) {
            if (c->fis[3] == 2 || c->fis[3] == 0x82)
                s->write_cache = c->fis[3] == 2;
            if (c->fis[3] == 3)
                s->transfer_mode = c->fis[12];
        }
    }
    if (error) {
        fis[2] = 0x51;
        fis[3] = 4 | (c->packet ? s->sense_key << 4 : 0);
        s->tfd = fis[2] | (unsigned)fis[3] << 8;
    }
    if ((hba.command & 4) && c->header_valid) {
        uint8_t count[4];
        put32(count, c->transferred);
        ram_copy(c->header + 4, count, 4, true);
    }
    if ((hba.command & 4) && (s->cmd & 16) && ram_range(c->fb, 256)) {
        if (c->pio && !error) {
            uint8_t setup[20] = {0x5f, 0x60, 0x58};
            setup[15] = 0x50;
            put16(setup + 16, c->bytes > 65535 ? 0 : c->bytes);
            ram_copy(c->fb + 0x20, setup, 20, true);
            s->pis |= AHCI_PSS;
        }
        ram_copy(c->fb + 0x40, fis, 20, true);
    }
    s->ci &= ~(1u << c->slot);
    s->sact &= ~(1u << c->slot);
    s->pis |= error ? error : AHCI_DHRS;
    irq_update();
}
void platform_ahci_poll(void)
{
    for (unsigned port = 0; port < hba.count; ++port) {
        struct ahci_port *s = hba.port + port;
        /* Consume only the slots queued when this port's pass started. */
        platform_lock(&ahci_lock);
        uint32_t pending = hba.present && (s->cmd & 1) && (s->cmd & 16) ? s->ci : 0;
        platform_unlock(&ahci_lock);
        while (pending) {
            struct command c = {.port = s};
            c.slot = __builtin_ctz(pending);
            pending &= ~(1u << c.slot);
            platform_lock(&ahci_lock);
            if (!(s->ci & (1u << c.slot)) || !(s->cmd & 1)) {
                platform_unlock(&ahci_lock);
                continue;
            }
            uint32_t error = snapshot(&c);
            if (!error)
                s->tfd = 0x80;
            platform_unlock(&ahci_lock);
            if (!error)
                error = transfer(&c);
            platform_lock(&ahci_lock);
            complete(&c, error);
            platform_unlock(&ahci_lock);
            if (c.prd)
                hal_free_pages(c.prd, c.pages);
        }
    }
}
