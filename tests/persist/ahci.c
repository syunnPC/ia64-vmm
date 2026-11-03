/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AHCI 1.3.1 polled persistence backend against a model HBA and ATA disk:
 * §10.6.3 BIOS/OS handoff, §10.4.3 HBA reset, §10.1.2 and §10.3.1 port
 * start, §10.4.2 COMRESET, §10.10.1 staggered spin-up, §4.2.2/§4.2.3
 * command list, command table and PRDT, §5.6.2 command completion,
 * §6.2.2.1 non-queued error recovery; ACS-3 IDENTIFY DEVICE, READ/WRITE DMA
 * EXT, FLUSH CACHE EXT, STANDBY IMMEDIATE and the Power-Up In Standby
 * spin-up (SET FEATURES 07h); then broken HBAs and devices.
 * Registers are plain memory: the model takes every value that differs from
 * the one it last published as a host write. DMA uses addresses above
 * 4 GiB on pages whose physical order is the reverse of their virtual order. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/plog_ahci.c"

#define POOL 16u
#define DISK 4096u /* backed sectors from m.window */
#define PORTS 6u
#define ABAR 0x800u
static _Alignas(4096) uint8_t pool[POOL * 4096];
static uint8_t mmio[ABAR], config[256], visible[DISK * 512], durable[DISK * 512];
static uint32_t shadow[ABAR / 4];
static unsigned pool_used;
static uint64_t ticks, panic_at, phys = UINT64_C(0x2345600000);
static bool panicking, alloc_fail;
static struct pci_device device = {.class_code = 0x010601,
                                   .owner = PCI_OWNER_VMM,
                                   .reservation = "vmm-media",
                                   .bars = {[5] = {.size = ABAR}},
                                   .command = 6};

enum fault_kind { FAULT_NONE, FAULT_DROP, FAULT_TFES, FAULT_TFES_BUSY, FAULT_HBFS, FAULT_SHORT };
enum { ATA, ATAPI, MULTIPLIER };
struct model_port {
    /* Properties. */
    bool device, presence_only, never_ready, no_relink;
    unsigned kind, ready_delay, link_delay;
    /* State. */
    bool link, d2h, st, cr, fre, fr, error, busy, sud, pod, stuck_busy;
    uint64_t link_at, linking_at, ready_at, cr_off_at, fr_off_at, comreset_at, done_at, spun_at;
    uint64_t clb, fb;
    uint32_t is, serr, tfd, sig, ci, sctl, prdbc, status;
    bool comreset;
};
static struct {
    /* HBA and device properties. */
    uint32_t cap, pi, cap2, vs;
    unsigned reset_delay, bios_release, bios_busy_at;
    bool hr_stuck, cr_stuck, fr_stuck, gone, write_cache, firmware_running, puis;
    uint16_t firmware_command;
    uint16_t id[256];
    uint64_t window;
    struct {
        unsigned opcode, skip;
        enum fault_kind kind;
    } fault;
    /* State. */
    bool active, ae, resetting, oos, bos, bb, standby;
    uint64_t hr_done_at, oos_at;
    uint32_t is;
    struct model_port port[PORTS];
    unsigned resets, comresets, commands, writes, reads, flushes, standbys, max_bytes;
    unsigned order[8], ordered;
} m;

void vmm_log(enum log_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
bool log_in_panic(void)
{
    return panicking;
}
void hal_panic(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}
uint8_t *platform_pci_map_physical(uint64_t base, uint64_t size)
{
    (void)base;
    assert(size <= ABAR);
    return mmio;
}
uint32_t platform_pci_host_read(uint16_t bdf, unsigned off, unsigned size)
{
    (void)bdf;
    assert(off + size <= sizeof(config));
    uint32_t value = 0;
    memcpy(&value, config + off, size);
    return value;
}
void platform_pci_host_write(uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    (void)bdf;
    assert(off + size <= sizeof(config));
    memcpy(config + off, &value, size);
}
void *hal_alloc_pages(size_t n, uint32_t flags)
{
    if (alloc_fail)
        return NULL;
    assert(pool_used + n <= POOL);
    uint8_t *p = pool + pool_used * 4096;
    pool_used += n;
    if (flags & HAL_PAGE_ZERO)
        memset(p, 0, n * 4096);
    return p;
}
bool amd64_memory_physical(uintptr_t address, uint64_t *pa)
{
    if (address < (uintptr_t)pool || address >= (uintptr_t)pool + sizeof(pool))
        return false;
    uint64_t page = (address - (uintptr_t)pool) / 4096;
    *pa = phys + (POOL - 1 - page) * 4096 + (address & 4095);
    return true;
}
/* Device view of one physical range; it never crosses a page. */
static uint8_t *model_dma(uint64_t pa, unsigned length)
{
    assert(config[4] & 4); /* Bus Master Enable */
    if (!(m.cap >> 31))
        assert(pa + length - 1 <= UINT32_MAX);
    assert(pa >= phys && pa < phys + POOL * 4096 && (pa & 4095) + length <= 4096);
    uint64_t page = (pa - phys) / 4096;
    return pool + (POOL - 1 - page) * 4096 + (pa & 4095);
}
static uint32_t r32(unsigned off)
{
    uint32_t value;
    memcpy(&value, mmio + off, 4);
    return value;
}
static void w32(unsigned off, uint32_t value)
{
    memcpy(mmio + off, &value, 4);
    shadow[off / 4] = value;
}
static bool written(unsigned off, uint32_t *value)
{
    *value = r32(off);
    return *value != shadow[off / 4];
}
static unsigned port_reg(unsigned p, unsigned off)
{
    return 0x100 + p * 0x80 + off;
}
static void put32(uint8_t *p, uint32_t value)
{
    memcpy(p, &value, 4);
}
static uint32_t get32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, 4);
    return value;
}

/* §4.2.3.3: each PRD's DBC is the byte count minus one, bit 0 set (an even
 * count); DBA is word aligned. */
static unsigned prd_bytes(const uint8_t *prdt, unsigned entries)
{
    unsigned total = 0;
    for (unsigned i = 0; i < entries; ++i) {
        uint32_t dba = get32(prdt + i * 16), dbc = get32(prdt + i * 16 + 12) & 0x3fffff;
        assert(!(dba & 1) && (dbc & 1) && !get32(prdt + i * 16 + 8));
        total += dbc + 1;
    }
    return total;
}
static void prd_copy(const uint8_t *prdt, unsigned entries, uint8_t *data, bool to_host)
{
    for (unsigned i = 0; i < entries; ++i) {
        uint64_t dba = get32(prdt + i * 16) | (uint64_t)get32(prdt + i * 16 + 4) << 32;
        unsigned n = (get32(prdt + i * 16 + 12) & 0x3fffff) + 1;
        uint8_t *at = model_dma(dba, n);
        if (to_host)
            memcpy(at, data, n);
        else
            memcpy(data, at, n);
        data += n;
    }
}
static uint32_t signature(unsigned kind)
{
    return kind == ATA ? 0x101 : kind == ATAPI ? 0xeb140101 : 0x96690101;
}
/* Status values: ACS-3 Error << 8 | Status; 0 is success (DRDY). */
static unsigned model_command(struct model_port *pt, const uint8_t *cfis, const uint8_t *prdt,
                              unsigned entries, bool write, unsigned *bytes)
{
    unsigned opcode = cfis[2], total = prd_bytes(prdt, entries);
    uint64_t lba = cfis[4] | (uint64_t)cfis[5] << 8 | (uint64_t)cfis[6] << 16 |
                   (uint64_t)cfis[8] << 24 | (uint64_t)cfis[9] << 32 | (uint64_t)cfis[10] << 40;
    unsigned count = cfis[12] | cfis[13] << 8;
    if (!count)
        count = 65536;
    assert(!m.standby); /* STANDBY IMMEDIATE is the last command. */
    if (m.ordered < 8)
        m.order[m.ordered++] = opcode;
    *bytes = 0;
    if (opcode == 0xec) { /* IDENTIFY DEVICE: PIO data-in */
        assert(!write && total == 512 && entries);
        if (pt->kind != ATA)
            return 0x0451;
        uint16_t id[256];
        memcpy(id, m.id, sizeof(id));
        /* ACS-3 PUIS: an incomplete response until the spin-up. */
        if (m.puis) {
            id[0] |= 4;
            id[2] = 0x37c8;
            for (unsigned i = 60; i < 256; ++i)
                id[i] = 0;
        }
        prd_copy(prdt, entries, (uint8_t *)id, true);
        *bytes = 512;
        return 0;
    }
    if (opcode == 0xef) { /* SET FEATURES */
        assert(!entries && !write);
        if (cfis[3] != 7)
            return 0x0451;
        m.puis = false;
        return 0;
    }
    if (opcode == 0x25 || opcode == 0x35) { /* READ/WRITE DMA EXT */
        assert(cfis[7] & 0x40);             /* LBA */
        assert(write == (opcode == 0x35) && total == count * 512);
        uint64_t capacity = m.id[100] | (uint64_t)m.id[101] << 16 | (uint64_t)m.id[102] << 32 |
                            (uint64_t)m.id[103] << 48;
        if (lba >= capacity || count > capacity - lba)
            return 0x1051; /* IDNF */
        assert(lba >= m.window && lba - m.window + count <= DISK);
        uint8_t *at = visible + (lba - m.window) * 512;
        if (total > m.max_bytes)
            m.max_bytes = total;
        if (write) {
            ++m.writes;
            prd_copy(prdt, entries, at, false);
            if (!m.write_cache)
                memcpy(durable + (lba - m.window) * 512, at, total);
        } else {
            ++m.reads;
            prd_copy(prdt, entries, at, true);
        }
        *bytes = total;
        return 0;
    }
    if (opcode == 0xea) { /* FLUSH CACHE EXT */
        assert(!entries && !write);
        ++m.flushes;
        memcpy(durable, visible, sizeof(durable));
        return 0;
    }
    if (opcode == 0xe0) { /* STANDBY IMMEDIATE */
        assert(!entries && !write);
        ++m.standbys;
        m.standby = true;
        return 0;
    }
    return 0x0451; /* ABRT */
}
/* §5.6.2: fetch the command header, the command FIS and the PRDT. */
static void model_issue(struct model_port *pt)
{
    assert(pt->st && pt->cr && pt->d2h && !pt->error && !pt->busy && !(pt->tfd & 0x88));
    uint8_t *header = model_dma(pt->clb, 32);
    uint32_t dw0 = get32(header);
    unsigned entries = dw0 >> 16;
    assert((dw0 & 31) == 5 && !(dw0 & (1u << 5)) &&
           !(dw0 >> 12 & 15)); /* CFL 5, not ATAPI, PMP 0 */
    uint64_t ctba = get32(header + 8) | (uint64_t)get32(header + 12) << 32;
    assert(!(ctba & 127) && entries <= 8);
    uint8_t *table = model_dma(ctba, 0x80 + entries * 16);
    assert(table[0] == 0x27 && table[1] == 0x80); /* H2D Register FIS, C bit, PM port 0 */
    ++m.commands;
    unsigned bytes,
        status = model_command(pt, table, table + 0x80, entries, (dw0 >> 6) & 1, &bytes);
    enum fault_kind kind = FAULT_NONE;
    if (m.fault.kind && m.fault.opcode == table[2]) {
        if (m.fault.skip)
            --m.fault.skip;
        else {
            kind = m.fault.kind;
            m.fault.kind = FAULT_NONE;
        }
    }
    pt->busy = true;
    pt->done_at = kind == FAULT_DROP ? UINT64_MAX : ticks + 2;
    pt->prdbc = kind == FAULT_SHORT ? bytes - 512 : bytes;
    pt->tfd = 0x80;
    if (kind == FAULT_TFES || kind == FAULT_TFES_BUSY)
        status = 0x0451;
    if (kind == FAULT_TFES_BUSY)
        pt->stuck_busy = true;
    if (kind == FAULT_HBFS)
        status = 0x10000;
    pt->status = status;
}
static void model_complete(struct model_port *pt)
{
    unsigned status = pt->status;
    pt->busy = false;
    put32(model_dma(pt->clb, 32) + 4, pt->prdbc);
    if (status == 0x10000) {
        pt->is |= 1u << 29;
        pt->error = true;
        pt->tfd = 0x50;
        return;
    } /* HBFS */
    if (status) {
        pt->tfd = pt->stuck_busy ? 0x80 | (status & 0xffff) : status;
        pt->is |= 1u << 30;
        pt->error = true; /* TFES */
        return;
    }
    pt->tfd = 0x50;
    pt->is |= 1;
    pt->ci = 0;
}
static void model_hba_reset(void)
{
    ++m.resets;
    m.firmware_running = false;
    m.is = 0;
    for (unsigned p = 0; p < PORTS; ++p) {
        struct model_port *pt = &m.port[p];
        bool device = pt->device, presence = pt->presence_only, never = pt->never_ready,
             relink = pt->no_relink;
        unsigned kind = pt->kind, ready = pt->ready_delay, delay = pt->link_delay;
        uint64_t clb = pt->clb, fb = pt->fb;
        memset(pt, 0, sizeof(*pt));
        pt->device = device;
        pt->presence_only = presence;
        pt->never_ready = never;
        pt->kind = kind;
        pt->ready_delay = ready;
        pt->link_delay = delay;
        pt->no_relink = relink;
        pt->clb = clb;
        pt->fb = fb;
        pt->tfd = 0x7f;
        pt->sig = UINT32_MAX;
        pt->link_at = UINT64_MAX;
        pt->ready_at = UINT64_MAX;
        pt->cr_off_at = pt->fr_off_at = pt->linking_at = UINT64_MAX;
        pt->sud = !(m.cap >> 27 & 1);
        pt->pod = true;
        /* Without staggered spin-up every port starts a COMRESET. */
        if (pt->sud && device && !presence) {
            pt->link_at = m.hr_done_at + delay;
            pt->linking_at = m.hr_done_at;
        }
    }
}
static void model_port(unsigned p)
{
    struct model_port *pt = &m.port[p];
    uint32_t v;
    bool implemented = m.pi >> p & 1, bios = m.bos;
    for (unsigned off = 0; off < 0x80; off += 4) {
        bool changed = written(port_reg(p, off), &v);
        if (!changed)
            continue;
        assert(implemented && !bios && m.ae && !m.resetting);
        /* PxTFD, PxSIG, PxSSTS, reserved and vendor registers are not written. */
        assert(off <= 0x18 || off == 0x2c || off == 0x30 || off == 0x34 || off == 0x38);
    }
    if (!implemented)
        return;
    if (written(port_reg(p, 0), &v) || written(port_reg(p, 4), &v))
        assert(!pt->st && !pt->cr);
    if (written(port_reg(p, 8), &v) || written(port_reg(p, 12), &v))
        assert(!pt->fre && !pt->fr);
    if (!(m.cap >> 31))
        assert(!r32(port_reg(p, 4)) && !r32(port_reg(p, 12)));
    if (written(port_reg(p, 0x10), &v))
        pt->is &= ~v;
    if (written(port_reg(p, 0x30), &v))
        pt->serr &= ~v;
    assert(!r32(port_reg(p, 0x14)) && !r32(port_reg(p, 0x34))); /* no interrupts, no NCQ */
    if (written(port_reg(p, 0x2c), &v)) {
        /* §3.3.11: DET only changes while ST is clear. §10.4.2: DET 1
         * for at least 1 ms, then 0; the link then comes up again. */
        assert(!pt->st && !(v & ~15u));
        if ((v & 15) == 1 && !pt->comreset) {
            pt->comreset = true;
            pt->comreset_at = ticks;
            pt->link = pt->d2h = false;
            pt->tfd = 0x7f;
            pt->link_at = UINT64_MAX;
            ++m.comresets;
        } else if (!(v & 15) && pt->comreset) {
            assert(ticks - pt->comreset_at >= 1);
            pt->comreset = false;
            pt->stuck_busy = false;
            pt->error = false;
            if (pt->device && !pt->presence_only && !pt->no_relink) {
                pt->link_at = ticks + pt->link_delay;
                pt->linking_at = ticks;
            }
        } else
            assert(false);
        pt->sctl = v;
    }
    if (written(port_reg(p, 0x18), &v)) {
        assert(!(v >> 28) && !(v & 8)); /* no ICC request, no CLO */
        bool st = v & 1, fre = v >> 4 & 1, sud = v >> 1 & 1;
        if ((m.cap >> 27 & 1) && sud && !pt->sud) {
            /* §10.10.1: one port at a time; every port spun up before
             * shows a device or had 10 ms for its OOB signalling. */
            for (unsigned q = 0; q < PORTS; ++q)
                if (q != p && m.port[q].spun_at)
                    assert(m.port[q].link || m.port[q].linking_at <= ticks ||
                           ticks - m.port[q].spun_at >= 10);
            pt->sud = true;
            pt->spun_at = ticks;
            if (pt->device && !pt->presence_only) {
                pt->link_at = ticks + pt->link_delay;
                pt->linking_at = ticks + 1;
            }
        }
        if (fre && !pt->fre) {
            /* §4.2.1: a 256-byte aligned receive area. */
            pt->fb = r32(port_reg(p, 8)) | (uint64_t)r32(port_reg(p, 12)) << 32;
            assert(pt->fb && !(pt->fb & 255));
            (void)model_dma(pt->fb, 256);
            pt->fre = pt->fr = true;
            pt->fr_off_at = UINT64_MAX;
        } else if (!fre && pt->fre) {
            /* §3.3.7: FRE is cleared only after ST and CR. */
            assert(!st && !pt->st && !pt->cr);
            pt->fre = false;
            pt->fr_off_at = m.fr_stuck ? UINT64_MAX : ticks + 1;
        }
        if (st && !pt->st) {
            /* §10.3.1: FRE first, then ST with BSY, DRQ clear and DET 3h;
             * §10.1.2: PxSERR cleared before. */
            assert(pt->fre && pt->link && pt->d2h && !(pt->tfd & 0x88) && !pt->cr && !pt->serr);
            pt->clb = r32(port_reg(p, 0)) | (uint64_t)r32(port_reg(p, 4)) << 32;
            assert(pt->clb && !(pt->clb & 1023));
            pt->st = pt->cr = true;
            pt->error = false;
            pt->cr_off_at = UINT64_MAX;
        } else if (!st && pt->st) {
            /* §3.3.14: clearing ST clears PxCI; CR follows. */
            pt->st = false;
            pt->ci = 0;
            pt->busy = false;
            pt->error = false;
            pt->cr_off_at = m.cr_stuck ? UINT64_MAX : ticks + 1;
        }
    }
    if (written(port_reg(p, 0x38), &v)) {
        assert(v == (pt->ci | 1) && !pt->ci); /* slot 0 only, write-1-to-set */
        pt->ci = 1;
        model_issue(pt);
    }
    if (ticks >= pt->cr_off_at) {
        pt->cr = false;
        pt->cr_off_at = UINT64_MAX;
    }
    if (ticks >= pt->fr_off_at) {
        pt->fr = false;
        pt->fr_off_at = UINT64_MAX;
    }
    if (!pt->link && ticks >= pt->link_at) {
        pt->link = true;
        pt->link_at = pt->linking_at = UINT64_MAX;
        pt->tfd = 0x80;
        pt->serr |= 1u << 26 | 1u << 16;
        pt->ready_at = pt->never_ready ? UINT64_MAX : ticks + pt->ready_delay;
    }
    /* The first D2H Register FIS reaches the HBA once FRE is set. */
    if (pt->link && !pt->d2h && pt->fre && ticks >= pt->ready_at) {
        pt->d2h = true;
        pt->tfd = 0x50;
        pt->sig = signature(pt->kind);
    }
    if (pt->busy && ticks >= pt->done_at)
        model_complete(pt);
    w32(port_reg(p, 0), r32(port_reg(p, 0)));
    w32(port_reg(p, 4), r32(port_reg(p, 4)));
    w32(port_reg(p, 8), r32(port_reg(p, 8)));
    w32(port_reg(p, 12), r32(port_reg(p, 12)));
    w32(port_reg(p, 0x10), pt->is);
    w32(port_reg(p, 0x14), 0);
    w32(port_reg(p, 0x18), (pt->st ? 1u : 0) | (pt->sud ? 2u : 0) | (pt->pod ? 4u : 0) |
                               (pt->fre ? 16u : 0) | (pt->fr ? 1u << 14 : 0) |
                               (pt->cr ? 1u << 15 : 0));
    w32(port_reg(p, 0x20), pt->tfd);
    w32(port_reg(p, 0x24), pt->sig);
    /* DET 1h: presence detected, communication not yet established. */
    w32(port_reg(p, 0x28), pt->link                                       ? 0x113u
                           : pt->presence_only || ticks >= pt->linking_at ? 1u
                                                                          : 0);
    w32(port_reg(p, 0x2c), pt->sctl);
    w32(port_reg(p, 0x30), pt->serr);
    w32(port_reg(p, 0x34), 0);
    w32(port_reg(p, 0x38), pt->ci);
}
static void model_tick(void)
{
    if (!m.active)
        return;
    if (m.gone) {
        memset(mmio, 0xff, sizeof(mmio));
        return;
    }
    uint32_t v;
    /* Ports the firmware left running may DMA until the HBA reset; while
     * the BIOS owns the HBA its command register stays. */
    assert(!m.firmware_running || m.bos || !(config[4] & 4));
    static const unsigned ro[] = {0, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24};
    for (unsigned i = 0; i < sizeof(ro) / sizeof(ro[0]); ++i)
        assert(!written(ro[i], &v));
    if (written(0x28, &v)) {
        /* §10.6.3: only OOS is requested; BOS stays the BIOS's. */
        assert((m.cap2 & 1) && (v & 2) && !(v & 8) && (!m.bos || (v & 1)));
        if (!m.oos) {
            m.oos = true;
            m.oos_at = ticks;
        }
    }
    if (m.oos && m.bos && m.bios_busy_at && ticks >= m.oos_at + m.bios_busy_at)
        m.bb = true;
    if (m.oos && m.bos && ticks >= m.oos_at + m.bios_release) {
        m.bos = false;
        m.bb = false;
    }
    bool bios = m.bos;
    /* §10.6.3: while the BIOS owns the HBA, the firmware's command register stays. */
    if (bios)
        assert((config[4] | config[5] << 8) == m.firmware_command);
    if (written(4, &v)) {
        assert(!bios && !(v & 2) && !(v & 4)); /* GHC.IE and MRSM stay clear */
        if (v & 1) {
            /* §10.4.3: AE before or with HR when CAP.SAM is clear. */
            assert(v >> 31);
            if (!m.resetting) {
                m.resetting = true;
                m.hr_done_at = m.hr_stuck ? UINT64_MAX : ticks + m.reset_delay;
            }
        }
        m.ae = v >> 31;
    }
    if (written(8, &v)) {
        assert(!bios);
        m.is &= ~v;
    }
    if (m.resetting && ticks >= m.hr_done_at) {
        m.resetting = false;
        m.ae = false;
        model_hba_reset();
    }
    for (unsigned p = 0; p < PORTS; ++p)
        model_port(p);
    w32(0, m.cap);
    w32(4, (m.ae ? 1u << 31 : 0) | (m.resetting ? 1u : 0));
    w32(8, m.is);
    w32(0xc, m.pi);
    w32(0x10, m.vs);
    w32(0x24, m.cap2);
    w32(0x28, (m.bos ? 1u : 0) | (m.oos ? 2u : 0) | (m.bb ? 16u : 0));
}
uint64_t hal_counter_hz(void)
{
    return 1000;
}
uint64_t hal_counter(void)
{
    if (++ticks == panic_at)
        panicking = true;
    model_tick();
    return ticks;
}

static void identify_data(uint64_t capacity)
{
    memset(m.id, 0, sizeof(m.id));
    const char model[] = "MODEL AHCI DISK                         ";
    for (unsigned i = 0; i < 20; ++i)
        m.id[27 + i] = (uint16_t)(model[i * 2] << 8 | model[i * 2 + 1]);
    m.id[49] = 3u << 8; /* LBA, DMA */
    m.id[60] = (uint16_t)(capacity > 0x0fffffff ? 0xffff : capacity);
    m.id[61] = (uint16_t)(capacity > 0x0fffffff ? 0x0fff : capacity >> 16);
    m.id[82] = 1u << 5 | 1u << 14;                      /* volatile write cache, NOP */
    m.id[83] = 0x4000 | 1u << 10 | 1u << 12 | 1u << 13; /* LBA48, FLUSH CACHE (EXT) */
    m.id[84] = 0x4000;
    m.id[85] = (uint16_t)(m.write_cache ? 1u << 5 : 0);
    m.id[86] = 1u << 10 | 1u << 12 | 1u << 13;
    m.id[87] = 0x4000;
    for (unsigned i = 0; i < 4; ++i)
        m.id[100 + i] = (uint16_t)(capacity >> (16 * i));
    m.id[106] = 0x4000; /* one logical sector per physical sector */
}
static void setup(void)
{
    memset(&hba, 0, sizeof(hba));
    memset(&block, 0, sizeof(block));
    memset(&m, 0, sizeof(m));
    memset(mmio, 0, sizeof(mmio));
    memset(shadow, 0, sizeof(shadow));
    memset(config, 0, sizeof(config));
    memset(visible, 0, sizeof(visible));
    memset(durable, 0, sizeof(durable));
    pool_used = 0;
    ticks = panic_at = 0;
    panicking = alloc_fail = false;
    phys = UINT64_C(0x2345600000);
    device.bars[5].io = false;
    device.bars[5].size = ABAR;
    /* S64A, SNCQ, NCS 32, NP 6; AHCI 1.3.1; ports 0-5 implemented. */
    m.cap = 1u << 31 | 1u << 30 | 31u << 8 | (PORTS - 1);
    m.pi = (1u << PORTS) - 1;
    m.vs = 0x10301;
    m.reset_delay = 5;
    m.write_cache = true;
    m.active = true;
    m.firmware_command = 6;
    for (unsigned p = 0; p < PORTS; ++p) {
        m.port[p].tfd = 0x7f;
        m.port[p].sig = UINT32_MAX;
        m.port[p].ready_delay = 20;
        m.port[p].link_delay = 10;
        m.port[p].link_at = m.port[p].ready_at = m.port[p].cr_off_at = m.port[p].fr_off_at =
            UINT64_MAX;
        m.port[p].linking_at = UINT64_MAX;
    }
    m.port[2].device = true;
    identify_data(DISK);
    config[4] = 6;
    /* Power-on state: every port with a device has its link up. */
    for (unsigned p = 0; p < PORTS; ++p)
        if (m.port[p].device && !m.port[p].presence_only) {
            m.port[p].link = true;
            m.port[p].tfd = 0x80;
        }
}
static void program(void)
{
    w32(0, m.cap);
    w32(0xc, m.pi);
    w32(0x10, m.vs);
    w32(0x24, m.cap2);
    m.bos = m.cap2 & 1;
    w32(0x28, m.bos ? 1u : 0);
    for (unsigned p = 0; p < PORTS; ++p) {
        w32(port_reg(p, 0x18), 6);
        w32(port_reg(p, 0x20), m.port[p].tfd);
        w32(port_reg(p, 0x24), m.port[p].sig);
        w32(port_reg(p, 0x28), m.port[p].link ? 0x113u : 0);
        m.port[p].sud = m.port[p].pod = true;
    }
}
static const struct plog_block *begin(void)
{
    program();
    return amd64_plog_ahci(&device, true);
}
static void pattern(uint8_t *p, unsigned bytes, unsigned seed)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = (uint8_t)(i * 7 + seed * 13 + 1);
}
/* The selected port idle: ST, CR, FRE and FR clear (§10.1.2). */
static bool idle(unsigned p)
{
    return !m.port[p].st && !m.port[p].cr && !m.port[p].fre && !m.port[p].fr &&
           !(r32(port_reg(p, 0x18)) & 0xc011);
}

int main(void)
{
    static uint8_t data[64 * 512], back[64 * 512];
    /* §10.4.3, §10.1.2, §10.3.1 from the power-on state. */
    setup();
    const struct plog_block *b = begin();
    assert(b && b->sectors == DISK && m.resets == 1 && m.port[2].st && m.port[2].fre);
    assert(m.port[2].clb >= phys && m.port[2].fb >= phys && r32(port_reg(2, 4)) == phys >> 32);
    for (unsigned p = 0; p < PORTS; ++p)
        if (p != 2)
            assert(!r32(port_reg(p, 0)) && !m.port[p].fre);
    assert((config[4] & 6) == 6 && !(r32(4) & 2) && (r32(4) >> 31));
    assert(m.order[0] == 0xec);
    /* Writes, flushes and reads through one to four PRDs; requests above
     * 32 sectors take several commands. */
    const unsigned counts[] = {1, 7, 8, 9, 16, 17, 31, 32, 33, 40, 64};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        unsigned n = counts[i], lba = 100 + i * 70, commands = m.commands;
        pattern(data, n * 512, i);
        assert(b->write(lba, n, data) && !memcmp(visible + lba * 512, data, n * 512));
        assert(m.commands == commands + (n + 31) / 32);
        assert(memcmp(durable + lba * 512, data, n * 512)); /* still in the volatile write cache */
        unsigned flushes = m.flushes;
        assert(b->flush() && m.flushes == flushes + 1 &&
               !memcmp(durable + lba * 512, data, n * 512));
        memset(back, 0, sizeof(back));
        assert(b->read(lba, n, back) && !memcmp(back, data, n * 512));
    }
    assert(m.max_bytes == 16384);
    for (unsigned i = 0; i < 100; ++i) {
        pattern(data, 512, 50 + i);
        assert(b->write(i, 1, data) && b->read(i, 1, back) && !memcmp(back, data, 512));
    }
    /* Range checks need no command. */
    unsigned commands = m.commands;
    assert(!b->write(DISK, 1, data) && !b->write(DISK - 1, 2, data) &&
           !b->read(UINT64_MAX, 1, back));
    assert(m.commands == commands && b->write(DISK - 1, 1, data));
    /* Without a volatile write cache the flush still completes. */
    setup();
    m.write_cache = false;
    identify_data(DISK);
    b = begin();
    assert(b && b->write(3, 1, data) && !memcmp(durable + 3 * 512, data, 512) && b->flush());
    /* Capacity beyond 2^32 sectors: LBA bits 47:24 in the FIS. */
    setup();
    m.window = (UINT64_C(1) << 32) - 8;
    identify_data(UINT64_C(1) << 33);
    b = begin();
    assert(b && b->sectors == UINT64_C(1) << 33);
    pattern(data, 40 * 512, 21);
    assert(b->write(m.window, 40, data) && !memcmp(visible, data, 40 * 512));
    assert(b->read(m.window, 40, back) && !memcmp(back, data, 40 * 512));
    pattern(data, 512, 22);
    assert(b->write((UINT64_C(1) << 32) + 100, 1, data) && !memcmp(visible + 108 * 512, data, 512));
    /* 512-byte logical sectors on 4 KiB physical sectors. */
    setup();
    m.id[106] = 0x4000 | 1u << 13 | 3;
    assert(begin());
    /* Sparse PI; the device on the last implemented port. */
    setup();
    m.pi = 0x21;
    m.port[2].device = false;
    m.port[2].link = false;
    m.port[5].device = true;
    m.port[5].link = true;
    m.port[5].tfd = 0x80;
    b = begin();
    assert(b && m.port[5].st && !m.port[0].fre);
    /* A disk spinning up: ready within 31 s. */
    setup();
    m.port[2].ready_delay = 8000;
    b = begin();
    assert(b && ticks >= 8000);
    /* §10.10.1 staggered spin-up, one port at a time: no link until
     * PxCMD.SUD; links of their own lengths. */
    setup();
    m.cap |= 1u << 27;
    m.port[2].link_delay = 300;
    b = begin();
    assert(b && m.port[2].sud && m.port[2].st);
    for (unsigned q = 0; q < PORTS; ++q)
        assert(m.port[q].sud);
    /* The settle window: a single disk whose link comes late is taken; a
     * second disk linking late refuses the start. */
    setup();
    m.port[2].link_delay = 800;
    b = begin();
    assert(b && m.port[2].st);
    setup();
    m.port[4].device = true;
    m.port[4].link_delay = 500;
    assert(!begin() && idle(2) && idle(4));
    /* Power-Up In Standby: SET FEATURES 07h, then a complete IDENTIFY. */
    setup();
    m.puis = true;
    b = begin();
    assert(b && !m.puis && b->sectors == DISK && m.order[0] == 0xec && m.order[1] == 0xef &&
           m.order[2] == 0xec);
    /* An incomplete IDENTIFY without the spin-up code is refused. */
    setup();
    m.id[0] |= 4;
    m.id[2] = 0x8c73;
    assert(!begin() && idle(2));
    /* Without S64A, DMA below 4 GiB. */
    setup();
    m.cap &= ~(1u << 31);
    phys = 0x7f000000;
    b = begin();
    assert(b && m.port[2].clb < UINT32_MAX && b->write(5, 8, data) && b->read(5, 8, back));
    /* Without S64A and RAM above 4 GiB: refused before any DMA pointer. */
    setup();
    m.cap &= ~(1u << 31);
    assert(!begin() && !r32(port_reg(2, 0)) && !r32(port_reg(2, 8)) && !m.port[2].fre);

    /* §10.6.3 BIOS/OS handoff: BOS cleared within 25 ms, or BB and up to 2 s. */
    setup();
    m.cap2 = 1;
    m.bios_release = 10;
    assert(begin() && m.oos && !m.bos);
    setup();
    m.cap2 = 1;
    m.bios_busy_at = 5;
    m.bios_release = 1500;
    assert(begin() && ticks >= 1500);
    setup();
    m.cap2 = 1;
    m.bios_busy_at = 5;
    m.bios_release = UINT32_MAX;
    assert(!begin() && ticks >= 2000 && ticks < 2200 && !m.resets);
    setup();
    m.cap2 = 1;
    m.bios_release = UINT32_MAX;
    assert(!begin() && ticks >= 25 && ticks < 200 && !m.resets);
    /* Before AHCI 1.2 offset 24h is reserved and reads 0: no handoff. */
    setup();
    m.vs = 0x10100;
    assert(begin() && !m.oos);
    /* The BIOS busy with bus mastering on: the command register stays until
     * BOS clears; then bus mastering off until the reset. */
    setup();
    m.cap2 = 1;
    m.bios_busy_at = 5;
    m.bios_release = 300;
    m.firmware_running = true;
    m.port[2].st = m.port[2].cr = m.port[2].fre = m.port[2].fr = true;
    m.port[2].d2h = true;
    m.port[2].tfd = 0x50;
    m.port[2].clb = 0x7e000000;
    m.port[2].fb = 0x7e000400;
    program();
    w32(port_reg(2, 0), 0x7e000000);
    w32(port_reg(2, 8), 0x7e000400);
    w32(port_reg(2, 0x18), 0xc017);
    m.ae = true;
    w32(4, 1u << 31);
    b = amd64_plog_ahci(&device, true);
    assert(b && m.oos && !m.bos && m.resets == 1 && ticks >= 300);
    /* The firmware left a port running: bus mastering waits for the reset. */
    setup();
    program();
    m.firmware_running = true;
    m.port[2].st = m.port[2].cr = m.port[2].fre = m.port[2].fr = true;
    m.port[2].d2h = true;
    m.port[2].tfd = 0x50;
    m.port[2].sig = 0x101;
    m.port[2].clb = 0x7e000000;
    m.port[2].fb = 0x7e000400;
    m.ae = true;
    w32(port_reg(2, 0), 0x7e000000);
    w32(port_reg(2, 8), 0x7e000400);
    w32(4, 1u << 31);
    w32(port_reg(2, 0x18), 0xc017);
    b = amd64_plog_ahci(&device, true);
    assert(b && m.resets == 1 && m.port[2].clb >= phys);

    /* Broken HBAs and devices: bounded, the port left idle. */
    setup();
    m.hr_stuck = true;
    assert(!begin() && ticks >= 1000 && ticks < 1200 && !(config[4] & 4));
    setup();
    m.port[2].device = false;
    m.port[2].link = false;
    assert(!begin() && ticks >= 1000 && ticks < 1200);
    setup();
    m.port[4].device = true;
    m.port[4].link = true;
    m.port[4].tfd = 0x80;
    assert(!begin() && idle(2) && idle(4));
    setup();
    m.port[4].presence_only = true;
    assert(!begin() && ticks >= 1000 && ticks < 1200 && idle(2));
    setup();
    m.port[2].kind = ATAPI;
    assert(!begin() && idle(2) && !m.commands);
    setup();
    m.port[2].kind = MULTIPLIER;
    assert(!begin() && idle(2) && !m.commands);
    setup();
    m.port[2].never_ready = true;
    assert(!begin() && ticks >= 31000 && ticks < 33000 && idle(2));
    setup();
    m.id[83] &= ~(1u << 10);
    assert(!begin() && idle(2));
    setup();
    m.id[83] &= ~(1u << 13);
    assert(!begin() && idle(2));
    setup();
    m.id[83] = (uint16_t)(m.id[83] & 0x3fff);
    assert(!begin() && idle(2));
    setup();
    m.id[0] = 0x8000;
    assert(!begin() && idle(2));
    setup();
    identify_data(0);
    assert(!begin() && idle(2));
    setup();
    m.id[106] = 0x4000 | 1u << 12;
    m.id[117] = 2048;
    assert(!begin() && idle(2));
    setup();
    m.fault.opcode = 0xec;
    m.fault.kind = FAULT_TFES;
    assert(!begin() && idle(2));
    setup();
    m.fault.opcode = 0xec;
    m.fault.kind = FAULT_SHORT;
    assert(!begin() && idle(2));
    setup();
    m.fault.opcode = 0xec;
    m.fault.kind = FAULT_DROP;
    assert(!begin() && ticks >= 30000 && ticks < 32000 && idle(2));
    setup();
    alloc_fail = true;
    assert(!begin() && !m.port[2].fre);
    setup();
    memset(mmio, 0xff, sizeof(mmio));
    m.active = false;
    assert(!amd64_plog_ahci(&device, true) && ticks < 100);
    setup();
    device.bars[5].io = true;
    assert(!begin() && !m.resets);
    setup();
    device.bars[5].size = 0x100 + 2 * 0x80; /* ports beyond ABAR */
    assert(!begin() && !m.resets);

    /* §6.2.2.1: a task-file error fails that command only. */
    setup();
    b = begin();
    assert(b);
    m.fault.opcode = 0x35;
    m.fault.kind = FAULT_TFES;
    assert(!b->write(0, 1, data) && b->write(0, 1, data) && b->flush() && !m.comresets);
    /* BSY still set after the error: COMRESET before ST. */
    m.fault.opcode = 0x35;
    m.fault.kind = FAULT_TFES_BUSY;
    assert(!b->write(1, 1, data) && m.comresets == 1 && b->write(1, 1, data));
    setup();
    b = begin();
    m.fault.opcode = 0xea;
    m.fault.kind = FAULT_TFES;
    assert(b && b->write(0, 1, data) && !b->flush() && b->flush());
    setup();
    b = begin();
    m.fault.opcode = 0x25;
    m.fault.kind = FAULT_SHORT;
    assert(b && !b->read(0, 8, back) && b->read(0, 8, back));
    /* Recovery that cannot stop the port stops the backend; the HBA reset
     * idles the port (§10.1.2). */
    setup();
    b = begin();
    m.fault.opcode = 0x35;
    m.fault.kind = FAULT_TFES;
    assert(b);
    m.cr_stuck = true;
    uint64_t at = ticks;
    assert(!b->write(0, 1, data) && ticks - at < 2000 && m.resets == 2 && idle(2));
    commands = m.commands;
    assert(!b->write(0, 1, data) && !b->read(0, 1, back) && !b->flush() && m.commands == commands);
    /* FIS receive that does not stop: the HBA reset. */
    setup();
    b = begin();
    m.fault.opcode = 0x35;
    m.fault.kind = FAULT_HBFS;
    m.fr_stuck = true;
    assert(b && !b->write(0, 1, data) && m.resets == 2 && idle(2));
    /* A COMRESET after which the link does not return: bounded, the port
     * stopped, nothing more issued. */
    setup();
    b = begin();
    m.fault.opcode = 0x35;
    m.fault.kind = FAULT_TFES_BUSY;
    m.port[2].no_relink = true;
    assert(b);
    at = ticks;
    assert(!b->write(0, 1, data) && m.comresets == 1 && ticks - at >= 31000 && ticks - at < 33000 &&
           idle(2));
    commands = m.commands;
    assert(!b->write(0, 1, data) && !b->flush() && m.commands == commands);
    /* A panic during the recovery: no COMRESET, no restart of the port. */
    setup();
    b = begin();
    m.fault.opcode = 0x35;
    m.fault.kind = FAULT_TFES_BUSY;
    assert(b);
    at = ticks;
    panic_at = at + 5;
    assert(!b->write(0, 1, data) && !m.comresets && !m.port[2].st);
    /* A panic while the link does not return: the wait for the device ends. */
    setup();
    b = begin();
    m.fault.opcode = 0x35;
    m.fault.kind = FAULT_TFES_BUSY;
    m.port[2].no_relink = true;
    assert(b);
    at = ticks;
    panic_at = at + 1000;
    assert(!b->write(0, 1, data) && m.comresets == 1 && ticks - at < 2000);
    /* A host bus fatal error stops the backend; the port is stopped. */
    setup();
    b = begin();
    m.fault.opcode = 0x35;
    m.fault.kind = FAULT_HBFS;
    assert(b);
    at = ticks;
    assert(!b->write(0, 1, data) && idle(2) && ticks - at < 1000 && m.resets == 1);
    commands = m.commands;
    assert(!b->write(0, 1, data) && !b->read(0, 1, back) && !b->flush() && m.commands == commands);
    /* A lost completion: bounded wait, the backend stops, the port is stopped. */
    setup();
    b = begin();
    assert(b);
    m.fault.opcode = 0x35;
    m.fault.skip = 3;
    m.fault.kind = FAULT_DROP;
    for (unsigned i = 0; i < 3; ++i)
        assert(b->write(i, 1, data));
    at = ticks;
    assert(!b->write(3, 1, data) && ticks - at >= 30000 && ticks - at < 32000 && idle(2) &&
           m.resets == 1);
    commands = m.commands;
    at = ticks;
    assert(!b->read(0, 1, back) && !b->write(0, 1, data) && !b->flush() && m.commands == commands &&
           ticks == at);
    /* A controller that is gone: all-ones registers. */
    setup();
    b = begin();
    assert(b);
    m.gone = true;
    memset(mmio, 0xff, sizeof(mmio));
    at = ticks;
    assert(!b->write(0, 1, data) && ticks - at < 100);
    /* A panic during the wait: return at once, the port untouched. */
    setup();
    b = begin();
    m.fault.opcode = 0x35;
    m.fault.kind = FAULT_DROP;
    assert(b);
    panic_at = ticks + 50;
    at = ticks;
    assert(!b->write(0, 1, data) && ticks - at < 1000 && m.port[2].st);
    /* After a VMM panic no command reaches the device. */
    setup();
    b = begin();
    assert(b);
    panicking = true;
    commands = m.commands;
    assert(!b->write(0, 1, data) && !b->read(0, 1, back) && !b->flush() && m.commands == commands);
    assert(!r32(port_reg(2, 0x38)));

    /* Shutdown: FLUSH CACHE EXT, then STANDBY IMMEDIATE before power is
     * removed; only the flush before a machine reset. */
    setup();
    b = begin();
    assert(b && b->write(7, 1, data));
    m.ordered = 0;
    amd64_plog_ahci_shutdown(true);
    assert(m.ordered == 2 && m.order[0] == 0xea && m.order[1] == 0xe0 && m.standby);
    assert(!memcmp(durable, visible, sizeof(durable)));
    commands = m.commands;
    assert(!b->write(0, 1, data) && !b->read(0, 1, back) && !b->flush());
    amd64_plog_ahci_shutdown(true);
    assert(m.commands == commands);
    setup();
    b = begin();
    assert(b && b->write(7, 1, data));
    m.ordered = 0;
    amd64_plog_ahci_shutdown(false);
    assert(m.ordered == 1 && m.order[0] == 0xea && !m.standby &&
           !memcmp(durable, visible, sizeof(durable)));
    /* A failed flush: no STANDBY IMMEDIATE. */
    setup();
    b = begin();
    assert(b && b->write(7, 1, data));
    m.fault.opcode = 0xea;
    m.fault.kind = FAULT_TFES;
    m.ordered = 0;
    amd64_plog_ahci_shutdown(true);
    assert(m.ordered == 1 && m.order[0] == 0xea && !m.standbys && !m.standby);
    /* hal_shutdown: power is removed unless the machine is reset. */
    assert(amd64_power_removed(HAL_SHUTDOWN_POWER_OFF, false, false) &&
           amd64_power_removed(HAL_SHUTDOWN_HALT, false, false));
    assert(!amd64_power_removed(HAL_SHUTDOWN_RESET, false, false));
    assert(amd64_power_removed(HAL_SHUTDOWN_RESET, true, false) &&
           amd64_power_removed(HAL_SHUTDOWN_RESET, false, true));
    /* Not after a panic, nor after the backend stopped. */
    setup();
    b = begin();
    assert(b);
    panicking = true;
    commands = m.commands;
    amd64_plog_ahci_shutdown(true);
    assert(m.commands == commands);
    setup();
    b = begin();
    m.fault.opcode = 0x35;
    m.fault.kind = FAULT_HBFS;
    assert(b && !b->write(0, 1, data));
    commands = m.commands;
    amd64_plog_ahci_shutdown(true);
    assert(m.commands == commands);
    puts("PLOG AHCI: passed");
}
