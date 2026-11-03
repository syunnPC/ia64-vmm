/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/User/System/AhciPort.hpp
 */
/* Polled AHCI subset for the VMM medium: one port with one ATA disk,
 * command slot 0, IDENTIFY DEVICE, READ/WRITE DMA EXT, FLUSH CACHE EXT and
 * STANDBY IMMEDIATE (AHCI 1.3.1 §4.2, §5.6, §6.2.2.1, §10.1-§10.6; ACS-3).
 * DMA stays in VMM-owned RAM. Apache-2.0 text: LICENSES/Apache-2.0.txt. */
#include "host/amd64/amd64.h"
#include "host/amd64/plog_ahci.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#if !defined(IA64VMM_AMD64_HOST_ONLY)
#define PAGES 4u /* data buffer: 32 sectors per command */
#define COMMAND_SECONDS 30u
#define READY_SECONDS 31u /* ATA: BSY clears within 31 s of a reset */
enum { CAP = 0, GHC = 4, PI = 0xc, VS = 0x10, CAP2 = 0x24, BOHC = 0x28 };
enum {
    CLB = 0,
    CLBU = 4,
    FB = 8,
    FBU = 0xc,
    PIS = 0x10,
    CMD = 0x18,
    TFD = 0x20,
    SIG = 0x24,
    SSTS = 0x28,
    SCTL = 0x2c,
    SERR = 0x30,
    CI = 0x38
};
enum {
    ST = 1,
    SUD = 2,
    POD = 4,
    FRE = 16,
    FR = 1u << 14,
    CR = 1u << 15,
    CPD = 1u << 20,
    ICC = 0xfu << 28
};
enum { TFES = 1u << 30, HBFS = 1u << 29, HBDS = 1u << 28, IFS = 1u << 27 };
/* page: the command list at 0 (1 KiB aligned), the received FIS area at
 * 400h (256-byte aligned), slot 0's command table at 500h (128-byte
 * aligned) with its PRDT at 580h. */
static struct {
    uint8_t *mmio, *page, *data;
    uint64_t page_pa, data_pa[PAGES];
    unsigned port, base;
    bool live, s64a, receiving;
} hba;
static const char *who = "PLOG";
static bool quiet;
static struct plog_block block;
static uint32_t reg(unsigned off)
{
    return *(volatile uint32_t *)(hba.mmio + off);
}
static void wr(unsigned off, uint32_t value)
{
    *(volatile uint32_t *)(hba.mmio + off) = value;
}
static uint32_t preg(unsigned off)
{
    return reg(hba.base + off);
}
static void pwr(unsigned off, uint32_t value)
{
    wr(hba.base + off, value);
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
/* Bounded wait for (register & mask) == value; all-ones is an HBA that is gone. */
static bool wait_reg(unsigned off, uint32_t mask, uint32_t value, unsigned ms)
{
    uint64_t end = hal_counter() + hal_counter_hz() / 1000 * ms;
    do {
        uint32_t n = reg(off);
        if (n == UINT32_MAX)
            return false;
        if ((n & mask) == value)
            return true;
        __asm__ volatile("pause");
    } while (hal_counter() < end);
    return false;
}
/* Without CAP.S64A every byte the HBA reads or writes lies below 4 GiB. */
static void *dma_pages(unsigned pages)
{
    uint8_t *p = hal_alloc_pages(pages, HAL_PAGE_ZERO);
    if (!p) {
        LOGE("%s AHCI DMA allocation failed", who);
        return NULL;
    }
    for (unsigned i = 0; i < pages; ++i)
        if (!hba.s64a && physical(p + i * 4096) > UINT32_MAX - 4095) {
            LOGE("%s AHCI without 64-bit addressing; low-memory DMA allocation unavailable", who);
            return NULL;
        }
    return p;
}
/* §10.4.3: AE, then HR until it reads 0 within 1 s; the reset clears AE
 * again (CAP.SAM 0) and leaves GHC.IE clear. */
static bool hba_reset(void)
{
    wr(GHC, 1u << 31);
    wr(GHC, 1u << 31 | 1);
    if (!wait_reg(GHC, 1, 0, 1000)) {
        LOGE("%s AHCI HBA reset timeout GHC=%x", who, reg(GHC));
        return false;
    }
    wr(GHC, 1u << 31);
    return true;
}
/* §10.1.2: idle the port, ST then CR, FRE then FR, within 500 ms each;
 * when CR or FR stays set, the HBA reset (every port is the VMM's). The
 * backend stops; DMA storage of an unfinished command is never reused. */
static void stop(void)
{
    hba.live = false;
    uint32_t cmd = preg(CMD);
    if (cmd == UINT32_MAX)
        return;
    bool idle = true;
    if (cmd & (ST | CR)) {
        pwr(CMD, cmd & ~(ST | ICC));
        idle = wait_reg(hba.base + CMD, CR, 0, 500);
    }
    if (idle && (preg(CMD) & (FRE | FR))) {
        pwr(CMD, preg(CMD) & ~(FRE | ICC));
        idle = wait_reg(hba.base + CMD, FR, 0, 500);
    }
    if (!idle && preg(CMD) != UINT32_MAX && !log_in_panic()) {
        LOGE("%s AHCI port %u did not stop CMD=%x; HBA reset", who, hba.port, preg(CMD));
        (void)hba_reset();
    }
    hba.receiving = false;
}
/* §10.3.1: a functional device, BSY and DRQ clear with PxSSTS.DET 3h. */
static bool ready(void)
{
    uint64_t end = hal_counter() + hal_counter_hz() * READY_SECONDS;
    do {
        if (log_in_panic())
            return false;
        uint32_t tfd = preg(TFD), ssts = preg(SSTS);
        if (tfd == UINT32_MAX || ssts == UINT32_MAX)
            return false;
        if (!(tfd & 0x88) && (ssts & 15) == 3)
            return true;
        __asm__ volatile("pause");
    } while (hal_counter() < end);
    LOGE("%s AHCI port %u not ready TFD=%x SSTS=%x", who, hba.port, preg(TFD), preg(SSTS));
    return false;
}
/* §10.1.2 step 5, then ST (FRE is already set). */
static void run(void)
{
    pwr(SERR, UINT32_MAX);
    pwr(PIS, UINT32_MAX);
    pwr(CMD, (preg(CMD) & ~ICC) | ST);
}
/* §6.2.2.1 after a task-file error: ST cleared (it also clears PxCI), CR
 * waited for, PxSERR and PxIS cleared, a COMRESET (§10.4.2) when BSY or
 * DRQ is still set, then ST again. Failing that, the backend stops; after
 * a panic nothing more is sent. */
static void recover(void)
{
    pwr(CMD, preg(CMD) & ~(ST | ICC));
    if (!wait_reg(hba.base + CMD, CR, 0, 500)) {
        LOGE("%s AHCI port %u recovery: CR stuck", who, hba.port);
        stop();
        return;
    }
    pwr(SERR, UINT32_MAX);
    pwr(PIS, UINT32_MAX);
    if (log_in_panic()) {
        hba.live = false;
        return;
    }
    if (preg(TFD) & 0x88) {
        uint32_t sctl = preg(SCTL) & ~15u;
        pwr(SCTL, sctl | 1);
        pause_ms(1);
        pwr(SCTL, sctl);
    }
    if (!ready()) {
        if (log_in_panic())
            hba.live = false;
        else
            stop();
        return;
    }
    run();
}
/* §4.2.2/§4.2.3: command header 0 (CFL 5 dwords, W, PRDTL), a Register
 * Host to Device FIS with C set, one PRD per data page; issue through
 * PxCI and poll. Completion: PxCI bit 0 clear, PRDBC the expected count. */
static bool command(uint8_t opcode, uint8_t feature, uint8_t device, uint64_t lba, unsigned count,
                    unsigned bytes, bool write)
{
    if (!hba.live || log_in_panic())
        return false;
    uint32_t *header = (uint32_t *)hba.page;
    uint8_t *table = hba.page + 0x500;
    unsigned entries = (bytes + 4095) / 4096;
    memset(table, 0, 0x80 + PAGES * 16);
    table[0] = 0x27;
    table[1] = 0x80;
    table[2] = opcode;
    table[3] = feature;
    table[7] = device;
    for (unsigned i = 0; i < 3; ++i) {
        table[4 + i] = (uint8_t)(lba >> (8 * i));
        table[8 + i] = (uint8_t)(lba >> (8 * (i + 3)));
    }
    table[12] = (uint8_t)count;
    table[13] = (uint8_t)(count >> 8);
    for (unsigned i = 0; i < entries; ++i) {
        uint32_t *prd = (uint32_t *)(table + 0x80 + i * 16);
        unsigned n = bytes - i * 4096 < 4096 ? bytes - i * 4096 : 4096;
        prd[0] = (uint32_t)hba.data_pa[i];
        prd[1] = (uint32_t)(hba.data_pa[i] >> 32);
        prd[3] = n - 1;
    }
    uint64_t ctba = hba.page_pa + 0x500;
    header[0] = 5 | (write ? 1u << 6 : 0) | entries << 16;
    header[1] = 0;
    header[2] = (uint32_t)ctba;
    header[3] = (uint32_t)(ctba >> 32);
    __asm__ volatile("mfence" ::: "memory");
    pwr(PIS, UINT32_MAX);
    pwr(CI, 1);
    uint64_t end = hal_counter() + hal_counter_hz() * COMMAND_SECONDS;
    do {
        if (log_in_panic()) {
            hba.live = false;
            return false;
        }
        uint32_t is = preg(PIS), ci = preg(CI);
        if (is == UINT32_MAX || ci == UINT32_MAX)
            break;
        if (is & (HBFS | HBDS | IFS)) {
            LOGE("%s AHCI port %u command=%x fatal PxIS=%x", who, hba.port, opcode, is);
            stop();
            return false;
        }
        if (is & TFES) {
            LOGE("%s AHCI port %u command=%x TFD=%x", who, hba.port, opcode, preg(TFD));
            recover();
            return false;
        }
        if (!(ci & 1)) {
            uint32_t done = __atomic_load_n(&header[1], __ATOMIC_ACQUIRE), tfd = preg(TFD);
            if (!(tfd & 0x89) && done == bytes)
                return true;
            LOGE("%s AHCI port %u command=%x TFD=%x transferred=%u of %u", who, hba.port, opcode,
                 tfd, done, bytes);
            if (tfd & 0x89)
                recover();
            return false;
        }
        __asm__ volatile("pause");
    } while (hal_counter() < end);
    LOGE("%s AHCI port %u no completion command=%x CI=%x TFD=%x", who, hba.port, opcode, preg(CI),
         preg(TFD));
    stop();
    return false;
}
/* ACS-3 READ DMA EXT 25h, WRITE DMA EXT 35h: LBA mode, 48-bit address. */
static bool transfer(bool write, uint64_t lba, unsigned count, uint8_t *data)
{
    if (!hba.live || lba >= block.sectors || count > block.sectors - lba)
        return false;
    while (count) {
        unsigned n = count < PAGES * 8 ? count : PAGES * 8, bytes = n * 512;
        if (write)
            memcpy(hba.data, data, bytes);
        if (!command(write ? 0x35 : 0x25, 0, 0x40, lba, n, bytes, write))
            return false;
        if (!write)
            memcpy(data, hba.data, bytes);
        lba += n;
        count -= n;
        data += bytes;
    }
    return true;
}
static bool read_blocks(uint64_t lba, unsigned count, void *data)
{
    return transfer(false, lba, count, data);
}
static bool write_blocks(uint64_t lba, unsigned count, const void *data)
{
    return transfer(true, lba, count, (uint8_t *)data);
}
/* FLUSH CACHE EXT EAh, also with the volatile write cache disabled. */
static bool flush(void)
{
    return command(0xea, 0, 0, 0, 0, 0, false);
}
/* §10.6.3: OS Ownership; the BIOS clears BOS within 25 ms, or sets BB and
 * then has 2 s to finish its outstanding commands. CAP2 (AHCI 1.2) reads 0
 * where it is reserved. */
static bool handoff(void)
{
    if (!(reg(CAP2) & 1))
        return true;
    uint32_t bohc = reg(BOHC);
    if (bohc == UINT32_MAX)
        return false;
    wr(BOHC, (bohc & ~8u) | 2); /* OOC is write-1-to-clear */
    uint64_t hz = hal_counter_hz(), end = hal_counter() + hz / 40;
    bool busy = false;
    for (;;) {
        bohc = reg(BOHC);
        if (bohc == UINT32_MAX)
            return false;
        if (!(bohc & 1))
            return true;
        uint64_t now = hal_counter();
        if ((bohc & 16) && !busy) {
            busy = true;
            end = now + hz * 2;
        }
        if (now >= end)
            break;
        __asm__ volatile("pause");
    }
    LOGE("%s AHCI BIOS kept the HBA BOHC=%x", who, bohc);
    return false;
}
/* §10.10.1: with CAP.SSS one port at a time, PxCMD.SUD and then device
 * presence (PxSSTS.DET not 0h) or 20 ms (SATA OOB: COMINIT within 10 ms);
 * cold presence power first. Then 1 s for every link to settle: exactly
 * one implemented port with a device (DET 3h) and none still establishing
 * communication (1h). */
static bool select_port(uint32_t pi, bool staggered)
{
    for (unsigned p = 0; p < 32; ++p)
        if (pi >> p & 1) {
            unsigned off = 0x100 + p * 0x80;
            uint32_t cmd = reg(off + CMD),
                     want = cmd | (staggered ? SUD : 0) | (cmd & CPD ? POD : 0);
            if (want == cmd)
                continue;
            wr(off + CMD, want & ~ICC);
            uint64_t end = hal_counter() + hal_counter_hz() / 50;
            while (staggered && !(reg(off + SSTS) & 15) && hal_counter() < end)
                __asm__ volatile("pause");
        }
    pause_ms(1000);
    unsigned present = 0, found = 0;
    bool pending = false;
    for (unsigned p = 0; p < 32; ++p)
        if (pi >> p & 1) {
            uint32_t det = reg(0x100 + p * 0x80 + SSTS);
            if (det == UINT32_MAX)
                return false;
            if ((det & 15) == 3) {
                ++present;
                found = p;
            } else if ((det & 15) == 1)
                pending = true;
        }
    if (present > 1 || (present && pending)) {
        LOGE("%s AHCI requires one attached disk; %u port(s) with a link%s", who, present,
             pending ? ", one still linking" : "");
        return false;
    }
    if (!present) {
        if (quiet)
            LOGI("%s: no disk on the reserved AHCI controller", who);
        else
            LOGE("%s AHCI: no disk with an established link", who);
        return false;
    }
    hba.port = found;
    hba.base = 0x100 + found * 0x80;
    return true;
}
static void text(char *out, const uint16_t *words, unsigned n)
{
    unsigned end = 0;
    for (unsigned i = 0; i < n * 2; ++i) {
        uint8_t c = (uint8_t)(words[i / 2] >> (i & 1 ? 0 : 8)); /* ATA strings: first byte high */
        out[i] = c >= 0x20 && c < 0x7f ? (char)c : '?';
        if (c != ' ')
            end = i + 1;
    }
    out[end] = 0;
}
/* ACS-3 IDENTIFY DEVICE. A device in Power-Up In Standby (word 2 37C8h or
 * 738Ch) spins up on SET FEATURES 07h, then answers again; an incomplete
 * response (word 0 bit 2) is refused. */
static bool identify(void)
{
    const uint16_t *w = (const uint16_t *)hba.data;
    if (!command(0xec, 0, 0, 0, 0, 512, false))
        return false;
    if ((w[2] == 0x37c8 || w[2] == 0x738c) &&
        (!command(0xef, 7, 0, 0, 0, 0, false) || !command(0xec, 0, 0, 0, 0, 512, false)))
        return false;
    if (w[0] & 4) {
        LOGE("%s AHCI port %u IDENTIFY DEVICE incomplete (word 2 %x)", who, hba.port, w[2]);
        return false;
    }
    return true;
}
static const struct plog_block *start(const struct pci_device *d)
{
    const struct pci_bar *bar = &d->bars[5];
    if (bar->io || bar->size < 0x180) {
        LOGE("%s AHCI ABAR (BAR5) unusable", who);
        return NULL;
    }
    hba.mmio = platform_pci_map_physical(bar->base, bar->size);
    if (!hba.mmio)
        return NULL;
    /* The firmware's command register stays while the BIOS may own the
     * HBA (memory decode added if it is off); then bus mastering waits
     * for the reset of any command lists the firmware left running. */
    if (!(d->command & 2))
        platform_pci_host_write(d->bdf, 4, d->command | 2, 2);
    if (!handoff())
        return NULL;
    platform_pci_host_write(d->bdf, 4, (d->command & ~4u) | 2, 2);
    /* §3.1.2: GHC.AE before the other AHCI registers. */
    wr(GHC, 1u << 31);
    uint32_t cap = reg(CAP), pi = reg(PI), version = reg(VS);
    unsigned last = 0;
    for (unsigned p = 0; p < 32; ++p)
        if (pi >> p & 1)
            last = p;
    if (cap == UINT32_MAX || pi == UINT32_MAX || !pi || version < 0x905 ||
        0x180 + last * 0x80 > bar->size) {
        LOGE("%s AHCI HBA unsupported CAP=%x PI=%x VS=%x", who, cap, pi, version);
        return NULL;
    }
    hba.s64a = cap >> 31;
    if (!hba_reset())
        return NULL;
    platform_pci_host_write(d->bdf, 4, d->command | 6, 2);
    hba.page = dma_pages(1);
    hba.data = hba.page ? dma_pages(PAGES) : NULL;
    if (!hba.data)
        return NULL;
    hba.page_pa = physical(hba.page);
    for (unsigned i = 0; i < PAGES; ++i)
        hba.data_pa[i] = physical(hba.data + i * 4096);
    if (!select_port(pi, cap >> 27 & 1))
        return NULL;
    if (preg(CMD) & (ST | CR | FRE | FR)) {
        LOGE("%s AHCI port %u not idle after the reset", who, hba.port);
        return NULL;
    }
    pwr(CLB, (uint32_t)hba.page_pa);
    pwr(CLBU, (uint32_t)(hba.page_pa >> 32));
    pwr(FB, (uint32_t)(hba.page_pa + 0x400));
    pwr(FBU, (uint32_t)((hba.page_pa + 0x400) >> 32));
    pwr(CMD, (preg(CMD) & ~ICC) | FRE);
    hba.receiving = true;
    if (!ready())
        return NULL;
    uint32_t signature = preg(SIG);
    if (signature != 0x101) {
        LOGE("%s AHCI port %u signature %x: ATA disks only (no ATAPI or port multiplier)", who,
             hba.port, signature);
        return NULL;
    }
    run();
    hba.live = true;
    if (!identify())
        return NULL;
    /* Word 0 bit 15 clear (ATA); word 83 valid with 48-bit addresses (bit
     * 10) and FLUSH CACHE EXT (bit 13); words 100-103 the capacity; word
     * 106 with bit 12 gives the logical sector size in words 117-118. */
    const uint16_t *w = (const uint16_t *)hba.data;
    uint64_t sectors =
        w[100] | (uint64_t)w[101] << 16 | (uint64_t)w[102] << 32 | (uint64_t)w[103] << 48;
    bool wide_sectors = (w[106] & 0xd000) == 0x5000 && (w[117] | (uint32_t)w[118] << 16) != 256;
    char model[41];
    text(model, w + 27, 20);
    if ((w[0] & 0x8000) || (w[83] & 0xc000) != 0x4000 || !(w[83] & 0x0400) || !(w[83] & 0x2000) ||
        !sectors || sectors >> 48 || wide_sectors) {
        LOGE("%s AHCI port %u '%s' requires 48-bit LBA, FLUSH CACHE EXT and 512-byte sectors", who,
             hba.port, model);
        return NULL;
    }
    bool cache = (w[87] & 0xc000) == 0x4000 && (w[85] & 0x20);
    block = (struct plog_block){sectors, read_blocks, write_blocks, flush};
    LOGI("%s AHCI port=%u '%s' VS=%x sectors=%llu write-cache=%s", who, hba.port, model, version,
         (unsigned long long)sectors, cache ? "on" : "off");
    return &block;
}
void amd64_plog_ahci_shutdown(bool standby)
{
    if (!hba.live || log_in_panic())
        return;
    bool ok = flush() && (!standby || command(0xe0, 0, 0, 0, 0, 0, false));
    hba.live = false;
    if (ok)
        LOGI("%s AHCI %s complete", who, standby ? "standby" : "flush");
    else
        LOGE("%s AHCI shutdown %s failed", who, standby ? "standby" : "flush");
}
const struct plog_block *amd64_plog_ahci(const struct pci_device *d, bool log)
{
    who = log ? "PLOG" : "NVRAM";
    quiet = !log;
    const struct plog_block *b = start(d);
    /* Failed after FRE: idle the port; its DMA pages stay. */
    if (!b && hba.receiving)
        stop();
    hba.live = b != NULL;
    return b;
}

#endif
