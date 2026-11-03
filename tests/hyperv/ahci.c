/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AHCI 1.3.1 sections 3.1, 3.3, 4.2 and 10: registers and command DMA. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "platform/ahci.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"

#define BASE UINT64_C(0x100000000)
#define BAR UINT64_C(0xfebf0000)
#define SECTORS 4096u
static uint8_t ram[2 * 1024 * 1024], firmware[4096], disk[SECTORS * 512];
static bool level, failed, reset_during_io, mutate_prdt, fail_alloc;
static unsigned calls, flushes, notifications, allocations;
static uint64_t disk_lba_window;
static struct platform_block block;
void *hal_alloc_pages(size_t n, unsigned flags)
{
    (void)flags;
    if (fail_alloc)
        return NULL;
    ++allocations;
    return calloc(n, 4096);
}
void hal_free_pages(void *p, size_t n)
{
    (void)n;
    --allocations;
    free(p);
}
void vmm_log(enum log_level l, const char *fmt, ...)
{
    (void)l;
    (void)fmt;
}
static void put32(unsigned at, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        ram[at + i] = value >> (8 * i);
}
static void put64(unsigned at, uint64_t value)
{
    put32(at, value);
    put32(at + 4, value >> 32);
}
static uint32_t get32(unsigned at)
{
    return ram[at] | (uint32_t)ram[at + 1] << 8 | (uint32_t)ram[at + 2] << 16 |
           (uint32_t)ram[at + 3] << 24;
}
static bool read_disk(void *ctx, uint64_t lba, unsigned n, void *p)
{
    unsigned sector = block.sector_bytes ? block.sector_bytes : 512;
    assert(lba >= disk_lba_window);
    lba -= disk_lba_window;
    assert(ctx == disk && lba < sizeof(disk) / sector && n && n <= sizeof(disk) / sector - lba);
    assert((uintptr_t)p < (uintptr_t)ram || (uintptr_t)p >= (uintptr_t)(ram + sizeof(ram)));
    ++calls;
    if (reset_during_io)
        platform_ahci_reset();
    if (mutate_prdt)
        put64(4096 + 128, BASE + sizeof(ram));
    if (failed)
        return false;
    memcpy(p, disk + lba * sector, n * sector);
    return true;
}
static bool write_disk(void *ctx, uint64_t lba, unsigned n, const void *p)
{
    assert(lba >= disk_lba_window);
    lba -= disk_lba_window;
    assert(ctx == disk && lba < SECTORS && n && n <= SECTORS - lba);
    ++calls;
    if (failed)
        return false;
    memcpy(disk + lba * 512, p, n * 512);
    return true;
}
static bool flush_disk(void *ctx)
{
    assert(ctx == disk);
    ++flushes;
    return !failed;
}
static void irq(void *ctx, bool high)
{
    assert(ctx == &level);
    level = high;
}
static void notify(void *ctx)
{
    assert(ctx == &notifications);
    ++notifications;
}
static struct platform_block block = {.context = disk,
                                      .sectors = SECTORS,
                                      .read = read_disk,
                                      .write = write_disk,
                                      .flush = flush_disk};
static uint32_t reg(unsigned off)
{
    return platform_ahci_ops.read(NULL, NULL, off, 4);
}
static void wr(unsigned off, uint64_t value)
{
    platform_ahci_ops.write(NULL, NULL, off, value, 4);
}
static void setup(void)
{
    platform_ahci_cleanup();
    assert(!allocations);
    memset(ram, 0, sizeof(ram));
    level = failed = reset_during_io = mutate_prdt = false;
    calls = flushes = notifications = 0;
    disk_lba_window = 0;
    block.read_only = fail_alloc = false;
    block.sector_bytes = 0;
    block.sectors = SECTORS;
    assert(platform_ahci_init(&block, 1, BAR, 20, irq, &level, notify, &notifications));
    platform_ahci_cfg_write(4, 6, 2);
    wr(4, 0x80000002);
    wr(0x100, BASE);
    wr(0x104, BASE >> 32);
    wr(0x108, BASE + 1024);
    wr(0x10c, BASE >> 32);
    wr(0x114, UINT32_MAX);
    wr(0x118, 0x11);
}
static void command(unsigned slot, unsigned op, uint64_t lba, unsigned sectors, bool write)
{
    unsigned header = slot * 32;
    memset(ram + header, 0, 32);
    memset(ram + 4096, 0, 160);
    put32(header, 5 | (write ? 0x40 : 0) | (sectors ? 1u << 16 : 0));
    put64(header + 8, BASE + 4096);
    ram[4096] = 0x27;
    ram[4097] = 0x80;
    ram[4098] = op;
    ram[4100] = lba;
    ram[4101] = lba >> 8;
    ram[4102] = lba >> 16;
    ram[4103] = 0x40;
    ram[4104] = lba >> 24;
    ram[4105] = lba >> 32;
    ram[4106] = lba >> 40;
    ram[4108] = sectors;
    ram[4109] = sectors >> 8;
    if (sectors) {
        put64(4224, BASE + 8192);
        put32(4236, sectors * 512 - 1);
    }
    wr(0x110, UINT32_MAX);
    wr(8, 1);
    wr(0x138, 1u << slot);
    assert(reg(0x138) == (1u << slot) && notifications);
}
static void success(unsigned slot, unsigned bytes)
{
    platform_ahci_poll();
    assert(!reg(0x138) && !(reg(0x120) & 1) && get32(slot * 32 + 4) == bytes);
    assert(ram[1024 + 0x40] == 0x34 && level && (reg(0x110) & 1));
    wr(0x110, UINT32_MAX);
    wr(8, 1);
    assert(!level);
}
static void optical_setup(void)
{
    setup();
    platform_ahci_cleanup();
    block.read_only = true;
    block.sector_bytes = 2048;
    block.sectors = sizeof(disk) / 2048;
    assert(platform_ahci_init(&block, 1, BAR, 20, irq, &level, notify, &notifications));
    platform_ahci_cfg_write(4, 6, 2);
    wr(4, 0x80000002);
    wr(0x100, BASE);
    wr(0x104, BASE >> 32);
    wr(0x108, BASE + 1024);
    wr(0x10c, BASE >> 32);
    wr(0x114, UINT32_MAX);
    wr(0x118, 0x01000011);
}
static void packet(const uint8_t cdb[12], unsigned bytes)
{
    command(0, 0xa0, 0, bytes ? 1 : 0, false);
    put32(0, 5 | 0x20 | (bytes ? 1u << 16 : 0));
    if (bytes)
        put32(4236, ((bytes + 1) & ~1u) - 1);
    memcpy(ram + 4160, cdb, 12);
}
static void request_sense(unsigned key, unsigned asc)
{
    const uint8_t cdb[12] = {3, 0, 0, 0, 18};
    packet(cdb, 18);
    success(0, 18);
    assert((ram[8194] & 15) == key && ram[8204] == asc);
}
struct medium {
    uint8_t data[8192];
    unsigned sector, reads, writes;
    bool fail, reset;
};
static bool medium_read(void *context, uint64_t lba, unsigned count, void *buffer)
{
    struct medium *m = context;
    assert(lba < sizeof(m->data) / m->sector && count <= sizeof(m->data) / m->sector - lba);
    ++m->reads;
    if (m->reset) {
        wr(0x12c, 1);
        wr(0x12c, 0);
    }
    if (m->fail)
        return false;
    memcpy(buffer, m->data + lba * m->sector, count * m->sector);
    return true;
}
static bool medium_write(void *context, uint64_t lba, unsigned count, const void *buffer)
{
    struct medium *m = context;
    assert(lba < sizeof(m->data) / m->sector && count <= sizeof(m->data) / m->sector - lba);
    ++m->writes;
    memcpy(m->data + lba * m->sector, buffer, count * m->sector);
    return true;
}
static bool medium_flush(void *context)
{
    (void)context;
    return true;
}
static unsigned multi_base(unsigned port)
{
    return 0x10000 + port * 0x4000;
}
static void multi_configure(unsigned port)
{
    uint64_t base = BASE + multi_base(port);
    unsigned at = 0x100 + port * 0x80;
    wr(at, base);
    wr(at + 4, base >> 32);
    wr(at + 8, base + 1024);
    wr(at + 12, base >> 32);
    wr(at + 20, UINT32_MAX);
    wr(at + 24, 0x11);
}
static void multi_command(unsigned port, unsigned op, unsigned bytes, bool write,
                          const uint8_t *cdb)
{
    unsigned b = multi_base(port), table = b + 2048, at = 0x100 + port * 0x80;
    memset(ram + b, 0, 32);
    memset(ram + table, 0, 144);
    put32(b, 5 | (write ? 0x40 : 0) | (cdb ? 0x20 : 0) | (bytes ? 1u << 16 : 0));
    put64(b + 8, BASE + table);
    ram[table] = 0x27;
    ram[table + 1] = 0x80;
    ram[table + 2] = op;
    ram[table + 7] = 0xe0;
    ram[table + 12] = 1;
    if (cdb)
        memcpy(ram + table + 64, cdb, 12);
    if (bytes) {
        put64(table + 128, BASE + b + 4096);
        put32(table + 140, bytes - 1);
    }
    wr(at + 16, UINT32_MAX);
    wr(8, 1u << port);
    wr(at + 56, 1);
}
static void multiport(void)
{
    platform_ahci_cleanup();
    memset(ram, 0, sizeof(ram));
    static struct medium media[3] = {{.sector = 512}, {.sector = 512}, {.sector = 2048}};
    struct platform_block blocks[PLATFORM_AHCI_MAX_PORTS];
    for (unsigned i = 0; i < 3; ++i) {
        memset(media[i].data, 0x31 + i, sizeof(media[i].data));
        blocks[i] = (struct platform_block){.context = media + i,
                                            .sectors = sizeof(media[i].data) / media[i].sector,
                                            .read = medium_read,
                                            .write = medium_write,
                                            .flush = medium_flush,
                                            .sector_bytes = media[i].sector,
                                            .read_only = i == 2};
    }
    assert(!platform_ahci_init(blocks, 0, BAR, 20, irq, &level, notify, &notifications));
    assert(!platform_ahci_init(blocks, 33, BAR, 20, irq, &level, notify, &notifications));
    assert(platform_ahci_init(blocks, 3, BAR, 20, irq, &level, notify, &notifications));
    platform_ahci_cfg_write(4, 6, 2);
    wr(4, 0x80000002);
    assert((reg(0) & 31) == 2 && reg(12) == 7 && reg(0x228) == 0x113 && !reg(0x2a8));
    for (unsigned i = 0; i < 3; ++i)
        multi_configure(i);
    assert(reg(0x124) == 0x101 && reg(0x1a4) == 0x101 && reg(0x224) == 0xeb140101);
    multi_command(0, 0x25, 512, false, NULL);
    multi_command(1, 0x25, 512, false, NULL);
    multi_command(2, 0xa0, 2048, false, (const uint8_t[12]){0x28, 0, 0, 0, 0, 0, 0, 0, 1});
    platform_ahci_poll();
    assert(reg(8) == 7 && level);
    for (unsigned i = 0; i < 3; ++i) {
        unsigned b = multi_base(i), at = 0x100 + i * 0x80;
        assert(media[i].reads == 1 && !reg(at + 56) && !(reg(at + 32) & 1));
        assert(!memcmp(ram + b + 4096, media[i].data, media[i].sector));
        wr(at + 16, UINT32_MAX);
        wr(8, 1u << i);
        assert(level == (i != 2));
    }
    memset(ram + multi_base(1) + 4096, 0x77, 512);
    multi_command(1, 0x35, 512, true, NULL);
    platform_ahci_poll();
    assert(media[1].writes == 1 && media[1].data[0] == 0x77 && media[0].data[0] == 0x31);
    multi_command(0, 0xec, 512, false, NULL);
    multi_command(1, 0xec, 512, false, NULL);
    platform_ahci_poll();
    assert(memcmp(ram + multi_base(0) + 4096 + 20, ram + multi_base(1) + 4096 + 20, 20));
    media[0].fail = true;
    multi_command(0, 0x25, 512, false, NULL);
    multi_command(1, 0x25, 512, false, NULL);
    platform_ahci_poll();
    assert((reg(0x120) & 1) && !(reg(0x1a0) & 1));
    media[0].fail = false;
    /* Resetting an active port cancels its DMA without cancelling siblings. */
    media[0].reset = true;
    memset(ram + multi_base(0) + 4096, 0, 512);
    multi_command(0, 0x25, 512, false, NULL);
    multi_command(1, 0x25, 512, false, NULL);
    platform_ahci_poll();
    media[0].reset = false;
    assert(!ram[multi_base(0) + 4096] && !reg(0x138) && !reg(0x1b8) && !(reg(0x1a0) & 1));
    assert(!memcmp(ram + multi_base(1) + 4096, media[1].data, 512));
    for (unsigned i = 0; i < 3; ++i)
        multi_command(i, i == 2 ? 0xa1 : 0xec, 512, false, NULL);
    wr(4, 0x80000001);
    platform_ahci_poll();
    for (unsigned i = 0; i < 3; ++i)
        assert(!reg(0x138 + i * 0x80) && !reg(0x100 + i * 0x80));
    platform_ahci_cleanup();
    for (unsigned i = 3; i < PLATFORM_AHCI_MAX_PORTS; ++i)
        blocks[i] = blocks[0];
    assert(platform_ahci_init(blocks, PLATFORM_AHCI_MAX_PORTS, BAR, 20, irq, &level, notify,
                              &notifications));
    platform_ahci_cfg_write(4, 2, 2);
    assert((reg(0) & 31) == 31 && reg(12) == UINT32_MAX);
    platform_ahci_cfg_write(4, 6, 2);
    wr(4, 0x80000002);
    multi_configure(31);
    multi_command(31, 0x25, 512, false, NULL);
    platform_ahci_poll();
    assert(reg(8) == (1u << 31) && level && !reg(0x10b8));
    assert(!memcmp(ram + multi_base(31) + 4096, media[0].data, 512));
    wr(0x1090, UINT32_MAX);
    wr(8, 1u << 31);
    assert(!level);
    platform_ahci_cleanup();
    assert(!allocations);
    /* ATA IDENTIFY advertises native 4 KiB logical sectors without translation. */
    static struct medium native = {.sector = 4096};
    memset(native.data, 0x49, sizeof(native.data));
    blocks[0] = (struct platform_block){.context = &native,
                                        .sectors = 2,
                                        .sector_bytes = 4096,
                                        .read = medium_read,
                                        .write = medium_write,
                                        .flush = medium_flush};
    assert(platform_ahci_init(blocks, 1, BAR, 20, irq, &level, notify, &notifications));
    platform_ahci_cfg_write(4, 6, 2);
    wr(4, 0x80000002);
    multi_configure(0);
    multi_command(0, 0xec, 512, false, NULL);
    platform_ahci_poll();
    unsigned b = multi_base(0);
    assert((get32(b + 4096 + 106 * 2) & 0xffff) == 0x5000 && get32(b + 4096 + 117 * 2) == 2048);
    multi_command(0, 0x25, 4096, false, NULL);
    platform_ahci_poll();
    assert(native.reads == 1 && !memcmp(ram + b + 4096, native.data, 4096) && get32(b + 4) == 4096);
    memset(ram + b + 4096, 0x88, 4096);
    multi_command(0, 0x35, 4096, true, NULL);
    platform_ahci_poll();
    assert(native.writes == 1 && !memcmp(ram + b + 4096, native.data, 4096));
    platform_ahci_cleanup();
    assert(!allocations);
}
int main(void)
{
    assert(gm_add_region(&(struct gm_region){BASE, sizeof(ram), GM_RAM, "ram", ram, NULL, NULL}));
    assert(gm_add_region(&(struct gm_region){BASE + sizeof(ram), sizeof(firmware), GM_FIRMWARE,
                                             "firmware", firmware, NULL, NULL}));
    setup();
    assert(platform_ahci_cfg_read(8, 4) >> 8 == 0x010601);
    platform_ahci_cfg_write(0x24, UINT32_MAX, 4);
    assert(platform_ahci_cfg_read(0x24, 4) == 0xffffe000u);
    platform_ahci_cfg_write(0x24, BAR, 4);
    assert(platform_ahci_cfg_read(0x24, 4) == BAR && reg(0xc) == 1 && reg(0x128) == 0x113);
    assert((reg(0) & 0xc0000000u) == 0x80000000u);
    assert(((reg(0) >> 20) & 15) == 1 && ((reg(0x128) >> 4) & 15) == 1);
    command(0, 0xec, 0, 1, false);
    assert(!calls);
    success(0, 512);
    assert((ram[8192 + 83 * 2 + 1] & 4) && get32(8192 + 100 * 2) == SECTORS);
    for (unsigned i = 0; i < 1024; ++i)
        ram[8192 + i] = (uint8_t)(i * 17);
    command(31, 0x35, 7, 2, true);
    assert(!calls);
    success(31, 1024);
    assert(calls == 1 && !memcmp(disk + 7 * 512, ram + 8192, 1024));
    memset(ram + 8192, 0, 1024);
    command(1, 0x25, 7, 2, false);
    success(1, 1024);
    assert(!memcmp(disk + 7 * 512, ram + 8192, 1024));
    command(0, 0xea, 0, 0, false);
    success(0, 0);
    assert(flushes == 1);
    /* Fragmented PRDs may cross sector boundaries and bounce-buffer chunks. */
    setup();
    for (unsigned i = 0; i < 1024 * 512; ++i)
        ram[8192 + i] = (uint8_t)(i * 31 + i / 512);
    command(0, 0x35, 256, 1024, true);
    put32(0, 5 | 0x40 | 3u << 16);
    put64(4224, BASE + 8192);
    put32(4236, 768 - 1);
    put64(4240, BASE + 8192 + 768);
    put32(4252, 64 * 1024 - 1);
    put64(4256, BASE + 8192 + 768 + 64 * 1024);
    put32(4268, 1024 * 512 - 768 - 64 * 1024 - 1);
    success(0, 1024 * 512);
    assert(calls == 8);
    assert(!memcmp(disk + 256 * 512, ram + 8192, 1024 * 512));
    memset(ram + 8192, 0, 1024 * 512);
    command(0, 0x25, 256, 1024, false);
    success(0, 1024 * 512);
    assert(!memcmp(disk + 256 * 512, ram + 8192, 1024 * 512));
    /* Disabling the volatile write cache flushes now and after later writes. */
    command(0, 0xef, 0, 0, false);
    ram[4099] = 0x82;
    success(0, 0);
    assert(flushes == 1);
    command(0, 0x35, 0, 1, true);
    success(0, 512);
    assert(flushes == 2);
    command(0, 0xec, 0, 1, false);
    success(0, 512);
    assert(!(ram[8192 + 85 * 2] & 0x20));
    command(0, 0xef, 0, 0, false);
    ram[4099] = 2;
    success(0, 0);
    command(0, 0xec, 0, 1, false);
    success(0, 512);
    assert(ram[8192 + 85 * 2] & 0x20);
    /* PIO commands and 28-bit zero count (256 sectors) use the same medium. */
    command(0, 0x20, 256, 256, false);
    ram[4108] = 0;
    success(0, 256 * 512);
    assert(ram[1056] == 0x5f && !memcmp(disk + 256 * 512, ram + 8192, 256 * 512));
    command(0, 0x30, 256, 1, true);
    success(0, 512);
    /* Unsupported FPDMA/ATAPI and malformed headers never call the backend. */
    unsigned rejected = calls;
    command(0, 0x60, 0, 1, false);
    platform_ahci_poll();
    assert(calls == rejected && (reg(0x110) & 1u << 30));
    command(0, 0x25, 0, 1, false);
    put32(0, 5 | 0x20 | 1u << 16);
    platform_ahci_poll();
    assert(calls == rejected && (reg(0x110) & 1u << 30));
    command(0, 0x35, 0, 1, false);
    platform_ahci_poll();
    assert(calls == rejected && (reg(0x110) & 1u << 30));
    /* Full PRDT is checked before any backend mutation, even late entries. */
    command(0, 0x35, 7, 2, true);
    put32(0, 5 | 0x40 | 2u << 16);
    put32(4236, 511);
    put64(4240, BASE + sizeof(ram));
    put32(4252, 511);
    unsigned before = calls;
    platform_ahci_poll();
    assert(calls == before && (reg(0x110) & 1u << 29));
    command(0, 0x35, SECTORS - 1, 2, true);
    platform_ahci_poll();
    assert(calls == before && (reg(0x120) & 1));
    command(0, 0x25, UINT64_C(1) << 48, 1, false);
    put64(8, UINT64_MAX - 127);
    platform_ahci_poll();
    assert(calls == before && (reg(0x110) & 1u << 29));
    command(0, 0x35, 0, 1, true);
    fail_alloc = true;
    platform_ahci_poll();
    fail_alloc = false;
    assert(calls == before && (reg(0x110) & 1u << 29));
    /* Snapshot remains authoritative if a callback changes the guest PRDT. */
    setup();
    mutate_prdt = true;
    command(0, 0x25, 7, 2, false);
    success(0, 1024);
    assert(!memcmp(disk + 7 * 512, ram + 8192, 1024));
    /* A concurrent reset drops the command's stale data/FIS and interrupt. */
    setup();
    reset_during_io = true;
    command(0, 0x25, 7, 1, false);
    platform_ahci_poll();
    platform_ahci_cfg_write(4, 2, 2);
    assert(!reg(0x138) && !level && !ram[8192] && !ram[1088]);
    setup();
    failed = true;
    command(0, 0x25, 7, 1, false);
    platform_ahci_poll();
    assert((reg(0x110) & 1u << 30) && (reg(0x120) & 1) && !reg(0x138));
    /* Reset and subsequent I/O recover from an ordinary medium error. */
    failed = false;
    wr(0x12c, 1);
    assert((reg(0x128) & 15) == 1);
    wr(0x12c, 0);
    assert(reg(0x124) == 0x101);
    command(0, 0x25, 7, 1, false);
    success(0, 512);
    /* EDK2 chooses LBA48 above its 28-bit capacity threshold. */
    setup();
    platform_ahci_cleanup();
    disk_lba_window = UINT64_C(1) << 32;
    block.sectors = disk_lba_window + SECTORS;
    assert(platform_ahci_init(&block, 1, BAR, 20, irq, &level, notify, &notifications));
    platform_ahci_cfg_write(4, 6, 2);
    wr(4, 0x80000002);
    wr(0x100, BASE);
    wr(0x104, BASE >> 32);
    wr(0x108, BASE + 1024);
    wr(0x10c, BASE >> 32);
    wr(0x114, UINT32_MAX);
    wr(0x118, 0x11);
    command(0, 0xec, 0, 1, false);
    success(0, 512);
    assert(get32(8192 + 60 * 2) == 0x0fffffff && get32(8192 + 100 * 2) == SECTORS);
    assert(get32(8192 + 102 * 2) == 1);
    command(0, 0x25, disk_lba_window + 7, 1, false);
    ram[4103] = 0xe0;
    success(0, 512);
    assert(!memcmp(disk + 7 * 512, ram + 8192, 512));
    command(0, 0x35, disk_lba_window + 8, 1, true);
    ram[4103] = 0xe0;
    success(0, 512);
    assert(!memcmp(disk + 8 * 512, ram + 8192, 512));
    /* Interrupt masking affects the line, while status remains latched. */
    setup();
    wr(4, 0x80000000);
    command(0, 0xec, 0, 1, false);
    platform_ahci_poll();
    assert(!level && (reg(0x110) & 1) && (reg(8) & 1));
    wr(4, 0x80000002);
    assert(level);
    platform_ahci_cfg_write(5, 4, 1);
    assert(!level);
    platform_ahci_cfg_write(5, 0, 1);
    assert(level);
    wr(0x110, UINT32_MAX);
    wr(8, 1);
    assert(!level);
    /* HBA reset keeps PCI decode and clears DMA pointers; registers are immutable. */
    wr(0, 0);
    assert(reg(0) & 0x80000000u);
    wr(4, 0x80000001);
    assert(platform_ahci_cfg_read(4, 2) == 6 && !reg(0x138) && !reg(0x100) && !reg(0x108));
    setup();
    wr(0x118, 0x10);
    command(0, 0xec, 0, 1, false);
    platform_ahci_poll();
    assert(reg(0x138));
    unsigned notified = notifications;
    wr(0x118, 0x11);
    assert(notifications > notified);
    success(0, 512);
    /* The 16-bit PRDT length is supported without a smaller private limit. */
    setup();
    command(0, 0x25, 7, 1, false);
    put32(0, 5 | 65535u << 16);
    for (unsigned i = 0; i < 65535; ++i) {
        unsigned at = 4224 + i * 16;
        put64(at, BASE + 1536 * 1024);
        put32(at + 8, 0);
        put32(at + 12, 1);
    }
    success(0, 512);
    assert(calls == 1 && allocations == 1);
    /* EDK2's ScsiBus/ScsiDisk enumeration and optical block reads. */
    optical_setup();
    assert(reg(0x124) == 0xeb140101);
    command(0, 0xa1, 0, 1, false);
    success(0, 512);
    assert(ram[8192] == 0x80 && ram[8193] == 0x85);
    packet((const uint8_t[12]){0}, 0);
    success(0, 0);
    packet((const uint8_t[12]){0x12, 0, 0, 0, 36}, 36);
    success(0, 36);
    assert(ram[8192] == 5 && ram[8193] == 0x80 && !memcmp(ram + 8200, "IA64VMM ", 8));
    packet((const uint8_t[12]){0x12, 0, 0, 1, 0}, 256);
    success(0, 36);
    packet((const uint8_t[12]){0x12, 1, 0, 0, 32}, 32);
    success(0, 6);
    assert(ram[8195] == 2 && ram[8196] == 0 && ram[8197] == 0x80);
    packet((const uint8_t[12]){0x25}, 8);
    success(0, 8);
    assert(ram[8192] == 0 && ram[8194] == 3 && ram[8195] == 0xff && ram[8198] == 8);
    for (unsigned i = 0; i < 80 * 2048; ++i)
        disk[7 * 2048 + i] = (uint8_t)(i * 13 + i / 2048);
    packet((const uint8_t[12]){0x28, 0, 0, 0, 0, 7, 0, 0, 80}, 80 * 2048);
    success(0, 80 * 2048);
    assert(calls == 3 && !memcmp(ram + 8192, disk + 7 * 2048, 80 * 2048));
    packet((const uint8_t[12]){0xa8, 0, 0, 0, 0, 7, 0, 0, 0, 1}, 2048);
    success(0, 2048);
    assert(!memcmp(ram + 8192, disk + 7 * 2048, 2048));
    packet((const uint8_t[12]){0xff}, 0);
    platform_ahci_poll();
    assert(reg(0x120) & 1);
    request_sense(5, 0x20);
    request_sense(0, 0);
    packet((const uint8_t[12]){0x28, 0, 0, 0, 3, 0xff, 0, 0, 2}, 4096);
    before = calls;
    platform_ahci_poll();
    assert(calls == before && (reg(0x120) & 1));
    request_sense(5, 0x21);
    packet((const uint8_t[12]){0x2a, 0, 0, 0, 0, 7, 0, 0, 1}, 2048);
    platform_ahci_poll();
    assert(calls == before);
    request_sense(7, 0x27);
    failed = true;
    packet((const uint8_t[12]){0x28, 0, 0, 0, 0, 7, 0, 0, 1}, 2048);
    platform_ahci_poll();
    failed = false;
    request_sense(3, 0x11);
    packet((const uint8_t[12]){0x43, 0, 0, 0, 0, 0, 0, 0, 20}, 20);
    success(0, 20);
    assert(ram[8193] == 18 && ram[8194] == 1 && ram[8195] == 1 && ram[8206] == 0xaa);
    packet((const uint8_t[12]){0x5a, 8, 0x2a, 0, 0, 0, 0, 0, 64}, 64);
    success(0, 38);
    assert(ram[8195] & 0x80);
    packet((const uint8_t[12]){0x5a, 8, 0x3f, 0, 0, 0, 0, 0, 64}, 64);
    success(0, 50);
    assert(ram[8200] == 1 && ram[8212] == 0x2a);
    packet((const uint8_t[12]){0x46, 0, 0, 0, 0, 0, 0, 0, 64}, 64);
    success(0, 20);
    assert(ram[8199] == 0x10 && ram[8209] == 0x10 && ram[8210] == 1);
    packet((const uint8_t[12]){0x4a, 1, 0, 0, 0x10, 0, 0, 0, 8}, 8);
    success(0, 8);
    assert(ram[8194] == 4 && ram[8197] == 2);
    packet((const uint8_t[12]){0x1e, 0, 0, 0, 1}, 0);
    success(0, 0);
    packet((const uint8_t[12]){0x1b, 0, 0, 0, 2}, 0);
    platform_ahci_poll();
    request_sense(5, 0x53);
    packet((const uint8_t[12]){0x1e}, 0);
    success(0, 0);
    packet((const uint8_t[12]){0x1b, 0, 0, 0, 2}, 0);
    success(0, 0);
    packet((const uint8_t[12]){0}, 0);
    platform_ahci_poll();
    request_sense(2, 0x3a);
    packet((const uint8_t[12]){0x1b, 0, 0, 0, 3}, 0);
    success(0, 0);
    packet((const uint8_t[12]){0}, 0);
    success(0, 0);
    setup();
    platform_ahci_cfg_write(4, 2, 2);
    command(0, 0x35, 1, 1, true);
    platform_ahci_poll();
    assert(!calls && (reg(0x110) & 1u << 29));
    setup();
    platform_ahci_cleanup();
    block.read_only = true;
    assert(platform_ahci_init(&block, 1, BAR, 20, irq, &level, notify, &notifications));
    platform_ahci_cfg_write(4, 6, 2);
    wr(0x100, BASE);
    wr(0x104, BASE >> 32);
    wr(0x108, BASE + 1024);
    wr(0x10c, BASE >> 32);
    wr(0x118, 0x11);
    command(0, 0x35, 1, 1, true);
    platform_ahci_poll();
    assert(!calls && (reg(0x120) & 1));
    platform_ahci_cleanup();
    assert(!allocations && !platform_ahci_present());
    block.sectors = 0;
    assert(!platform_ahci_init(&block, 1, BAR, 20, irq, &level, NULL, NULL));
    block.sectors = (UINT64_C(1) << 48) + 1;
    assert(!platform_ahci_init(&block, 1, BAR, 20, irq, &level, NULL, NULL));
    block.sectors = SECTORS;
    block.sector_bytes = 8192;
    assert(!platform_ahci_init(&block, 1, BAR, 20, irq, &level, NULL, NULL));
    block.sector_bytes = 2048;
    block.read_only = false;
    assert(!platform_ahci_init(&block, 1, BAR, 20, irq, &level, NULL, NULL));
    multiport();
    puts("guest AHCI: passed");
}
