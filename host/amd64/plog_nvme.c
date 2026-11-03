/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Polled NVMe subset for the VMM medium: one admin and one I/O queue pair,
 * Identify, Write, Read and Flush on namespace 1 (NVMe 1.4 §3.1, §4.3, §4.6,
 * §5, §6, §7.6.1). One command is outstanding at a time. DMA stays in
 * VMM-owned RAM. */
#include "host/amd64/amd64.h"
#include "host/amd64/plog_nvme.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#if !defined(IA64VMM_AMD64_HOST_ONLY)
#define ENTRIES 16u /* per queue, at most CAP.MQES + 1 */
#define PAGES 4u    /* data buffer */
#define COMMAND_SECONDS 30u
enum { CAP = 0, VS = 8, CC = 0x14, CSTS = 0x1c, AQA = 0x24, ASQ = 0x28, ACQ = 0x30 };
static struct {
    uint8_t *mmio, *data;
    uint32_t *sq[2], *cq[2];
    uint64_t sq_pa[2], cq_pa[2], data_pa[PAGES], list_pa, ready_ticks, shutdown_ticks;
    unsigned stride, entries, tail[2], head[2], phase[2], max_sectors;
    uint16_t cid;
    bool live, enabled;
} nv;
static const char *who = "PLOG";
static struct plog_block block;
static uint32_t reg(unsigned off)
{
    return *(volatile uint32_t *)(nv.mmio + off);
}
static void wr(unsigned off, uint32_t value)
{
    *(volatile uint32_t *)(nv.mmio + off) = value;
}
/* 64-bit registers as two dword accesses, the low dword first. */
static uint64_t reg64(unsigned off)
{
    uint64_t low = reg(off);
    return low | (uint64_t)reg(off + 4) << 32;
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
/* §3.1.24/§3.1.25: SQyTDBL is doorbell 2y, CQyHDBL doorbell 2y + 1. */
static unsigned bell(unsigned index)
{
    return 0x1000 + index * nv.stride;
}
/* CSTS.RDY reaching ready within CAP.TO; CSTS.CFS ends a wait for ready. */
static bool wait_ready(bool ready)
{
    uint64_t end = hal_counter() + nv.ready_ticks;
    do {
        uint32_t csts = reg(CSTS);
        if (csts == UINT32_MAX || (ready && (csts & 2)))
            return false;
        if ((csts & 1) == ready)
            return true;
        __asm__ volatile("pause");
    } while (hal_counter() < end);
    return false;
}
/* §7.3.2 controller reset by CC.EN 1 -> 0, with CC.SHN cleared. §3.1.5:
 * clearing EN while CSTS.RDY is 0 has undefined results; a controller
 * still becoming ready is waited for first. */
static bool disable(void)
{
    uint32_t cc = reg(CC);
    if (cc == UINT32_MAX)
        return false;
    if (cc & 1) {
        (void)wait_ready(true);
        wr(CC, cc & ~(1u | 3u << 14));
    }
    nv.enabled = false;
    return wait_ready(false);
}
static void stop(void)
{
    nv.live = false; /* Timed-out DMA storage is retained; never reused. */
    if (nv.enabled && !disable())
        LOGE("%s NVMe controller reset timeout CSTS=%x", who, reg(CSTS));
}
/* §4.6: a new completion's phase tag differs from the previous pass's;
 * the expected tag starts at 1 and is inverted at each wrap. */
static bool execute(unsigned q, uint32_t *c, uint32_t *result)
{
    if (!nv.live || log_in_panic())
        return false;
    /* §4.2: CID FFFFh marks errors not tied to a command (§5.14.1.1). */
    if (++nv.cid == 0xffff)
        nv.cid = 0;
    uint16_t cid = nv.cid;
    c[0] |= (uint32_t)cid << 16;
    memcpy(nv.sq[q] + nv.tail[q] * 16, c, 64);
    if (++nv.tail[q] == nv.entries)
        nv.tail[q] = 0;
    __asm__ volatile("mfence" ::: "memory");
    wr(bell(2 * q), nv.tail[q]);
    uint64_t end = hal_counter() + hal_counter_hz() * COMMAND_SECONDS;
    do {
        if (log_in_panic()) {
            nv.live = false;
            return false;
        }
        uint32_t *e = nv.cq[q] + nv.head[q] * 4;
        uint32_t status = __atomic_load_n(&e[3], __ATOMIC_ACQUIRE);
        if ((status >> 16 & 1) != nv.phase[q]) {
            uint32_t csts = reg(CSTS);
            if (csts == UINT32_MAX || (csts & 2))
                break;
            __asm__ volatile("pause");
            continue;
        }
        uint32_t value = e[0], queue = e[2] >> 16;
        if (++nv.head[q] == nv.entries) {
            nv.head[q] = 0;
            nv.phase[q] ^= 1;
        }
        wr(bell(2 * q + 1), nv.head[q]);
        if ((status & 0xffff) != cid || queue != q) {
            LOGE("%s NVMe completion cid=%u sq=%u, expected cid=%u sq=%u", who, status & 0xffff,
                 queue, cid, q);
            stop();
            return false;
        }
        /* §4.6.1: SC and SCT zero is success. */
        if (status >> 17 & 0x7ff) {
            LOGE("%s NVMe opcode=%x queue=%u status SCT=%u SC=%x", who, c[0] & 255, q,
                 status >> 25 & 7, status >> 17 & 255);
            return false;
        }
        if (result)
            *result = value;
        return true;
    } while (hal_counter() < end);
    LOGE("%s NVMe no completion opcode=%x queue=%u CSTS=%x", who, c[0] & 255, q, reg(CSTS));
    stop();
    return false;
}
/* §4.3: the buffer's first page in PRP1; a second page in PRP2, or more
 * pages through the PRP list. */
static void prp(uint32_t *c, unsigned bytes)
{
    uint64_t second = bytes > 8192 ? nv.list_pa : bytes > 4096 ? nv.data_pa[1] : 0;
    c[6] = (uint32_t)nv.data_pa[0];
    c[7] = (uint32_t)(nv.data_pa[0] >> 32);
    c[8] = (uint32_t)second;
    c[9] = (uint32_t)(second >> 32);
}
/* §6.9 Read, §6.15 Write: SLBA in CDW10-11, 0's based NLB in CDW12. */
static bool transfer(bool write, uint64_t lba, unsigned count, uint8_t *data)
{
    if (!nv.live || lba >= block.sectors || count > block.sectors - lba)
        return false;
    while (count) {
        unsigned n = count < nv.max_sectors ? count : nv.max_sectors, bytes = n * 512;
        uint32_t c[16] = {write ? 1u : 2u, 1};
        prp(c, bytes);
        c[10] = (uint32_t)lba;
        c[11] = (uint32_t)(lba >> 32);
        c[12] = n - 1;
        if (write)
            memcpy(nv.data, data, bytes);
        if (!execute(1, c, NULL))
            return false;
        if (!write)
            memcpy(data, nv.data, bytes);
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
/* §6.8: mandatory in the NVM command set, with or without a volatile write cache. */
static bool flush(void)
{
    uint32_t c[16] = {0, 1};
    return execute(1, c, NULL);
}
static bool identify(unsigned cns, uint32_t nsid)
{
    uint32_t c[16] = {6, nsid};
    c[10] = cns;
    prp(c, 4096);
    return execute(0, c, NULL);
}
static bool create(unsigned opcode, uint64_t pa, uint32_t cdw11)
{
    uint32_t c[16] = {opcode};
    c[6] = (uint32_t)pa;
    c[7] = (uint32_t)(pa >> 32);
    c[10] = (nv.entries - 1) << 16 | 1;
    c[11] = cdw11;
    return execute(0, c, NULL);
}
static void text(char *out, const uint8_t *in, unsigned n)
{
    unsigned end = 0;
    for (unsigned i = 0; i < n && in[i]; ++i) {
        out[i] = in[i] >= 0x20 && in[i] < 0x7f ? (char)in[i] : '?';
        if (in[i] != ' ')
            end = i + 1;
    }
    out[end] = 0;
}
static const struct plog_block *start(const struct pci_device *d)
{
    if (d->bars[0].io || d->bars[0].size < 0x1010) {
        LOGE("%s NVMe BAR0 unusable", who);
        return NULL;
    }
    nv.mmio = platform_pci_map_physical(d->bars[0].base, d->bars[0].size);
    if (!nv.mmio)
        return NULL;
    /* Completions are polled. The admin completion queue has no interrupt
     * enable (§7.5), so INTx, MSI and MSI-X are all turned off. Bus
     * mastering waits for the reset of any queues the firmware left. */
    platform_pci_host_write(d->bdf, 4, (d->command & ~4u) | 0x402, 2);
    if (d->msi_offset)
        platform_pci_host_write(d->bdf, d->msi_offset + 2,
                                platform_pci_host_read(d->bdf, d->msi_offset + 2, 2) & ~1u, 2);
    if (d->msix_offset)
        platform_pci_host_write(d->bdf, d->msix_offset + 2,
                                platform_pci_host_read(d->bdf, d->msix_offset + 2, 2) & ~0x8000u,
                                2);
    uint64_t cap = reg64(CAP);
    uint32_t version = reg(VS);
    unsigned mqes = cap & 0xffff;
    nv.stride = 4u << (cap >> 32 & 15);
    /* §3.1.1: CAP.CSS bit 0 (NVM command set), CAP.MPSMIN 4 KiB; MQES 0 is invalid. */
    if (cap == UINT64_MAX || version == UINT32_MAX || !mqes || !(cap >> 37 & 1) ||
        (cap >> 48 & 15) || version < 0x10000 || 0x1000 + 4 * nv.stride > d->bars[0].size) {
        LOGE("%s NVMe controller unsupported CAP=%llx VS=%x", who, (unsigned long long)cap,
             version);
        return NULL;
    }
    nv.entries = mqes + 1 < ENTRIES ? mqes + 1 : ENTRIES;
    /* CAP.TO in 500 ms units, plus one unit. */
    nv.ready_ticks = hal_counter_hz() / 2 * ((cap >> 24 & 255) + 1);
    uint8_t *pages = hal_alloc_pages(5 + PAGES, HAL_PAGE_ZERO);
    if (!pages) {
        LOGE("%s NVMe DMA allocation failed", who);
        return NULL;
    }
    for (unsigned q = 0; q < 2; ++q) {
        nv.sq[q] = (uint32_t *)(pages + q * 8192);
        nv.cq[q] = (uint32_t *)(pages + q * 8192 + 4096);
        nv.sq_pa[q] = physical(nv.sq[q]);
        nv.cq_pa[q] = physical(nv.cq[q]);
        nv.tail[q] = nv.head[q] = 0;
        nv.phase[q] = 1;
    }
    uint64_t *list = (uint64_t *)(pages + 4 * 4096);
    nv.list_pa = physical(list);
    nv.data = pages + 5 * 4096;
    for (unsigned i = 0; i < PAGES; ++i)
        nv.data_pa[i] = physical(nv.data + i * 4096);
    for (unsigned i = 1; i < PAGES; ++i)
        list[i - 1] = nv.data_pa[i];
    /* §7.6.1: reset, admin queues, configuration, enable. */
    if (!disable()) {
        LOGE("%s NVMe controller reset timeout CSTS=%x", who, reg(CSTS));
        return NULL;
    }
    platform_pci_host_write(d->bdf, 4, d->command | 0x406, 2);
    wr(AQA, (nv.entries - 1) << 16 | (nv.entries - 1));
    wr64(ASQ, nv.sq_pa[0]);
    wr64(ACQ, nv.cq_pa[0]);
    /* CC: EN, CSS NVM, MPS 4 KiB, AMS round robin, IOSQES 64 bytes, IOCQES 16 bytes. */
    wr(CC, 6u << 16 | 4u << 20 | 1);
    nv.enabled = true;
    if (!wait_ready(true)) {
        LOGE("%s NVMe controller not ready CSTS=%x", who, reg(CSTS));
        return NULL;
    }
    nv.live = true;
    if (!identify(1, 0))
        return NULL;
    /* §5.15 Identify Controller: MDTS in CAP.MPSMIN pages (0: no limit);
     * SQES and CQES bits 3:0 required, 7:4 maximum entry size (log2). */
    const uint8_t *id = nv.data;
    unsigned mdts = id[77], sqes = id[512], cqes = id[513];
    uint32_t rtd3e; /* µs, 0: not reported */
    memcpy(&rtd3e, id + 88, 4);
    uint64_t shutdown_us = !rtd3e ? 5000000 : rtd3e < 60000000 ? rtd3e : 60000000;
    nv.shutdown_ticks = hal_counter_hz() * shutdown_us / 1000000;
    char model[41];
    text(model, id + 24, 40);
    if ((sqes & 15) > 6 || sqes >> 4 < 6 || (cqes & 15) > 4 || cqes >> 4 < 4) {
        LOGE("%s NVMe entry sizes SQES=%x CQES=%x unsupported", who, sqes, cqes);
        return NULL;
    }
    nv.max_sectors = PAGES * 8;
    if (mdts && mdts < 16 && (8u << mdts) < nv.max_sectors)
        nv.max_sectors = 8u << mdts;
    if (!identify(0, 1))
        return NULL;
    /* Identify Namespace: NSZE; FLBAS bits 3:0 select the LBA format (MS, LBADS). */
    const uint8_t *ns = nv.data;
    uint64_t sectors;
    uint32_t format;
    memcpy(&sectors, ns, 8);
    memcpy(&format, ns + 128 + (ns[26] & 15) * 4, 4);
    if (!sectors || (ns[26] & 15) > ns[25] || (format >> 16 & 255) != 9 || (format & 0xffff)) {
        LOGE("%s requires NVMe namespace 1 with 512-byte LBAs and no metadata", who);
        return NULL;
    }
    /* §5.21.1.7 one I/O queue pair; §5.3 then §5.4, physically contiguous,
     * the completion queue without interrupts. */
    uint32_t features[16] = {9};
    features[10] = 7;
    if (!execute(0, features, NULL) || !create(5, nv.cq_pa[1], 1) ||
        !create(1, nv.sq_pa[1], 1u << 16 | 1))
        return NULL;
    block = (struct plog_block){sectors, read_blocks, write_blocks, flush};
    LOGI("%s NVMe '%s' VS=%x namespace 1 sectors=%llu queue=%u transfer=%u", who, model, version,
         (unsigned long long)sectors, nv.entries, nv.max_sectors * 512);
    return &block;
}
/* §7.6.2 normal shutdown: delete the I/O queues, then CC.SHN 01b until
 * CSTS.SHST is 10b, within RTD3E when reported (at most 60 s), else 5 s. */
void amd64_plog_nvme_shutdown(void)
{
    if (!nv.enabled || !nv.live || log_in_panic())
        return;
    uint32_t sq[16] = {0}, cq[16] = {4};
    sq[10] = cq[10] = 1;
    (void)execute(0, sq, NULL);
    (void)execute(0, cq, NULL);
    nv.live = false;
    if (!nv.enabled)
        return;
    wr(CC, (reg(CC) & ~(3u << 14)) | 1u << 14);
    uint64_t end = hal_counter() + nv.shutdown_ticks;
    do {
        uint32_t csts = reg(CSTS);
        if (csts == UINT32_MAX)
            break;
        if ((csts >> 2 & 3) == 2) {
            LOGI("%s NVMe shutdown complete", who);
            return;
        }
        __asm__ volatile("pause");
    } while (hal_counter() < end);
    LOGE("%s NVMe shutdown timeout CSTS=%x", who, reg(CSTS));
}
const struct plog_block *amd64_plog_nvme(const struct pci_device *d, bool log)
{
    who = log ? "PLOG" : "NVRAM";
    const struct plog_block *b = start(d);
    nv.live = b != NULL;
    /* Failed after CC.EN: reset the controller; its DMA pages stay. */
    if (!b && nv.enabled) {
        if (disable())
            LOGI("%s NVMe controller disabled after the failed start", who);
        else
            LOGE("%s NVMe controller reset timeout CSTS=%x", who, reg(CSTS));
    }
    return b;
}

#endif
