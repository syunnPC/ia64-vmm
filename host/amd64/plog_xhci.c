/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/User/Usb/Xhci.cpp
 */
/* Single-root-device polling subset; Apache-2.0 text: LICENSES/Apache-2.0.txt.
 * xHCI 1.2 §4.2, §4.3, §4.6, §4.9 and §6.4. DMA stays in VMM-owned RAM. */
#include "host/amd64/amd64.h"
#include "host/amd64/plog_xhci.h"
#include "persist/protocol.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#if !defined(IA64VMM_AMD64_HOST_ONLY)
static struct {
    uint8_t *mmio, *input, *output, *buffer;
    uint64_t buffer_pa, input_pa, event_pa;
    struct xhci_ring command, control, bulk[2];
    struct xhci_trb *events;
    unsigned op, runtime, bells, context_size, slot, port, speed;
    unsigned event_next, event_cycle, endpoint[2], address[2], interface;
    bool live, running, ac64, usb3[256];
} x;
static const char *who = "PLOG";
static bool quiet;
static struct plog_block block;
static uint32_t reg(unsigned off)
{
    return *(volatile uint32_t *)(x.mmio + off);
}
static void wr(unsigned off, uint32_t value)
{
    *(volatile uint32_t *)(x.mmio + off) = value;
}
static void wr64(unsigned off, uint64_t value)
{
    wr(off, (uint32_t)value);
    wr(off + 4, (uint32_t)(value >> 32));
}
static uint64_t physical(const void *p)
{
    uint64_t pa;
    if (!amd64_memory_physical((uintptr_t)p, &pa))
        hal_panic("PLOG DMA pointer unmapped");
    return pa;
}
static void pause_ms(unsigned ms)
{
    uint64_t end = hal_counter() + hal_counter_hz() / 1000 * ms;
    while (hal_counter() < end)
        __asm__ volatile("pause");
}
static bool wait_reg(unsigned off, uint32_t mask, uint32_t value)
{
    uint64_t end = hal_counter() + hal_counter_hz() * 3;
    do {
        uint32_t n = reg(off);
        if (n == UINT32_MAX)
            return false;
        if ((n & mask) == value)
            return true;
        __asm__ volatile("pause");
    } while (hal_counter() < end);
    LOGE("%s xHCI register timeout off=%x value=%x", who, off, reg(off));
    return false;
}
/* AC64 applies to every byte of every DMA allocation, including controller
 * structures and scratchpads, before any DMA pointer is programmed. */
static void *dma_pages(unsigned pages)
{
    void *p = hal_alloc_pages(pages, HAL_PAGE_ZERO);
    if (!p)
        return NULL;
    uint64_t pa = physical(p), bytes = (uint64_t)pages * 4096;
    if (!bytes || pa > UINT64_MAX - (bytes - 1) ||
        (!x.ac64 && (pa > UINT32_MAX || bytes - 1 > UINT32_MAX - pa))) {
        LOGE("%s xHCI DMA range unsupported; low-memory DMA allocation unavailable", who);
        return NULL;
    }
    return p;
}
static bool dma_ring(struct xhci_ring *r)
{
    r->trbs = dma_pages(1);
    if (!r->trbs)
        return false;
    r->physical = physical(r->trbs);
    r->cycle = 1;
    return true;
}
static int event(unsigned type, uint64_t pointer, unsigned endpoint, unsigned length,
                 bool enable_slot)
{
    uint64_t end = hal_counter() + hal_counter_hz() * 3;
    do {
        if (log_in_panic()) {
            x.live = false;
            return -1;
        }
        struct xhci_trb *at = &x.events[x.event_next];
        uint32_t control = __atomic_load_n(&at->control, __ATOMIC_ACQUIRE);
        if ((control & 1) != x.event_cycle) {
            __asm__ volatile("pause");
            continue;
        }
        struct xhci_trb e = {at->parameter, at->status, control};
        if (++x.event_next == 256) {
            x.event_next = 0;
            x.event_cycle ^= 1;
        }
        wr64(x.runtime + 0x38, x.event_pa + x.event_next * 16 + 8);
        if ((control >> 10 & 63) == 34)
            continue; /* Port status change. */
        unsigned slot = enable_slot ? control >> 24 : x.slot;
        int result = xhci_completion(&e, type, pointer, slot, endpoint, length);
        if (result >= 0 && enable_slot)
            x.slot = slot;
        if (result < 0)
            LOGE("%s xHCI completion type=%u code=%u slot=%u ep=%u ptr=%llx expected=%llx", who,
                 control >> 10 & 63, e.status >> 24, control >> 24, control >> 16 & 31,
                 (unsigned long long)e.parameter, (unsigned long long)pointer);
        return result;
    } while (hal_counter() < end);
    x.live = false; /* Timed-out DMA storage is retained; never reused. */
    LOGE("%s xHCI event timeout type=%u ep=%u", who, type, endpoint);
    return -1;
}
static bool command(unsigned type, uint64_t parameter, uint32_t flags)
{
    if (log_in_panic())
        return false;
    xhci_begin(&x.command);
    uint64_t pointer = xhci_push(&x.command, parameter, 0, type << 10 | x.slot << 24 | flags);
    xhci_commit(&x.command);
    __asm__ volatile("mfence" ::: "memory");
    wr(x.bells, 0);
    return event(33, pointer, 0, 0, type == 9) >= 0;
}
static int control(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                   unsigned length)
{
    if (log_in_panic())
        return -1;
    uint64_t setup = request_type | (uint64_t)request << 8 | (uint64_t)value << 16 |
                     (uint64_t)index << 32 | (uint64_t)length << 48;
    bool in = (request_type & 0x80) != 0;
    xhci_begin(&x.control);
    xhci_push(&x.control, setup, 8, 2u << 10 | 1u << 6 | (length ? (in ? 3u : 2u) << 16 : 0));
    uint64_t data_pointer = 0;
    if (length)
        data_pointer = xhci_push(&x.control, x.buffer_pa, length,
                                 3u << 10 | 1u << 5 | 1u << 2 | (in ? 1u << 16 : 0));
    uint64_t pointer =
        xhci_push(&x.control, 0, 0, 4u << 10 | 1u << 5 | (!length || !in ? 1u << 16 : 0));
    xhci_commit(&x.control);
    __asm__ volatile("mfence" ::: "memory");
    wr(x.bells + x.slot * 4, 1);
    /* Data and Status are separate TDs (§4.11.2.2). ISP/IOC on the
     * Data Stage reports its residue; Status has no payload length. */
    int actual = length ? event(32, data_pointer, 1, length, false) : 0;
    if (actual < 0)
        return actual;
    int rc = event(32, pointer, 1, 0, false);
    return rc < 0 ? rc : actual;
}
static int bulk(unsigned which, unsigned length)
{
    if (log_in_panic())
        return -1;
    xhci_begin(&x.bulk[which]);
    uint64_t pointer = xhci_push(&x.bulk[which], x.buffer_pa, length, 1u << 10 | 1u << 5 | 1u << 2);
    xhci_commit(&x.bulk[which]);
    __asm__ volatile("mfence" ::: "memory");
    wr(x.bells + x.slot * 4, x.endpoint[which]);
    return event(32, pointer, x.endpoint[which], length, false);
}
static bool clear_halt(unsigned which)
{
    unsigned dci = x.endpoint[which];
    uint32_t *out = (uint32_t *)(x.output + dci * x.context_size);
    unsigned state = __atomic_load_n(out, __ATOMIC_ACQUIRE) & 7;
    struct xhci_ring *r = &x.bulk[which];
    uint64_t dequeue = r->physical + r->next * 16 + r->cycle;
    /* xHCI §4.6.8: Reset Endpoint is legal only in Halted. Linux's
     * stalled-TD cleanup resets the host before USB CLEAR_FEATURE. */
    if (state == 2) {
        if (!command(14, 0, dci << 16))
            return false;
    } else if (state == 1 || state == 3) {
        /* §4.6.8 additional note: reconfigure a non-halted pipe to reset
         * its sequence/toggle state; Reset Endpoint would be illegal. */
        memset(x.input, 0, 4096);
        uint32_t *icc = (uint32_t *)x.input;
        icc[0] = 1u << dci;
        icc[1] = 1 | 1u << dci;
        memcpy(x.input + x.context_size, x.output, x.context_size);
        memcpy(x.input + (dci + 1) * x.context_size, out, x.context_size);
        uint32_t *ep = (uint32_t *)(x.input + (dci + 1) * x.context_size);
        ep[0] &= ~7u;
        ep[2] = (uint32_t)dequeue;
        ep[3] = dequeue >> 32;
        if (!command(12, x.input_pa, 0))
            return false;
    } else
        return false;
    if (control(2, 1, 0, x.address[which], 0) != 0)
        return false;
    return state != 2 || command(16, dequeue, dci << 16);
}
static bool reset_bot(void)
{
    bool ok =
        x.live && control(0x21, 0xff, 0, x.interface, 0) >= 0 && clear_halt(0) && clear_halt(1);
    if (!ok)
        x.live = false;
    return ok;
}
static int bot_transfer(bool in, void *data, unsigned length)
{
    if (!x.live || log_in_panic() || length > 16384)
        return -1;
    if (!in)
        memcpy(x.buffer, data, length);
    int n = bulk(in ? 0 : 1, length);
    if (in && n > 0)
        memcpy(data, x.buffer, (unsigned)n);
    return n;
}
static const struct bot_io bot = {bot_transfer, clear_halt, reset_bot};
static struct scsi_disk disk = {.io = &bot, .wait = pause_ms};
static bool read_blocks(uint64_t lba, unsigned count, void *data)
{
    return scsi_disk_read(&disk, lba, count, data);
}
static bool write_blocks(uint64_t lba, unsigned count, const void *data)
{
    return scsi_disk_write(&disk, lba, count, data);
}
static bool flush(void)
{
    return x.live && !log_in_panic() && scsi_disk_flush(&disk);
}
static void endpoint_context(unsigned dci, unsigned type, unsigned packet, unsigned burst,
                             uint64_t ring)
{
    uint32_t *e = (uint32_t *)(x.input + (dci + 1) * x.context_size);
    e[1] = 3u << 1 | type << 3 | burst << 8 | packet << 16;
    e[2] = (uint32_t)ring | 1;
    e[3] = ring >> 32;
    e[4] = type == 4 ? 8 : 0;
}
static bool enumerate(unsigned slot_type)
{
    if (!command(9, 0, slot_type << 16) || x.slot != 1)
        return false;
    memset(x.input, 0, 4096);
    uint32_t *icc = (uint32_t *)x.input, *slot = (uint32_t *)(x.input + x.context_size);
    icc[1] = 3;
    slot[0] = x.speed << 20 | 1u << 27;
    slot[1] = x.port << 16;
    unsigned mps = x.speed == 4 ? 512 : x.speed == 3 ? 64 : 8;
    endpoint_context(1, 4, mps, 0, x.control.physical);
    if (!command(11, x.input_pa, 0) || control(0x80, 6, 0x100, 0, 8) != 8)
        return false;
    unsigned mps0 = x.buffer[7];
    if (x.speed == 4) {
        if (mps0 != 9)
            return false;
        mps0 = 512;
    } else if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64)
        return false;
    if (mps0 != mps) {
        memset(x.input, 0, 4096);
        ((uint32_t *)x.input)[1] = 2;
        memcpy(x.input + 2 * x.context_size, x.output + x.context_size, x.context_size);
        uint32_t *ep = (uint32_t *)(x.input + 2 * x.context_size);
        ep[1] = (ep[1] & 0xffff) | mps0 << 16;
        if (!command(13, x.input_pa, 0))
            return false;
    }
    if (control(0x80, 6, 0x100, 0, 18) != 18 || x.buffer[0] != 18 || x.buffer[1] != 1)
        return false;
    if (x.buffer[4] == 9) {
        LOGE("%s USB hubs unsupported; use a root port", who);
        return false;
    }
    if (control(0x80, 6, 0x200, 0, 9) != 9)
        return false;
    unsigned total = x.buffer[2] | x.buffer[3] << 8;
    struct usb_bot u;
    if (total < 9 || total > 4096 || control(0x80, 6, 0x200, 0, total) != (int)total ||
        !usb_bot_interface(x.buffer, total, &u))
        return false;
    x.interface = u.interface;
    for (unsigned i = 0; i < 2; ++i) {
        x.address[i] = u.address[i];
        x.endpoint[i] = (u.address[i] & 15) * 2 + (i == 0);
    }
    memset(x.input, 0, 4096);
    icc[1] = 1 | 1u << x.endpoint[0] | 1u << x.endpoint[1];
    memcpy(slot, x.output, x.context_size);
    unsigned last = x.endpoint[0] > x.endpoint[1] ? x.endpoint[0] : x.endpoint[1];
    slot[0] = (slot[0] & 0x07ffffff) | last << 27;
    for (unsigned i = 0; i < 2; ++i)
        endpoint_context(x.endpoint[i], i ? 2 : 6, u.mps[i], u.burst[i], x.bulk[i].physical);
    return command(12, x.input_pa, 0) && control(0, 9, u.configuration, 0, 0) == 0;
}
static bool root_ports(unsigned ports, bool ppc)
{
    if (ppc)
        for (unsigned p = 1; p <= ports; ++p) {
            unsigned off = x.op + 0x400 + (p - 1) * 16;
            wr(off, (reg(off) & 0x0e00c200) | 0x200);
        }
    /* §4.3.1/§5.4.8: allow port power stabilization and USB3 automatic
     * link training. USB2 only requires CCS before its explicit reset. */
    uint64_t end = hal_counter() + hal_counter_hz();
    do {
        unsigned connected = 0;
        bool training = false;
        for (unsigned p = 1; p <= ports; ++p) {
            uint32_t status = reg(x.op + 0x400 + (p - 1) * 16);
            if (status == UINT32_MAX)
                return false;
            if (!(status & 1))
                continue;
            if (connected) {
                LOGE("%s requires one directly attached device", who);
                return false;
            }
            connected = p;
            if ((x.usb3[p] || (status >> 10 & 15) >= 4) && (!(status & 2) || (status & 0x1e0)))
                training = true;
        }
        if (connected && !training) {
            x.port = connected;
            return true;
        }
        pause_ms(1);
    } while (hal_counter() < end);
    if (quiet)
        LOGI("%s: no stick on the reserved controller", who);
    else
        LOGE("%s no trained root-port stick attached", who);
    return false;
}
static const struct plog_block *start(void);
/* log: media_log=1 asked for the stick; otherwise only the EFI variable
 * slots use it, and a missing stick or controller is not an error. */
const struct plog_block *amd64_plog_xhci(bool log)
{
    who = log ? "PLOG" : "NVRAM stick";
    quiet = !log;
    const struct plog_block *b = start();
    /* Failed after Run/Stop: halt the controller; its DMA pages stay. */
    if (!b && x.running) {
        wr(x.op, reg(x.op) & ~1u);
        (void)wait_reg(x.op + 4, 1, 1);
        x.running = x.live = false;
        LOGI("%s xHCI halted after the failed start", who);
    }
    return b;
}
static const struct plog_block *start(void)
{
    const struct pci_device *d = NULL;
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *p = platform_pci_device(i);
        if (p->owner == PCI_OWNER_VMM && p->reservation && !strcmp(p->reservation, "vmm-media"))
            d = p;
    }
    if (!d || d->class_code != 0x0c0330 || d->bars[0].io || d->bars[0].size < 0x1000) {
        unsigned class_code = d ? d->class_code : 0;
        if (quiet)
            LOGI(
                "%s: the reserved vmm_media controller (class %06x) is not NVMe, AHCI, EHCI or xHCI",
                who, class_code);
        else
            LOGE(
                "%s requires the reserved vmm_media controller (class %06x) to be NVMe, AHCI, EHCI or xHCI",
                who, class_code);
        return NULL;
    }
    x.mmio = platform_pci_map_physical(d->bars[0].base, d->bars[0].size);
    if (!x.mmio)
        return NULL;
    platform_pci_host_write(d->bdf, 4, d->command | 6, 2);
    uint32_t hcc = reg(0x10), hcs = reg(4), hcs2 = reg(8);
    uint32_t cap0 = reg(0);
    unsigned version = cap0 >> 16;
    if (cap0 == UINT32_MAX || hcc == UINT32_MAX || hcs == UINT32_MAX ||
        (version != 0x96 && (version < 0x100 || version > 0x120)))
        return NULL;
    x.op = cap0 & 255;
    x.runtime = reg(0x18) & ~31u;
    x.bells = reg(0x14) & ~3u;
    unsigned ports = hcs >> 24;
    x.context_size = hcc & 4 ? 64 : 32;
    x.ac64 = (hcc & 1) != 0;
    if (x.op < 32 || (x.op & 3) || x.op + 0x400 + ports * 16 > d->bars[0].size ||
        x.runtime > d->bars[0].size - 0x40 || x.bells > d->bars[0].size - 8 || !ports ||
        !(hcs & 255))
        return NULL;
    unsigned ext = hcc >> 16 << 2, slot_type[256] = {0};
    for (unsigned n = 0; ext && n < 64; ++n) {
        if (ext + 4 > d->bars[0].size)
            return NULL;
        uint32_t cap = reg(ext);
        if ((cap & 255) == 1) {
            if (ext + 8 > d->bars[0].size)
                return NULL;
            wr(ext, cap | 1u << 24);
            if (!wait_reg(ext, 1u << 16, 0))
                return NULL;
            /* xHCI §7.1.2: preserve RsvdP, clear SMI enables, acknowledge
             * only the three RW1C ownership/configuration status bits. */
            wr(ext + 4, (reg(ext + 4) & 0x000e1feeu) | 0xe0000000u);
        }
        if ((cap & 255) == 2) {
            if (ext + 16 > d->bars[0].size)
                return NULL;
            unsigned start = reg(ext + 8) & 255, count = reg(ext + 8) >> 8 & 255;
            if (!start || start + count > ports + 1)
                return NULL;
            for (unsigned p = start; p < start + count; ++p) {
                slot_type[p] = version >= 0x100 ? reg(ext + 12) & 31 : 0;
                x.usb3[p] = (cap >> 24) >= 3;
            }
        }
        unsigned next = cap >> 8 & 255;
        if (!next) {
            ext = 0;
            break;
        }
        ext += next * 4;
    }
    if (ext)
        return NULL;
    if (!wait_reg(x.op + 4, 0x800, 0))
        return NULL;
    wr(x.op, reg(x.op) & ~5u);
    if (!wait_reg(x.op + 4, 1, 1))
        return NULL;
    wr(x.op, 2);
    if (!wait_reg(x.op, 2, 0) || !wait_reg(x.op + 4, 0x800, 0) || !(reg(x.op + 8) & 1))
        return NULL;
    if (!dma_ring(&x.command) || !dma_ring(&x.control) || !dma_ring(&x.bulk[0]) ||
        !dma_ring(&x.bulk[1]))
        return NULL;
    x.input = dma_pages(1);
    x.output = dma_pages(1);
    x.events = dma_pages(1);
    uint64_t *dcbaa = dma_pages(1), *erst = dma_pages(1);
    uint8_t *buffer = dma_pages(32);
    if (!x.input || !x.output || !x.events || !dcbaa || !erst || !buffer)
        return NULL;
    x.buffer = buffer + ((65536 - (physical(buffer) & 65535)) & 65535);
    x.buffer_pa = physical(x.buffer);
    x.input_pa = physical(x.input);
    x.event_pa = physical(x.events);
    unsigned scratchpads = (hcs2 >> 21 & 31) << 5 | (hcs2 >> 27 & 31);
    if (scratchpads) {
        uint64_t *array = dma_pages((scratchpads * 8 + 4095) / 4096);
        uint8_t *scratch = dma_pages(scratchpads);
        if (!array || !scratch)
            return NULL;
        dcbaa[0] = physical(array);
        for (unsigned i = 0; i < scratchpads; ++i)
            array[i] = physical(scratch + i * 4096);
    }
    dcbaa[1] = physical(x.output);
    erst[0] = x.event_pa;
    erst[1] = 256;
    x.event_cycle = 1;
    wr64(x.op + 0x30, physical(dcbaa));
    wr64(x.op + 0x18, x.command.physical | 1);
    wr(x.op + 0x38, 1);
    wr(x.runtime + 0x28, 1);
    wr64(x.runtime + 0x30, physical(erst));
    wr64(x.runtime + 0x38, x.event_pa | 8);
    wr(x.runtime + 0x20, 1);
    wr(x.op, 1);
    x.running = true;
    if (!wait_reg(x.op + 4, 1, 0))
        return NULL;
    x.live = true;
    if (!root_ports(ports, (hcc & 8) != 0))
        return NULL;
    unsigned off = x.op + 0x400 + (x.port - 1) * 16;
    pause_ms(100);
    uint32_t status = reg(off);
    if ((status >> 10 & 15) < 4)
        wr(off, (status & 0x0e00c200) | 1u << 4); /* PR; never echo PED or RW1C. */
    if (!wait_reg(off, 1u << 4, 0) || !wait_reg(off, 3, 3))
        return NULL;
    pause_ms(10);
    x.speed = reg(off) >> 10 & 15;
    if (x.speed != 1 && x.speed != 3 && x.speed != 4) {
        LOGE("%s unsupported USB speed ID=%u", who, x.speed);
        return NULL;
    }
    if (!enumerate(slot_type[x.port])) {
        LOGE("%s USB BOT enumeration failed", who);
        return NULL;
    }
    disk.who = who;
    if (!scsi_disk_start(&disk))
        return NULL;
    block = (struct plog_block){disk.sectors, read_blocks, write_blocks, flush};
    LOGI("%s xHCI BOT root-port=%u speed=%u slot=%u sectors=%llu", who, x.port, x.speed, x.slot,
         (unsigned long long)block.sectors);
    return &block;
}

#endif
