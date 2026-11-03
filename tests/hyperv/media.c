/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real FAT32 identity discovery with a modeled synthetic SCSI transport. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "host/amd64/plog_hyperv.c"
#include "persist/fat32.h"

#define SECTORS 70000u
#define DEVICES 35u
static uint8_t disk[SECTORS * 512u], manifest[512];
#include "tests/persist/fat32-fixture.h"
static struct boot_info bi;
static struct amd64_storvsc_device devices[DEVICES];
static bool present[DEVICES], matches[DEVICES], detected, start_ok, attach_ok, io_ok, panic,
    enumerate_ok;
static unsigned order[DEVICES], damage[DEVICES], starts, enumerations, attaches, stops, reads,
    writes, flushes;
static size_t log_b_entry;
enum {
    CROSS_LINK = 1,
    DUPLICATE_PARTITION = 2,
    CORRUPT_BEFORE = 4,
    INVALID_BEFORE = 8,
    EARLY_IO = 16
};
static struct platform_block attached[32];
static const struct amd64_storvsc_device *last_device;
void vmm_log(enum log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}
int vmm_snprintf(char *out, size_t bytes, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(out, bytes, format, ap);
    va_end(ap);
    return n;
}
bool log_in_panic(void)
{
    return panic;
}
bool amd64_hyperv_detect(void)
{
    return detected;
}
bool amd64_storvsc_start(const uint8_t instance[16])
{
    assert(!instance);
    ++starts;
    return start_ok;
}
const struct amd64_storvsc_device *amd64_storvsc_find(unsigned target, unsigned lun)
{
    for (unsigned i = 0; i < DEVICES; ++i)
        if (present[i] && devices[i].target == target && devices[i].lun == lun)
            return devices + i;
    return NULL;
}
bool amd64_storvsc_enumerate(void)
{
    ++enumerations;
    return enumerate_ok;
}
unsigned amd64_storvsc_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < DEVICES; ++i)
        n += present[i];
    return n;
}
const struct amd64_storvsc_device *amd64_storvsc_device(unsigned index)
{
    for (unsigned i = 0; i < DEVICES; ++i)
        if (present[order[i]] && !index--)
            return devices + order[i];
    return NULL;
}
bool amd64_storvsc_read(const struct amd64_storvsc_device *d, uint64_t at, unsigned n, void *out)
{
    ++reads;
    last_device = d;
    assert(d >= devices && d < devices + DEVICES);
    if (!io_ok || at >= d->blocks || n > d->blocks - at)
        return false;
    unsigned fault = damage[d - devices];
    if ((fault & EARLY_IO) && at <= 1 && 1 - at < n)
        return false;
    assert(d->sector_bytes == 512);
    memcpy(out, disk + at * 512, n * 512);
    if (!at && (fault & DUPLICATE_PARTITION))
        memcpy((uint8_t *)out + 462, disk + 446, 16);
    if (!at && (fault & (CORRUPT_BEFORE | INVALID_BEFORE))) {
        uint8_t *mbr = out;
        memcpy(mbr + 462, disk + 446, 16);
        test_put32(mbr + 454, fault & INVALID_BEFORE ? SECTORS : 1);
        test_put32(mbr + 458, 1);
    }
    if ((fault & CROSS_LINK) && log_b_entry / 512 >= at && log_b_entry / 512 - at < n) {
        uint8_t *entry = (uint8_t *)out + log_b_entry - at * 512;
        unsigned cluster = test_cluster(TEST_MANIFEST + 4);
        test_put16(entry + 20, cluster >> 16);
        test_put16(entry + 26, cluster);
    }
    if (!matches[d - devices]) {
        for (unsigned file = 0; file < 3; ++file) {
            uint64_t lba = TEST_MANIFEST + file;
            if (lba >= at && lba - at < n)
                ((uint8_t *)out)[(lba - at) * 512 + 16] ^= 0x80;
        }
    }
    return true;
}
bool amd64_storvsc_write(const struct amd64_storvsc_device *d, uint64_t at, unsigned n,
                         const void *in)
{
    ++writes;
    last_device = d;
    (void)at;
    (void)n;
    (void)in;
    return io_ok;
}
bool amd64_storvsc_flush(const struct amd64_storvsc_device *d)
{
    ++flushes;
    last_device = d;
    return io_ok;
}
void amd64_storvsc_stop(void)
{
    ++stops;
}
bool platform_storage_attach(const struct platform_block *backing)
{
    if (!backing) {
        attaches = 0;
        return true;
    }
    if (!attach_ok)
        return false;
    assert(attaches < 32);
    attached[attaches++] = *backing;
    return true;
}
static void fixture_identity(const char *module, unsigned format)
{
    memset(manifest, 0, sizeof(manifest));
    bool nvram = !strcmp(module, "nvram");
    memcpy(manifest, nvram ? "NVRAMMAP" : "PLOGMAP1", 8);
    test_put32(manifest + 8, nvram ? 2 : 3);
    test_put32(manifest + 12, 512);
    manifest[16] = 0x5a;
    if (nvram) {
        test_put32(manifest + 32, format);
        test_put32(manifest + 36, 1);
    }
    test_put32(manifest + 508, plog_crc(manifest, 508));
    memcpy(disk + (TEST_MANIFEST + (nvram ? format ? 1 : 2 : 0)) * 512, manifest, 512);
    bi.module_count = 1;
    strcpy(bi.modules[0].name, module);
    bi.modules[0].hva = manifest;
    bi.modules[0].size = sizeof(manifest);
}
static void reset(const char *line)
{
    medium = NULL;
    guest_count = 0;
    connected = false;
    memset(&block, 0, sizeof(block));
    memset(attached, 0, sizeof(attached));
    memset(&bi, 0, sizeof(bi));
    detected = start_ok = attach_ok = io_ok = enumerate_ok = true;
    panic = false;
    starts = enumerations = attaches = stops = reads = writes = flushes = 0;
    last_device = NULL;
    const unsigned luns[] = {0, 3, 2, 1}, shuffled[] = {2, 0, 3, 1};
    for (unsigned i = 0; i < DEVICES; ++i) {
        present[i] = i < 4;
        matches[i] = i == 1;
        damage[i] = 0;
        order[i] = i < 4 ? shuffled[i] : i;
        devices[i] = (struct amd64_storvsc_device){
            .blocks = SECTORS, .sector_bytes = 512, .lun = i < 4 ? luns[i] : i + 4};
    }
    devices[2].sector_bytes = 2048;
    devices[2].readonly = true;
    devices[2].peripheral_type = 5;
    strcpy(bi.cmdline, line);
    fixture_identity("media-id", 0);
}
static const struct plog_block *select_media(const char *line)
{
    reset(line);
    return amd64_plog_hyperv(&bi);
}
int main(void)
{
    test_volume(SECTORS);
    test_file("persistence-manifest.bin", TEST_MANIFEST, 512);
    test_file("nvram-new.bin", TEST_MANIFEST + 1, 512);
    test_file("nvram-ref.bin", TEST_MANIFEST + 2, 512);
    test_file("log-a.bin", TEST_MANIFEST + 4, 512);
    log_b_entry = (size_t)(test_file("log-b.bin", TEST_MANIFEST + 5, 512) - disk);
    (void)test_fragment;
    const struct plog_block *p = select_media("");
    assert(p && medium == devices + 1 && starts == 1 && enumerations == 1 && attaches == 3);
    assert(!writes && !flushes && reads);
    assert(attached[0].context == devices && attached[1].context == devices + 3 &&
           attached[2].context == devices + 2);
    assert(attached[2].sector_bytes == 2048 && attached[2].read_only);
    uint8_t buffer[512];
    assert(p->read(0, 1, buffer) && last_device == devices + 1);
    assert(p->write(0, 1, buffer) && last_device == devices + 1 && writes == 1);
    assert(p->flush() && flushes == 1);
    assert(attached[0].write(attached[0].context, 0, 1, buffer) && last_device == devices);
    unsigned before = flushes;
    panic = true;
    amd64_plog_hyperv_shutdown();
    assert(!stops && flushes == before);
    panic = false;
    amd64_plog_hyperv_shutdown();
    assert(stops == 1 && flushes == before + 2);
    amd64_plog_hyperv_shutdown();
    assert(stops == 1);
    assert(select_media("vmm_media=auto guest_media=auto") && !writes && !flushes && attaches == 3);
    reset("");
    order[0] = 1;
    order[1] = 3;
    order[2] = 0;
    order[3] = 2;
    assert(amd64_plog_hyperv(&bi) && attached[0].context == devices &&
           attached[1].context == devices + 3);
    assert(select_media("vmm_media=hyperv:3 guest_media=hyperv:2") && attaches == 1 &&
           attached[0].read_only);
    assert(!select_media("vmm_media=hyperv:0") && !writes && !flushes && !attaches);
    const char *invalid[] = {"vmm_media=hyperv:",     "vmm_media=hyperv:64",
                             "vmm_media=pci:00:01.0", "vmm_media=hyperv:3 guest_media=hyperv:3",
                             "guest_media=hyperv:64", "guest_media=invalid"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!select_media(invalid[i]) && !attaches && !writes && !flushes);
    reset("");
    matches[1] = false;
    assert(!amd64_plog_hyperv(&bi) && !attaches);
    reset("");
    matches[0] = true;
    assert(!amd64_plog_hyperv(&bi) && !attaches && !writes && !flushes);
    reset("vmm_media=hyperv:3");
    matches[0] = true;
    assert(amd64_plog_hyperv(&bi) && attaches == 2);
    assert(attached[0].context == devices + 3 && attached[1].context == devices + 2);
    /* A boot-disk clone remains reserved despite unrelated allocation or
     * partition damage. Strict persistence validation must still reject it. */
    const unsigned damaged_clones[] = {CROSS_LINK, DUPLICATE_PARTITION, CORRUPT_BEFORE,
                                       INVALID_BEFORE};
    for (unsigned i = 0; i < sizeof(damaged_clones) / sizeof(damaged_clones[0]); ++i) {
        reset("");
        matches[0] = true;
        damage[0] = damaged_clones[i];
        assert(!amd64_plog_hyperv(&bi) && !attaches && !writes && !flushes);
        reset("vmm_media=hyperv:3");
        matches[0] = true;
        damage[0] = damaged_clones[i];
        assert(amd64_plog_hyperv(&bi) && attaches == 2 && !writes && !flushes);
        assert(attached[0].context == devices + 3 && attached[1].context == devices + 2);
        probing = devices;
        const struct plog_block readonly = {SECTORS, probe_read, NULL, NULL};
        assert(persist_identify(&readonly, "persistence-manifest.bin", manifest));
        const struct plog_block probe = {SECTORS, probe_read, write_blocks, flush};
        struct fat32 fs;
        struct fat32_file mf;
        assert(!persist_mount(&fs, &probe, "persistence-manifest.bin", manifest, &mf));
    }
    reset("");
    matches[0] = true;
    damage[0] = CORRUPT_BEFORE | EARLY_IO;
    assert(!amd64_plog_hyperv(&bi) && !attaches && !writes && !flushes);
    reset("vmm_media=hyperv:3 guest_media=hyperv:0");
    matches[0] = true;
    assert(!amd64_plog_hyperv(&bi) && !attaches && !writes && !flushes);
    reset("");
    matches[0] = true;
    devices[0].readonly = true;
    assert(!amd64_plog_hyperv(&bi) && !attaches);
    reset("");
    bi.module_count = 0;
    assert(!amd64_plog_hyperv(&bi) && !attaches);
    reset("vmm_media=hyperv:3 guest_media=hyperv:2");
    bi.module_count = 0;
    assert(amd64_plog_hyperv(&bi) && attaches == 1 && !writes && !flushes);
    reset("");
    manifest[508] ^= 1;
    assert(!amd64_plog_hyperv(&bi) && !starts);
    reset("");
    memset(manifest + 16, 0, 16);
    test_put32(manifest + 508, plog_crc(manifest, 508));
    assert(!amd64_plog_hyperv(&bi) && !starts);
    reset("");
    disk[TEST_MANIFEST * 512u + 16] ^= 1;
    assert(!amd64_plog_hyperv(&bi) && !attaches && !writes && !flushes);
    reset("");
    devices[1].readonly = true;
    assert(!amd64_plog_hyperv(&bi) && !attaches);
    reset("");
    devices[1].sector_bytes = 4096;
    assert(!amd64_plog_hyperv(&bi) && !attaches);
    reset("");
    devices[1].peripheral_type = 5;
    assert(!amd64_plog_hyperv(&bi) && !attaches);
    reset("");
    devices[0].sector_bytes = 4096;
    assert(amd64_plog_hyperv(&bi) && attaches == 3);
    assert(attached[0].sector_bytes == 4096);
    reset("");
    present[0] = present[2] = present[3] = false;
    assert(amd64_plog_hyperv(&bi) && !attaches);
    reset("");
    fixture_identity("persistence", 0);
    assert(amd64_plog_hyperv(&bi) && !writes && !flushes);
    for (unsigned format = 0; format < 2; ++format) {
        reset("");
        fixture_identity("nvram", format);
        assert(amd64_plog_hyperv(&bi) && !writes && !flushes);
    }
    reset("");
    fixture_identity("nvram", 0);
    test_put32(manifest + 36, 33);
    test_put32(manifest + 508, plog_crc(manifest, 508));
    assert(!amd64_plog_hyperv(&bi) && !starts);
    reset("");
    assert(amd64_hyperv_storage_start() && amd64_hyperv_storage_start());
    assert(amd64_plog_hyperv(&bi) && starts == 1);
    reset("");
    detected = false;
    assert(!amd64_plog_hyperv(&bi) && !starts);
    reset("");
    start_ok = false;
    assert(!amd64_plog_hyperv(&bi) && !enumerations);
    reset("");
    enumerate_ok = false;
    assert(!amd64_plog_hyperv(&bi) && !attaches);
    reset("");
    io_ok = false;
    assert(!amd64_plog_hyperv(&bi) && !writes && !flushes);
    reset("");
    attach_ok = false;
    assert(!amd64_plog_hyperv(&bi) && !attaches);
    reset("");
    for (unsigned i = 0; i < DEVICES; ++i)
        present[i] = true;
    assert(!amd64_plog_hyperv(&bi) && !attaches && !writes && !flushes);
    reset("");
    for (unsigned i = 0; i < 33; ++i)
        present[i] = true;
    assert(amd64_plog_hyperv(&bi) && attaches == 32 && !writes && !flushes);
    puts("Hyper-V media identity and selection tests passed");
}
