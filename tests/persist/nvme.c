/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NVMe 1.4 polled persistence backend against a model controller: §3.1
 * registers (CAP, CC, CSTS, AQA/ASQ/ACQ, doorbell stride), §7.6.1
 * initialization and the CC.EN controller reset, §5.15 Identify,
 * §5.21.1.7 Number of Queues, §5.3/§5.4 I/O queue creation, §4.3 PRP
 * entries and lists, §4.6 completion entries and the phase tag, §6.8/§6.9/
 * §6.15 Flush, Read and Write; §7.6.2 shutdown; then broken controllers. The model asserts
 * the host-side rules; DMA uses addresses above 4 GiB on pages whose
 * physical order is the reverse of their virtual order. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/plog_nvme.c"

#define POOL 32u
#define PHYS UINT64_C(0x2345600000)
#define NSZE 4096u
static _Alignas(4096) uint8_t pool[POOL * 4096];
static uint8_t mmio[0x4000], config[256], visible[NSZE * 512], durable[NSZE * 512];
static unsigned pool_used;
static uint64_t ticks;
static uint64_t panic_at;
static bool panicking, alloc_fail;
static const struct pci_device device = {.class_code = 0x010802,
                                         .owner = PCI_OWNER_VMM,
                                         .reservation = "vmm-media",
                                         .bars = {{.size = sizeof(mmio), .wide = true}},
                                         .msi_offset = 0x50,
                                         .msix_offset = 0x70,
                                         .command = 6};

enum fault_kind { FAULT_NONE, FAULT_DROP, FAULT_CID, FAULT_SQID, FAULT_STATUS, FAULT_FATAL };
struct model_sq {
    uint64_t base;
    unsigned size, head, cq;
    bool live;
};
struct model_cq {
    uint64_t base;
    unsigned size, head, tail, phase;
    bool live;
};
static struct {
    /* Controller properties. */
    uint64_t cap, nsze, window; /* LBAs window.. window + NSZE - 1 are backed */
    uint32_t vs, lbaf[2], rtd3e;
    unsigned mdts, sqes, cqes, nlbaf, flbas, ready_delay;
    bool vwc, never_ready, fatal, stale_phase, csts_ones, never_shutdown;
    struct {
        unsigned queue, opcode, skip;
        enum fault_kind kind;
        unsigned status;
    } fault;
    /* State. */
    bool active, on, ready, queues_set, cleared_enabling, firmware_queues, shutting;
    uint64_t ready_at, shutdown_at, asq, acq;
    uint32_t aqa, cc;
    struct model_sq sq[2];
    struct model_cq cq[2];
    unsigned enables, commands, writes, reads, flushes, max_bytes, order[16], ordered;
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
    assert(size == sizeof(mmio));
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
    *pa = PHYS + (POOL - 1 - page) * 4096 + (address & 4095);
    return true;
}
/* Device view of one physical range; it never crosses a page. */
static uint8_t *model_dma(uint64_t pa, unsigned length)
{
    assert(config[4] & 4); /* Bus Master Enable */
    assert(pa >= PHYS && pa < PHYS + POOL * 4096 && (pa & 4095) + length <= 4096);
    uint64_t page = (pa - PHYS) / 4096;
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
}
static uint64_t r64(unsigned off)
{
    return r32(off) | (uint64_t)r32(off + 4) << 32;
}
static void put32(uint8_t *p, uint32_t value)
{
    memcpy(p, &value, 4);
}
static void put64(uint8_t *p, uint64_t value)
{
    memcpy(p, &value, 8);
}
/* §3.1.24/§3.1.25: SQyTDBL at 1000h + 2y * (4 << DSTRD), CQyHDBL after it. */
static unsigned model_bell(unsigned index)
{
    return 0x1000 + index * (4u << (m.cap >> 32 & 15));
}

/* §4.3: PRP1 may carry a dword-aligned offset; a transfer ending in the
 * second page puts that page in PRP2, a longer one a PRP list whose
 * entries are page aligned. */
static void model_prp(const uint32_t *c, uint8_t *data, unsigned bytes, bool to_host)
{
    uint64_t prp1 = c[6] | (uint64_t)c[7] << 32, prp2 = c[8] | (uint64_t)c[9] << 32;
    assert(!(prp1 & 3));
    unsigned n = 4096 - (unsigned)(prp1 & 4095);
    if (n > bytes)
        n = bytes;
    uint8_t *at = model_dma(prp1, n);
    if (to_host)
        memcpy(at, data, n);
    else
        memcpy(data, at, n);
    bytes -= n;
    data += n;
    if (!bytes) {
        assert(!prp2);
        return;
    }
    if (bytes <= 4096) {
        assert(!(prp2 & 4095));
        at = model_dma(prp2, bytes);
        if (to_host)
            memcpy(at, data, bytes);
        else
            memcpy(data, at, bytes);
        return;
    }
    assert(!(prp2 & 7));
    for (unsigned i = 0; bytes; ++i) {
        uint64_t entry;
        memcpy(&entry, model_dma(prp2 + i * 8, 8), 8);
        assert(!(entry & 4095));
        n = bytes < 4096 ? bytes : 4096;
        at = model_dma(entry, n);
        if (to_host)
            memcpy(at, data, n);
        else
            memcpy(data, at, n);
        bytes -= n;
        data += n;
    }
}
/* Status values are SCT << 8 | SC (§4.6.1). */
static unsigned model_admin(const uint32_t *c, uint32_t *result)
{
    unsigned opcode = c[0] & 255, qid = c[10] & 0xffff, size = (c[10] >> 16) + 1;
    uint64_t base = c[6] | (uint64_t)c[7] << 32;
    if (m.ordered < 16)
        m.order[m.ordered++] = opcode;
    if (opcode == 6) { /* §5.15 Identify */
        static uint8_t data[4096];
        memset(data, 0, sizeof(data));
        unsigned cns = c[10] & 255;
        if (cns == 1) {
            memcpy(data + 24, "MODEL NVME", 10);
            data[77] = (uint8_t)m.mdts;
            put32(data + 80, m.vs);
            put32(data + 88, m.rtd3e);
            data[512] = (uint8_t)m.sqes;
            data[513] = (uint8_t)m.cqes;
            put32(data + 516, 1);
            data[525] = m.vwc;
        } else if (cns == 0) {
            if (c[1] != 1)
                return 0x0b; /* Invalid Namespace or Format */
            put64(data, m.nsze);
            put64(data + 8, m.nsze / 2);
            put64(data + 16, m.nsze / 4);
            data[25] = (uint8_t)m.nlbaf;
            data[26] = (uint8_t)m.flbas;
            for (unsigned i = 0; i < 2; ++i)
                put32(data + 128 + i * 4, m.lbaf[i]);
        } else
            return 0x02; /* Invalid Field in Command */
        model_prp(c, data, 4096, true);
        return 0;
    }
    if (opcode == 9) { /* §5.21.1.7: before any I/O queue exists */
        if ((c[10] & 255) != 7)
            return 0x02;
        if (m.sq[1].live || m.cq[1].live)
            return 0x0c; /* Command Sequence Error */
        m.queues_set = true;
        *result = 0x003f003f;
        return 0;
    }
    if (opcode == 5) { /* §5.3 Create I/O Completion Queue */
        if (!m.queues_set || qid != 1 || m.cq[1].live)
            return 0x101; /* Invalid Queue Identifier */
        if (size < 2 || size > (m.cap & 0xffff) + 1)
            return 0x102; /* Invalid Queue Size */
        if (!(c[11] & 1) && (m.cap >> 16 & 1))
            return 0x02;
        assert(!(c[11] & 2)); /* IEN clear: completions are polled. */
        if (base & 4095)
            return 0x13; /* Invalid PRP Offset */
        m.cq[1] = (struct model_cq){base, size, 0, 0, 1, true};
        return 0;
    }
    if (opcode == 1) { /* §5.4 Create I/O Submission Queue */
        if (!m.queues_set || qid != 1 || m.sq[1].live)
            return 0x101;
        if ((c[11] >> 16) != 1 || !m.cq[1].live)
            return 0x100; /* Completion Queue Invalid */
        if (size < 2 || size > (m.cap & 0xffff) + 1)
            return 0x102;
        if (!(c[11] & 1) && (m.cap >> 16 & 1))
            return 0x02;
        if (base & 4095)
            return 0x13;
        m.sq[1] = (struct model_sq){base, size, 0, 1, true};
        return 0;
    }
    if (opcode == 0) { /* §5.6 Delete I/O Submission Queue */
        if (qid != 1 || !m.sq[1].live)
            return 0x101;
        m.sq[1].live = false;
        return 0;
    }
    if (opcode == 4) { /* §5.5 Delete I/O Completion Queue, after its submission queues */
        if (qid != 1 || !m.cq[1].live)
            return 0x101;
        if (m.sq[1].live)
            return 0x10c; /* Invalid Queue Deletion */
        m.cq[1].live = false;
        return 0;
    }
    return 0x01; /* Invalid Command Opcode */
}
static unsigned model_io(const uint32_t *c)
{
    unsigned opcode = c[0] & 255;
    if (c[1] != 1)
        return 0x0b;
    if (opcode == 0) { /* §6.8 Flush */
        ++m.flushes;
        memcpy(durable, visible, sizeof(durable));
        return 0;
    }
    if (opcode != 1 && opcode != 2)
        return 0x01;
    uint64_t lba = c[10] | (uint64_t)c[11] << 32;
    unsigned blocks = (c[12] & 0xffff) + 1, bytes = blocks * 512;
    assert(!(c[12] >> 16) && !c[13] && !c[14] && !c[15]); /* No FUA, LR, PI or hints. */
    if (lba >= m.nsze || blocks > m.nsze - lba)
        return 0x80; /* LBA Out of Range */
    assert(lba >= m.window && lba - m.window + blocks <= NSZE);
    uint64_t at = (lba - m.window) * 512;
    if (m.mdts && bytes > 4096u << m.mdts)
        return 0x02;
    if (bytes > m.max_bytes)
        m.max_bytes = bytes;
    if (opcode == 1) {
        ++m.writes;
        model_prp(c, visible + at, bytes, false);
        if (!m.vwc)
            memcpy(durable + at, visible + at, bytes);
    } else {
        ++m.reads;
        model_prp(c, visible + at, bytes, true);
    }
    return 0;
}
static void model_complete(unsigned q, uint32_t cid, uint32_t result, unsigned status,
                           enum fault_kind kind)
{
    struct model_cq *cq = &m.cq[m.sq[q].cq];
    assert((cq->tail + 1) % cq->size != cq->head); /* The host released consumed entries. */
    uint32_t phase = m.stale_phase ? 1 : cq->phase,
             entry[4] = {result, 0, m.sq[q].head | (kind == FAULT_SQID ? q ^ 1 : q) << 16,
                         (kind == FAULT_CID ? cid ^ 1 : cid) | phase << 16 | status << 17};
    memcpy(model_dma(cq->base + cq->tail * 16, 16), entry, 16);
    if (++cq->tail == cq->size) {
        cq->tail = 0;
        cq->phase ^= 1;
    }
}
static void model_doorbells(void)
{
    for (unsigned q = 0; q < 2; ++q)
        if (m.cq[q].live) {
            unsigned head = r32(model_bell(2 * q + 1));
            assert(head < m.cq[q].size);
            m.cq[q].head = head;
        }
    for (unsigned q = 0; q < 2; ++q) {
        if (!m.sq[q].live)
            continue;
        unsigned tail = r32(model_bell(2 * q));
        assert(tail < m.sq[q].size);
        assert(!m.shutting || m.sq[q].head == tail); /* No command after CC.SHN. */
        while (m.sq[q].head != tail) {
            uint32_t c[16];
            memcpy(c, model_dma(m.sq[q].base + m.sq[q].head * 64, 64), 64);
            m.sq[q].head = (m.sq[q].head + 1) % m.sq[q].size;
            assert(!(c[0] >> 8 & 0xff) &&
                   c[0] >> 16 != 0xffff); /* FUSE 0, PSDT 0 (PRPs); CID not FFFFh */
            ++m.commands;
            uint32_t result = 0;
            unsigned status = q ? model_io(c) : model_admin(c, &result);
            enum fault_kind kind = FAULT_NONE;
            if (m.fault.kind && m.fault.queue == q && m.fault.opcode == (c[0] & 255) &&
                !m.fault.skip--) {
                kind = m.fault.kind;
                m.fault.kind = FAULT_NONE;
                if (kind == FAULT_STATUS)
                    status = m.fault.status;
                if (kind == FAULT_FATAL)
                    m.fatal = true;
            }
            if (kind != FAULT_DROP && kind != FAULT_FATAL)
                model_complete(q, c[0] >> 16, result, status, kind);
        }
    }
}
static void model_tick(void)
{
    if (!m.active)
        return;
    uint32_t cc = r32(0x14);
    if ((cc & 1) && !m.on) {
        /* §3.1.5: setting EN while CSTS.RDY is 1 is undefined. */
        assert(!m.ready);
        m.on = true;
        ++m.enables;
        m.cc = cc;
        m.aqa = r32(0x24);
        m.asq = r64(0x28);
        m.acq = r64(0x30);
        assert((m.aqa & 0xfff) && (m.aqa >> 16 & 0xfff) && !(m.asq & 4095) && !(m.acq & 4095));
        assert((cc >> 4 & 7) == 0 && (cc >> 7 & 15) >= (m.cap >> 48 & 15) &&
               (cc >> 7 & 15) <= (m.cap >> 52 & 15));
        m.ready_at = m.never_ready || m.fatal ? UINT64_MAX : ticks + m.ready_delay;
    } else if (!(cc & 1) && m.on) {
        /* §7.3.2 controller reset: queues and doorbells are gone. The reset
         * write carries no shutdown request. */
        assert(!(cc >> 14 & 3));
        if (!m.ready)
            m.cleared_enabling = true;
        m.on = m.firmware_queues = m.shutting = false;
        m.ready_at = ticks + m.ready_delay;
        memset(m.sq, 0, sizeof(m.sq));
        memset(m.cq, 0, sizeof(m.cq));
        m.queues_set = false;
        memset(mmio + 0x1000, 0, sizeof(mmio) - 0x1000);
    }
    if (m.on && !m.ready && ticks >= m.ready_at) {
        /* No doorbell before CSTS.RDY. */
        assert(!r32(model_bell(0)) && !r32(model_bell(1)));
        m.ready = true;
        m.sq[0] = (struct model_sq){m.asq, (m.aqa & 0xfff) + 1, 0, 0, true};
        m.cq[0] = (struct model_cq){m.acq, (m.aqa >> 16 & 0xfff) + 1, 0, 0, 1, true};
    }
    if (!m.on && m.ready && ticks >= m.ready_at)
        m.ready = false;
    /* Queues the firmware left may still DMA until the reset. */
    assert(!m.firmware_queues || !(config[4] & 4));
    /* §7.6.2: CC.SHN 01b, CSTS.SHST 10b when the shutdown is complete. */
    if (m.on && m.ready && (cc >> 14 & 3) && !m.shutting) {
        m.shutting = true;
        m.shutdown_at = m.never_shutdown ? UINT64_MAX : ticks + 10;
    }
    unsigned shst = m.shutting && ticks >= m.shutdown_at ? 2 : 0;
    w32(0x1c,
        m.csts_ones ? UINT32_MAX : (m.ready ? 1u : 0u) | (m.fatal && m.on ? 2u : 0u) | shst << 2);
    if (!m.on || !m.ready)
        return;
    /* §3.1.8-§3.1.10: the admin queue registers change only while disabled. */
    assert(r32(0x24) == m.aqa && r64(0x28) == m.asq && r64(0x30) == m.acq &&
           (r32(0x14) & ~(3u << 14)) == (m.cc & ~(3u << 14)));
    model_doorbells();
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

static void setup(void)
{
    memset(&nv, 0, sizeof(nv));
    memset(&block, 0, sizeof(block));
    memset(&m, 0, sizeof(m));
    memset(mmio, 0, sizeof(mmio));
    memset(config, 0, sizeof(config));
    memset(visible, 0, sizeof(visible));
    memset(durable, 0, sizeof(durable));
    pool_used = 0;
    ticks = panic_at = 0;
    panicking = alloc_fail = false;
    /* MQES 63, CQR, TO 1 s, DSTRD 0, CSS NVM, MPSMIN 4 KiB, MPSMAX 64 KiB. */
    m.cap = 63 | 1u << 16 | 2u << 24 | UINT64_C(1) << 37 | UINT64_C(4) << 52;
    m.vs = 0x10400;
    m.mdts = 5;
    m.sqes = 0x66;
    m.cqes = 0x44;
    m.nsze = NSZE;
    m.lbaf[0] = 9u << 16;
    m.vwc = true;
    m.ready_delay = 20;
    m.active = true;
    config[0x50] = 5;
    config[0x52] = 1; /* MSI, enabled */
    config[0x70] = 0x11;
    config[0x73] = 0x80; /* MSI-X, enabled */
    config[4] = 6;
}
static void program(void)
{
    w32(0, (uint32_t)m.cap);
    w32(4, (uint32_t)(m.cap >> 32));
    w32(8, m.vs);
}
static const struct plog_block *begin(void)
{
    program();
    return amd64_plog_nvme(&device, true);
}
static void pattern(uint8_t *p, unsigned bytes, unsigned seed)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = (uint8_t)(i * 7 + seed * 13 + 1);
}
static bool disabled(void)
{
    return !(r32(0x14) & 1) && !m.on && !(r32(0x1c) & 1);
}

int main(void)
{
    static uint8_t data[64 * 512], back[64 * 512];
    /* §7.6.1 from a disabled controller. */
    setup();
    const struct plog_block *b = begin();
    assert(b && b->sectors == NSZE && m.on && m.ready && m.enables == 1);
    assert((m.cc >> 16 & 15) == 6 && (m.cc >> 20 & 15) == 4); /* 64-byte SQ, 16-byte CQ entries */
    assert((m.cc >> 7 & 15) == 0 && (m.cc >> 11 & 7) == 0 && (m.cc >> 14 & 3) == 0);
    assert(m.sq[1].live && m.cq[1].live && m.order[0] == 6);
    assert(m.asq >= PHYS && m.acq >= PHYS && m.sq[1].base >= PHYS && m.cq[1].base >= PHYS);
    /* Polling only: INTx disabled, MSI and MSI-X off; bus master and memory on. */
    assert((config[4] | config[5] << 8) == 0x406 && !(config[0x52] & 1) && !(config[0x73] & 0x80));
    /* Write, Flush, Read through PRP1, PRP2 and PRP lists; the queues wrap. */
    const unsigned counts[] = {1, 7, 8, 9, 16, 17, 31, 32, 33, 40, 64};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        unsigned n = counts[i], lba = 100 + i * 70;
        pattern(data, n * 512, i);
        assert(b->write(lba, n, data) && !memcmp(visible + lba * 512, data, n * 512));
        assert(memcmp(durable + lba * 512, data, n * 512)); /* still in the volatile write cache */
        unsigned flushes = m.flushes;
        assert(b->flush() && m.flushes == flushes + 1 &&
               !memcmp(durable + lba * 512, data, n * 512));
        memset(back, 0, sizeof(back));
        assert(b->read(lba, n, back) && !memcmp(back, data, n * 512));
    }
    assert(m.max_bytes && m.max_bytes <= 16384);
    for (unsigned i = 0; i < 100; ++i) {
        pattern(data, 512, 50 + i);
        assert(b->write(i, 1, data) && b->read(i, 1, back) && !memcmp(back, data, 512));
    }
    /* Range checks need no command. */
    unsigned commands = m.commands;
    assert(!b->write(NSZE, 1, data) && !b->write(NSZE - 1, 2, data) &&
           !b->read(UINT64_MAX, 1, back));
    assert(m.commands == commands && b->write(NSZE - 1, 1, data));
    /* §4.2: the command identifier wraps without FFFFh. */
    for (unsigned i = 0; i < 65540; ++i)
        assert(b->read(i % 64, 1, back));

    /* MDTS 1: at most 8 KiB per command. */
    setup();
    m.mdts = 1;
    b = begin();
    assert(b);
    pattern(data, 32 * 512, 3);
    commands = m.commands;
    assert(b->write(10, 32, data) && m.commands == commands + 2 && m.max_bytes == 8192);
    assert(b->read(10, 32, back) && !memcmp(back, data, 32 * 512));
    /* MQES 1: two-entry queues wrap at every second command. */
    setup();
    m.cap = (m.cap & ~UINT64_C(0xffff)) | 1;
    b = begin();
    assert(b && (m.aqa & 0xfff) == 1 && m.sq[1].size == 2 && m.cq[1].size == 2);
    for (unsigned i = 0; i < 9; ++i) {
        pattern(data, 1024, i);
        assert(b->write(20 + i * 2, 2, data) && b->flush() && b->read(20 + i * 2, 2, back) &&
               !memcmp(back, data, 1024));
    }
    /* DSTRD 2: 16-byte doorbell stride. */
    setup();
    m.cap |= UINT64_C(2) << 32;
    b = begin();
    assert(b);
    pattern(data, 4096, 9);
    assert(b->write(5, 8, data) && b->read(5, 8, back) && !memcmp(back, data, 4096));
    /* Without a volatile write cache Flush still completes (§6.8). */
    setup();
    m.vwc = false;
    b = begin();
    assert(b && b->write(0, 1, data) && b->flush());
    /* FLBAS selects the format in use: format 1 has 512-byte LBAs. */
    setup();
    m.nlbaf = 1;
    m.flbas = 1;
    m.lbaf[0] = 12u << 16;
    m.lbaf[1] = 9u << 16;
    assert(begin());
    /* NSZE beyond 2^32 LBAs: the upper SLBA dword in CDW11. */
    setup();
    m.nsze = UINT64_C(1) << 33;
    m.window = (UINT64_C(1) << 32) - 8;
    b = begin();
    assert(b && b->sectors == UINT64_C(1) << 33);
    pattern(data, 40 * 512, 21);
    assert(b->write(m.window, 40, data) && !memcmp(visible, data, 40 * 512));
    assert(b->read(m.window, 40, back) && !memcmp(back, data, 40 * 512));
    pattern(data, 512, 22);
    assert(b->write((UINT64_C(1) << 32) + 100, 1, data) && !memcmp(visible + 108 * 512, data, 512));
    /* CAP.TO 0 still allows one 500 ms unit. */
    setup();
    m.cap &= ~(UINT64_C(255) << 24);
    m.ready_delay = 300;
    assert(begin());

    /* Enabled by the host firmware with its own queues: no bus mastering
     * until the reset, which also clears a shutdown notification left in CC. */
    setup();
    w32(0x14, 1 | 6u << 16 | 4u << 20);
    w32(0x24, 0x00070007);
    w32(0x28, 0x7f000000);
    w32(0x30, 0x7f001000);
    m.on = m.ready = m.firmware_queues = true;
    m.cc = r32(0x14);
    m.aqa = r32(0x24);
    m.asq = r64(0x28);
    m.acq = r64(0x30);
    assert(begin() && m.enables == 1 && m.asq >= PHYS);
    setup();
    w32(0x14, 1 | 1u << 14 | 6u << 16 | 4u << 20);
    w32(0x24, 0x00070007);
    w32(0x28, 0x7f000000);
    w32(0x30, 0x7f001000);
    m.on = m.ready = m.firmware_queues = true;
    m.cc = r32(0x14);
    m.aqa = r32(0x24);
    m.asq = r64(0x28);
    m.acq = r64(0x30);
    assert(begin() && m.enables == 1 && !(m.cc >> 14 & 3) && !(r32(0x1c) >> 2 & 3));
    /* Still becoming ready: wait for CSTS.RDY before clearing CC.EN. */
    setup();
    w32(0x14, 1 | 6u << 16 | 4u << 20);
    w32(0x24, 0x00070007);
    w32(0x28, 0x7f000000);
    w32(0x30, 0x7f001000);
    m.on = true;
    m.ready_at = 50;
    m.cc = r32(0x14);
    m.aqa = r32(0x24);
    m.asq = r64(0x28);
    m.acq = r64(0x30);
    assert(begin() && !m.cleared_enabling && ticks >= 50);

    /* Broken controllers: bounded by CAP.TO (1 s here), then disabled. */
    setup();
    m.never_ready = true;
    assert(!begin() && ticks < 4000 && disabled());
    setup();
    m.fatal = true;
    assert(!begin() && ticks < 1000 && disabled());
    setup();
    memset(mmio, 0xff, 0x40);
    m.active = false;
    assert(!amd64_plog_nvme(&device, true) && ticks < 100);
    setup();
    m.cap &= ~(UINT64_C(1) << 37);
    assert(!begin() && !m.enables);
    setup();
    m.cap |= UINT64_C(1) << 48;
    assert(!begin() && !m.enables);
    setup();
    m.cap &= ~UINT64_C(0xffff);
    assert(!begin() && !m.enables);
    setup();
    alloc_fail = true;
    assert(!begin() && !m.enables);
    setup();
    m.vs = 0x100;
    assert(!begin() && !m.enables);
    setup();
    m.cap |= UINT64_C(15) << 32; /* doorbells beyond BAR0 */
    assert(!begin() && !m.enables);
    static const struct pci_device small = {.class_code = 0x010802,
                                            .owner = PCI_OWNER_VMM,
                                            .reservation = "vmm-media",
                                            .bars = {{.size = 0x800, .wide = true}}};
    setup();
    assert(!amd64_plog_nvme(&small, true));
    setup();
    m.csts_ones = true;
    w32(0x1c, UINT32_MAX);
    assert(!begin() && ticks < 100 && !m.enables);
    setup();
    m.nlbaf = 0;
    m.flbas = 1;
    m.lbaf[0] = 12u << 16;
    m.lbaf[1] = 9u << 16;
    assert(!begin() && disabled());
    setup();
    m.sqes = 0x55;
    assert(!begin() && disabled());
    setup();
    m.cqes = 0x33;
    assert(!begin() && disabled());
    setup();
    m.sqes = 0x77;
    assert(!begin() && disabled());
    setup();
    m.cqes = 0x55;
    assert(!begin() && disabled());
    setup();
    m.lbaf[0] = 12u << 16;
    assert(!begin() && disabled());
    setup();
    m.lbaf[0] = 9u << 16 | 8;
    assert(!begin() && disabled());
    setup();
    m.nsze = 0;
    assert(!begin() && disabled());
    setup();
    m.fault.queue = 0;
    m.fault.opcode = 5;
    m.fault.kind = FAULT_STATUS;
    m.fault.status = 0x101;
    assert(!begin() && disabled());
    setup();
    m.fault.queue = 0;
    m.fault.opcode = 6;
    m.fault.kind = FAULT_DROP;
    assert(!begin() && ticks >= 1000 && ticks < 120000 && disabled());

    /* A lost completion: bounded wait, the backend stops, the controller is reset. */
    setup();
    b = begin();
    assert(b);
    m.fault.queue = 1;
    m.fault.opcode = 1;
    m.fault.skip = 3;
    m.fault.kind = FAULT_DROP;
    for (unsigned i = 0; i < 3; ++i)
        assert(b->write(i, 1, data));
    uint64_t at = ticks;
    assert(!b->write(3, 1, data) && ticks - at >= 1000 && ticks - at < 120000 && disabled());
    commands = m.commands;
    at = ticks;
    assert(!b->read(0, 1, back) && !b->write(0, 1, data) && !b->flush() && m.commands == commands &&
           ticks == at);
    /* A completion for another command or queue stops the backend. */
    setup();
    b = begin();
    m.fault.queue = 1;
    m.fault.opcode = 1;
    m.fault.kind = FAULT_CID;
    assert(b && !b->write(0, 1, data) && disabled() && !b->read(0, 1, back));
    setup();
    b = begin();
    m.fault.queue = 1;
    m.fault.opcode = 2;
    m.fault.kind = FAULT_SQID;
    assert(b && !b->read(0, 1, back) && disabled());
    /* An error status fails that command only. */
    setup();
    b = begin();
    m.fault.queue = 1;
    m.fault.opcode = 1;
    m.fault.kind = FAULT_STATUS;
    m.fault.status = 0x06;
    assert(b && !b->write(0, 1, data) && b->write(0, 1, data) && b->flush());
    setup();
    b = begin();
    m.fault.queue = 1;
    m.fault.opcode = 0;
    m.fault.kind = FAULT_STATUS;
    m.fault.status = 0x06;
    assert(b && b->write(0, 1, data) && !b->flush());
    setup();
    b = begin();
    m.fault.queue = 1;
    m.fault.opcode = 2;
    m.fault.kind = FAULT_STATUS;
    m.fault.status = 0x281;
    assert(b && !b->read(0, 1, back) && b->read(0, 1, back));
    /* A controller that never inverts the phase tag: the host must not take
     * the previous pass's entries as new completions. */
    setup();
    m.stale_phase = true;
    b = begin();
    assert(b);
    for (unsigned i = 0; i < m.cq[1].size - 1; ++i)
        assert(b->write(i, 1, data));
    assert(b->write(0, 1, data)); /* last entry of the first pass */
    at = ticks;
    assert(!b->write(1, 1, data) && ticks - at >= 1000 && disabled());
    /* SCT alone makes an error. */
    setup();
    b = begin();
    m.fault.queue = 1;
    m.fault.opcode = 1;
    m.fault.kind = FAULT_STATUS;
    m.fault.status = 0x100;
    assert(b && !b->write(0, 1, data) && b->write(0, 1, data));
    /* CSTS.CFS during a command ends the wait at once (§3.1.6). */
    setup();
    b = begin();
    m.fault.queue = 1;
    m.fault.opcode = 1;
    m.fault.kind = FAULT_FATAL;
    assert(b);
    at = ticks;
    assert(!b->write(0, 1, data) && ticks - at < 1000 && disabled());
    /* A controller that is gone: all-ones registers. */
    setup();
    b = begin();
    assert(b);
    m.active = false;
    memset(mmio, 0xff, 0x40);
    at = ticks;
    assert(!b->write(0, 1, data) && ticks - at < 100);
    /* A panic during the wait: return at once, without the controller reset. */
    setup();
    b = begin();
    m.fault.queue = 1;
    m.fault.opcode = 1;
    m.fault.kind = FAULT_DROP;
    assert(b);
    panic_at = ticks + 50;
    at = ticks;
    assert(!b->write(0, 1, data) && ticks - at < 1000 && (r32(0x14) & 1));

    /* After a VMM panic no command reaches the controller. */
    setup();
    b = begin();
    assert(b);
    panicking = true;
    commands = m.commands;
    unsigned tail = r32(model_bell(2));
    assert(!b->write(0, 1, data) && !b->read(0, 1, back) && !b->flush());
    assert(m.commands == commands && r32(model_bell(2)) == tail);

    /* §7.6.2 shutdown: the I/O queues deleted (submission queue first),
     * CC.SHN 01b, CSTS.SHST 10b; the controller stays enabled, unused. */
    setup();
    b = begin();
    assert(b && b->write(0, 1, data) && b->flush());
    commands = m.commands;
    amd64_plog_nvme_shutdown();
    assert(m.commands == commands + 2 && !m.sq[1].live && !m.cq[1].live);
    assert((r32(0x14) >> 14 & 3) == 1 && (r32(0x14) & 1) && (r32(0x1c) >> 2 & 3) == 2);
    at = ticks;
    assert(!b->write(0, 1, data) && !b->read(0, 1, back) && !b->flush());
    amd64_plog_nvme_shutdown();
    assert(m.commands == commands + 2 && ticks == at);
    /* Bounded by RTD3E when reported, else 5 s. */
    setup();
    m.never_shutdown = true;
    m.rtd3e = 2000000;
    b = begin();
    assert(b);
    at = ticks;
    amd64_plog_nvme_shutdown();
    assert(ticks - at >= 2000 && ticks - at < 2100);
    setup();
    m.never_shutdown = true;
    b = begin();
    assert(b);
    at = ticks;
    amd64_plog_nvme_shutdown();
    assert(ticks - at >= 5000 && ticks - at < 5100);
    /* Not after a panic, nor after the backend stopped. */
    setup();
    b = begin();
    assert(b);
    panicking = true;
    amd64_plog_nvme_shutdown();
    assert(!(r32(0x14) >> 14 & 3) && m.sq[1].live);
    setup();
    b = begin();
    m.fault.queue = 1;
    m.fault.opcode = 1;
    m.fault.kind = FAULT_CID;
    assert(b && !b->write(0, 1, data));
    commands = m.commands;
    amd64_plog_nvme_shutdown();
    assert(m.commands == commands && !(r32(0x14) & 1) && !(r32(0x14) >> 14 & 3));
    puts("PLOG NVMe: passed");
}
