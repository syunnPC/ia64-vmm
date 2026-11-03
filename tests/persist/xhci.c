/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xHCI 1.2 §4.3.5/4.6.8/4.9.4/5.4.8: unpowered roots, USB3
 * training, all DMA ranges, control short packets and host halt recovery.
 * QEMU hcd-xhci.c initializes PORTSC.PP to one; test power-on here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/plog_xhci.c"
static unsigned char mmio[8192], output[4096], input[4096], buffer[16384], allocation[32 * 4096];
static struct xhci_trb commands[256], controls[256], events[256], bulk_rings[2][256];
static uint64_t ticks, dma_pa;
static unsigned train_at, train_port, produced, order[32], order_count;
static bool simulate, short_data, usb, storage;
static unsigned controls_done, bulk_done[2], bot_phase, bot_length, bot_opcode, cache_mode, syncs,
    selects;
static uint32_t bot_tag;
static bool disabled_cache, initializing;
static _Alignas(4096) uint8_t dma_pool[64 * 4096];
static unsigned allocated, allocated_pages, high_allocation;
static uintptr_t high_pointer;
static const struct pci_device device = {.class_code = 0x0c0330,
                                         .owner = PCI_OWNER_VMM,
                                         .reservation = "vmm-media",
                                         .bars = {{.size = sizeof(mmio)}}};
unsigned platform_pci_count(void)
{
    return 1;
}
const struct pci_device *platform_pci_device(unsigned index)
{
    assert(!index);
    return &device;
}
uint8_t *platform_pci_map_physical(uint64_t base, uint64_t size)
{
    (void)base;
    assert(size == sizeof(mmio));
    return mmio;
}
void platform_pci_host_write(uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    (void)bdf;
    assert(off == 4 && value == 6 && size == 2);
}
void vmm_log(enum log_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
bool log_in_panic(void)
{
    return false;
}
void hal_panic(const char *message)
{
    (void)message;
    abort();
}
void *hal_alloc_pages(size_t n, uint32_t flags)
{
    (void)flags;
    if (!initializing)
        return allocation;
    assert(allocated_pages + n <= 64);
    uint8_t *result = dma_pool + allocated_pages * 4096;
    allocated_pages += n;
    if (++allocated == high_allocation)
        high_pointer = (uintptr_t)result;
    return result;
}
bool amd64_memory_physical(uintptr_t address, uint64_t *pa)
{
    if (initializing)
        *pa = address == high_pointer ? UINT64_C(0x100000000)
                                      : 0x100000 + address - (uintptr_t)dma_pool;
    else
        *pa = dma_pa;
    return true;
}
uint64_t hal_counter_hz(void)
{
    return 1000;
}
uint64_t hal_counter(void)
{
    ++ticks;
    if (initializing) {
        if (reg(x.op) == 2)
            wr(x.op, 0);
        wr(x.op + 4, reg(x.op) & 1 ? 0 : 1);
    }
    if (train_at && ticks < train_at) {
        unsigned off = x.op + 0x400 + (train_port - 1) * 16;
        assert(reg(off) & 0x200);
        wr(off, 0x200 | 1 | 7u << 5); /* Link detection precedes U0. */
    }
    if (train_at && ticks >= train_at) {
        unsigned off = x.op + 0x400 + (train_port - 1) * 16;
        assert(reg(off) & 0x200); /* Link appears only after PP. */
        wr(off, 0x200 | 3 | 4u << 10);
        train_at = 0;
    }
    if (simulate && produced < x.command.next) {
        unsigned i = produced++;
        unsigned type = commands[i].control >> 10 & 63;
        order[order_count++] = type;
        unsigned at = x.event_next;
        events[at] =
            (struct xhci_trb){x.command.physical + i * 16, 1u << 24, 33u << 10 | 1u << 24 | 1};
    }
    if (usb && controls_done < x.control.next) {
        unsigned next = controls_done, length = controls[next].parameter >> 48;
        unsigned request = controls[next].parameter >> 8 & 255;
        unsigned value = controls[next].parameter >> 16 & 65535;
        if (request == 6 && (value >> 8) == 1) {
            memset(buffer, 0, length);
            buffer[0] = 18;
            buffer[1] = 1;
            buffer[7] = 64;
        } else if (request == 6 && (value >> 8) == 2) {
            const uint8_t config[] = {9,    2, 32, 0, 1,    1, 0, 0x80, 50, 9, 4, 0, 0, 2, 8, 6,
                                      0x50, 0, 7,  5, 0x81, 2, 0, 2,    0,  7, 5, 1, 2, 0, 2, 0};
            memcpy(buffer, config, length);
        } else if (request == 0xff) {
            order[order_count++] = 102;
        } else if (request == 1) {
            assert(order_count && (order[order_count - 1] == 14 || order[order_count - 1] == 12));
            order[order_count++] = 101;
        } else if (request == 9) {
            assert(order_count && order[order_count - 1] == 12);
            order[order_count++] = 100;
        }
        unsigned at = x.event_next;
        if (length)
            events[at++] =
                (struct xhci_trb){x.control.physical + (next + 1) * 16,
                                  (short_data && length == 18 ? 13u << 24 | 11 : 1u << 24),
                                  32u << 10 | 1u << 16 | 1u << 24 | 1};
        events[at] = (struct xhci_trb){x.control.physical + (next + (length ? 2 : 1)) * 16,
                                       1u << 24, 32u << 10 | 1u << 16 | 1u << 24 | 1};
        controls_done = x.control.next;
    }
    if (storage)
        for (unsigned which = 0; which < 2; ++which) {
            if (bulk_done[which] == x.bulk[which].next)
                continue;
            unsigned i = bulk_done[which]++, actual = bulk_rings[which][i].status;
            if (bot_phase == 0) {
                assert(which == 1 && actual == 31);
                memcpy(&bot_tag, buffer + 4, 4);
                memcpy(&bot_length, buffer + 8, 4);
                bot_opcode = buffer[15];
                if (bot_opcode == 0x35)
                    ++syncs;
                if (bot_opcode == 0x15)
                    ++selects;
                if (bot_opcode == 0x2a)
                    assert((buffer[16] & 8) == (cache_mode == 1 ? 8 : 0));
                bot_phase = bot_length ? 1 : 2;
            } else if (bot_phase == 1) {
                if (bot_opcode == 0x1a) {
                    assert(which == 0);
                    memset(buffer, 0, actual);
                    buffer[0] = 23;
                    buffer[2] = cache_mode == 1 ? 0x10 : 0;
                    buffer[4] = 8;
                    buffer[5] = 18;
                    buffer[6] = cache_mode && !disabled_cache ? 4 : 0;
                    actual = 24;
                } else if (bot_opcode == 3) {
                    memset(buffer, 0, actual);
                    buffer[0] = 0x70;
                    buffer[2] = 5;
                    buffer[7] = 10;
                } else if (bot_opcode == 0x15) {
                    assert(!(buffer[6] & 4));
                    disabled_cache = true;
                }
                bot_phase = 2;
            } else {
                assert(which == 0 && actual == 13);
                memset(buffer, 0, 13);
                memcpy(buffer, "USBS", 4);
                memcpy(buffer + 4, &bot_tag, 4);
                if (bot_opcode == 0x1a) {
                    unsigned residue = bot_length - 24;
                    memcpy(buffer + 8, &residue, 4);
                }
                if (bot_opcode == 0x35)
                    buffer[12] = 1; /* Unsupported SYNCHRONIZE CACHE. */
                bot_phase = 0;
            }
            unsigned length = bulk_rings[which][i].status;
            events[x.event_next] =
                (struct xhci_trb){x.bulk[which].physical + i * 16,
                                  (actual < length ? 13u : 1u) << 24 | (length - actual),
                                  32u << 10 | x.endpoint[which] << 16 | 1u << 24 | 1};
        }
    return ticks;
}
static void setup(void)
{
    memset(&x, 0, sizeof(x));
    memset(mmio, 0, sizeof(mmio));
    memset(output, 0, sizeof(output));
    memset(input, 0, sizeof(input));
    memset(events, 0, sizeof(events));
    x.mmio = mmio;
    x.op = 32;
    x.runtime = 2048;
    x.bells = 3072;
    x.output = output;
    x.input = input;
    x.context_size = 32;
    x.input_pa = 0x50000;
    x.buffer = buffer;
    x.buffer_pa = 0x60000;
    x.events = events;
    x.event_pa = 0x70000;
    x.command = (struct xhci_ring){.trbs = commands, .physical = 0x10000, .cycle = 1};
    x.control = (struct xhci_ring){.trbs = controls, .physical = 0x20000, .cycle = 1};
    for (unsigned i = 0; i < 2; ++i) {
        x.bulk[i] =
            (struct xhci_ring){.trbs = bulk_rings[i], .physical = 0x30000 + i * 4096, .cycle = 1};
        x.endpoint[i] = i ? 2 : 3;
        x.address[i] = i ? 1 : 0x81;
    }
    x.slot = 1;
    x.live = true;
    x.event_cycle = 1;
    ticks = train_at = produced = order_count = 0;
    simulate = short_data = usb = storage = false;
    controls_done = bulk_done[0] = bulk_done[1] = bot_phase = syncs = selects = 0;
    disabled_cache = initializing = false;
    allocated = allocated_pages = 0;
    high_pointer = 0;
}
static void control_events(unsigned length, unsigned actual)
{
    unsigned at = x.event_next, next = x.control.next;
    if (length) {
        events[at++] = (struct xhci_trb){x.control.physical + (next + 1) * 16,
                                         (actual < length ? 13u : 1u) << 24 | (length - actual),
                                         32u << 10 | 1u << 16 | 1u << 24 | 1};
    }
    events[at] = (struct xhci_trb){x.control.physical + (next + (length ? 2 : 1)) * 16, 1u << 24,
                                   32u << 10 | 1u << 16 | 1u << 24 | 1};
}
int main(void)
{
    setup();
    train_port = 2;
    train_at = 800;
    x.usb3[2] = true;
    wr(x.op + 0x410, 1 | 7u << 5); /* CCS precedes USB3 U0/speed assignment. */
    assert(root_ports(3, true));
    assert(x.port == 2 && ticks >= 800 && ticks < 1100);
    for (unsigned p = 1; p <= 3; ++p)
        assert(reg(x.op + 0x400 + (p - 1) * 16) & 0x200);
    setup();
    assert(!root_ports(2, true));
    assert(ticks >= 1000 && ticks < 1100);
    setup();
    wr(x.op + 0x400, 1);
    assert(root_ports(1, false));
    setup();
    wr(x.op + 0x400, 1);
    wr(x.op + 0x410, 1);
    assert(!root_ports(2, false));
    /* Check the last byte too: a DMA object may straddle 4 GiB. */
    setup();
    x.ac64 = false;
    dma_pa = UINT32_MAX - 4095;
    assert(dma_pages(1));
    dma_pa++;
    assert(!dma_pages(1));
    assert(!dma_ring(&x.command));
    dma_pa = UINT64_C(0x100000000);
    assert(!dma_pages(1));
    x.ac64 = true;
    assert(dma_pages(1));
    dma_pa = UINT64_MAX - 4094;
    assert(!dma_pages(1));
    setup();
    control_events(18, 7);
    assert(control(0x80, 6, 0x100, 0, 18) == 7);
    assert((controls[1].control & ((1u << 2) | (1u << 5))) == ((1u << 2) | (1u << 5)));
    setup();
    control_events(18, 18);
    assert(control(0x80, 6, 0x100, 0, 18) == 18);
    for (unsigned state = 1; state <= 3; ++state) {
        setup();
        ((uint32_t *)(output + x.endpoint[0] * 32))[0] = state;
        simulate = usb = true;
        /* §4.6.8: Reset Endpoint, CLEAR_FEATURE, Set TR Dequeue. */
        unsigned commands_needed = state == 2 ? 3 : 2;
        assert(clear_halt(0));
        assert(order_count == commands_needed);
        if (state == 2)
            assert(order[0] == 14 && order[1] == 101 && order[2] == 16);
        else
            assert(order[0] == 12 && order[1] == 101);
        assert((controls[0].parameter & 0xffff) == 0x0102);
    }
    setup();
    simulate = usb = true;
    ((uint32_t *)(output + x.endpoint[0] * 32))[0] = 2;
    ((uint32_t *)(output + x.endpoint[1] * 32))[0] = 1;
    assert(reset_bot() && x.live);
    const unsigned recovery[] = {102, 14, 101, 16, 12, 101};
    assert(order_count == 6 && !memcmp(order, recovery, sizeof(recovery)));
    setup();
    simulate = usb = true;
    x.speed = 3;
    x.port = 1;
    x.endpoint[0] = x.endpoint[1] = 0;
    assert(enumerate(0));
    assert(order[order_count - 1] == 100 && order[order_count - 2] == 12);
    setup();
    simulate = usb = short_data = true;
    x.speed = 3;
    x.port = 1;
    x.endpoint[0] = x.endpoint[1] = 0;
    assert(!enumerate(0));
    assert(order_count == 2); /* Enable Slot, Address Device only. */
    for (cache_mode = 0; cache_mode <= 2; ++cache_mode) {
        setup();
        storage = true;
        assert(scsi_disk_caching(&disk));
        assert(disk.fua == (cache_mode == 1));
        assert(syncs == (cache_mode == 2 ? 1u : 0u));
        assert(selects == (cache_mode == 2 ? 1u : 0u));
        disk.sectors = 1000;
        assert(write_blocks(100, 1, buffer));
        assert(flush());
        assert(syncs == (cache_mode == 2 ? 1u : 0u));
    }
    /* Controller initialization must check each ring, both contexts, the
     * event ring, DCBAA, ERST, data, scratchpad array and scratchpad pages
     * before programming even the first hardware DMA pointer. */
    for (high_allocation = 1; high_allocation <= 12; ++high_allocation) {
        setup();
        initializing = true;
        wr(0, 0x100u << 16 | 32);
        wr(4, 1u << 24 | 1);
        wr(8, 2u << 27);
        wr(0x10, 0);
        wr(0x14, x.bells);
        wr(0x18, x.runtime);
        wr(x.op + 4, 1);
        wr(x.op + 8, 1);
        assert(!amd64_plog_xhci(true));
        assert(allocated >= high_allocation);
        assert(!reg(x.op + 0x30) && !reg(x.op + 0x18) && !reg(x.runtime + 0x30));
    }
    /* No stick: the start fails after Run/Stop, and the controller is
     * halted again (xHCI 1.2 §5.4.1, USBCMD R/S; USBSTS HCH). */
    setup();
    initializing = true;
    high_allocation = 0;
    allocated = 0;
    wr(0, 0x100u << 16 | 32);
    wr(4, 1u << 24 | 1);
    wr(8, 2u << 27);
    wr(0x10, 0);
    wr(0x14, x.bells);
    wr(0x18, x.runtime);
    wr(x.op + 4, 1);
    wr(x.op + 8, 1);
    assert(!amd64_plog_xhci(false));
    assert(!(reg(x.op) & 1) && (reg(x.op + 4) & 1) && !x.running);
    puts("PLOG xHCI: passed");
}
