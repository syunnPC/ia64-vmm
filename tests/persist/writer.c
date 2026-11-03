/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FAT32 files; inactive commits,
 * crash retention, and no I/O after an ambiguous read/write/flush failure. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "persist/persist.h"
#include "persist/protocol.h"
#include "core/log_internal.h"
#define SECTORS 100000u
#define FIRST (TEST_DATA + 100u)
#define SECOND (FIRST + 4096u)
#define CRASH (SECOND + 4096u)
#define DIRECTORY TEST_DIRECTORY
#define MANIFEST TEST_MANIFEST
static unsigned char disk[SECTORS * 512];
#include "tests/persist/fat32-fixture.h"
static unsigned char *entries[4];
static struct log_ring ring;
static unsigned calls, fail_at, reads, fail_read, cpu, written;
static bool panic;
static uint64_t mock_now, protected_lba = UINT64_MAX, relocated_lba = UINT64_MAX;
static const char *crash_text;
uint64_t log_media_end(void)
{
    return ring.write_pos;
}
static bool read_block(uint64_t lba, unsigned n, void *out)
{
    assert(lba + n <= SECTORS);
    if (++reads == fail_read)
        return false;
    memcpy(out, disk + lba * 512, n * 512);
    return true;
}
static bool write_block(uint64_t lba, unsigned n, const void *in)
{
    ++calls;
    if (calls == fail_at)
        return false;
    assert((lba >= FIRST && lba + n <= SECOND) || (lba >= SECOND && lba + n <= CRASH) ||
           (lba >= CRASH && lba + n <= CRASH + 4096) || (lba == relocated_lba && n == 1));
    assert(protected_lba == UINT64_MAX || lba + n <= protected_lba || lba >= protected_lba + 4096);
    written += n;
    memcpy(disk + lba * 512, in, n * 512);
    return true;
}
static bool flush(void)
{
    return ++calls != fail_at;
}
void vmm_log(enum log_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
int vmm_snprintf(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int result = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    return result;
}
uint64_t hal_counter(void)
{
    return mock_now;
}
uint64_t hal_counter_hz(void)
{
    return 100;
}
unsigned hal_cpu_index(void)
{
    return cpu;
}
bool log_in_panic(void)
{
    return panic;
}
size_t log_media_read(uint64_t *cursor, void *data, size_t size, uint64_t *lost)
{
    *lost = 0;
    size_t n = ring.write_pos - *cursor;
    if (n > size)
        n = size;
    size_t first = LOG_RING_SIZE - *cursor % LOG_RING_SIZE;
    if (first > n)
        first = n;
    memcpy(data, ring.data + *cursor % LOG_RING_SIZE, first);
    memcpy((unsigned char *)data + first, ring.data, n - first);
    *cursor += n;
    return n;
}
void log_media_flushed(uint64_t position)
{
    ring.flushed_pos = position;
}
size_t log_crash_read(uint64_t *cursor, void *data, size_t size)
{
    size_t n = crash_text ? strlen(crash_text) : 0;
    if (n > size)
        n = size;
    if (n)
        memcpy(data, crash_text, n);
    *cursor += n;
    return n;
}
static void put64(void *out, uint64_t n)
{
    memcpy(out, &n, 8);
}
static void put32(void *out, uint32_t n)
{
    memcpy(out, &n, 4);
}
static uint64_t get64(const void *in)
{
    uint64_t n;
    memcpy(&n, in, 8);
    return n;
}
static void seal(unsigned char *m)
{
    put32(m + 508, plog_crc(m, 508));
    memcpy(disk + MANIFEST * 512, m, 512);
}
static void setup(struct boot_info *bi, unsigned char m[512])
{
    memset(disk, 0, sizeof(disk));
    memset(m, 0, 512);
    memset(bi, 0, sizeof(*bi));
    memcpy(m, "PLOGMAP1", 8);
    put32(m + 8, 3);
    put32(m + 12, 512);
    memcpy(m + 16, "unique image key", 16);
    test_volume(SECTORS);
    entries[0] = test_file("log-a.bin", FIRST, 2 * 1024 * 1024);
    entries[1] = test_file("log-b.bin", SECOND, 2 * 1024 * 1024);
    entries[2] = test_file("persistence-manifest.bin", MANIFEST, 512);
    entries[3] = test_file("crash.bin", CRASH, 2 * 1024 * 1024);
    seal(m);
    bi->module_count = 1;
    strcpy(bi->modules[0].name, "persistence");
    bi->modules[0].hva = m;
    bi->modules[0].size = 512;
    strcpy(bi->cmdline, "media_log=1");
    ring.write_pos = 6;
    ring.flushed_pos = 0;
    memcpy(ring.data, "hello\n", 6);
    calls = fail_at = reads = fail_read = cpu = written = 0;
    relocated_lba = UINT64_MAX;
    panic = false;
    mock_now = 100;
    crash_text = NULL;
    protected_lba = UINT64_MAX;
}
static void record(unsigned char *m, uint64_t lba, uint64_t gen, bool crash)
{
    unsigned char *h = disk + lba * 512;
    memset(h, 0, 512);
    memcpy(h, "PLOGREC1", 8);
    put32(h + 8, 1);
    put32(h + 12, crash);
    put64(h + 16, gen);
    put64(h + 24, 6);
    memcpy(h + 32, m + 16, 16);
    memcpy(h + 512, "older\n", 6);
    put32(h + 48, plog_crc(h + 512, 6));
    put32(h + 508, plog_crc(h, 508));
}
static void refused(struct boot_info *bi, const struct plog_block *b)
{
    plog_init(bi, b);
    plog_poll(true);
    assert(!calls && !ring.flushed_pos && !plog_save_ok() && !strcmp(plog_status(), "refused"));
}
int main(void)
{
    struct boot_info bi;
    unsigned char m[512];
    struct plog_block b = {SECTORS, read_block, write_block, flush};
    for (unsigned fail = 1; fail <= 5; ++fail) {
        setup(&bi, m);
        plog_init(&bi, &b);
        fail_at = fail;
        plog_poll(true);
        assert(!ring.flushed_pos && !plog_save_ok() && !strcmp(plog_status(), "stopped"));
        unsigned previous = calls;
        plog_poll(true);
        assert(calls == previous);
    }
    for (unsigned fail = 1; fail <= 5; ++fail) {
        setup(&bi, m);
        plog_init(&bi, &b);
        plog_poll(true);
        unsigned char old[1024];
        memcpy(old, disk + FIRST * 512, sizeof(old));
        protected_lba = FIRST;
        ring.data[ring.write_pos++] = 'z';
        calls = 0;
        fail_at = fail;
        plog_poll(true);
        assert(ring.flushed_pos == 6 && !plog_save_ok());
        assert(!memcmp(old, disk + FIRST * 512, sizeof(old)));
    }
    setup(&bi, m);
    plog_init(&bi, &b);
    plog_poll(true);
    assert(ring.flushed_pos == 6 && calls == 5 && plog_save_ok() &&
           !strcmp(plog_status(), "verified"));
    protected_lba = FIRST;
    memcpy(ring.data + 6, "next\n", 5);
    ring.write_pos = 11;
    calls = 0;
    plog_poll(true);
    assert(ring.flushed_pos == 11 && calls == 5);
    assert(!memcmp(disk + (SECOND + 1) * 512, "hello\nnext\n", 11));
    ring.data[ring.write_pos++] = 'p';
    panic = true;
    calls = 0;
    plog_poll(true);
    assert(!calls && plog_save_ok());
    setup(&bi, m);
    plog_init(&bi, NULL);
    assert(!strcmp(plog_status(), "unavailable"));
    setup(&bi, m);
    cpu = 1;
    plog_init(&bi, &b);
    plog_poll(true);
    assert(!calls);
    /* Manifest identity, MBR/BPB bounds, file size and cross-links. */
    for (unsigned guard = 0; guard < 15; ++guard) {
        setup(&bi, m);
        switch (guard) {
        case 0:
            memset(m + 16, 0, 16);
            seal(m);
            break;
        case 1:
            disk[510] ^= 1;
            break;
        case 2:
            test_put32(disk + 446 + 12, SECTORS);
            break;
        case 3:
            disk[MANIFEST * 512 + 400] ^= 1;
            break;
        case 4:
            memcpy(entries[1] + 20, entries[0] + 20, 2);
            memcpy(entries[1] + 26, entries[0] + 26, 2);
            break;
        case 5:
            memcpy(entries[2] + 20, entries[0] + 20, 2);
            memcpy(entries[2] + 26, entries[0] + 26, 2);
            break;
        case 6:
            disk[(TEST_FAT + TEST_FAT_SIZE) * 512 + test_cluster(FIRST) * 4] ^= 1;
            break;
        case 7:
            disk[TEST_PARTITION * 512 + 12] ^= 1;
            break;
        case 8:
            disk[TEST_PARTITION * 512 + 13] = 3;
            break;
        case 9:
            test_put32(entries[0] + 28, 512);
            break;
        case 10:
            test_link(test_cluster(CRASH), test_cluster(SECOND));
            break;
        case 11:
            m[100] = 1;
            seal(m);
            break;
        case 12:
            test_file("nvram-new-00a.bin", FIRST, 2 * 1024 * 1024);
            break;
        case 13:
            test_file("nvram-new.bin", FIRST, 2 * 1024 * 1024);
            break;
        case 14:
            test_put16(entries[0] + 26, 2);
            break;
        }
        refused(&bi, &b);
    }
    setup(&bi, m);
    entries[0][18] ^= 1;
    plog_init(&bi, &b);
    plog_poll(true);
    assert(calls == 5);
    /* A raw image on a larger device keeps its partition and file bounds. */
    setup(&bi, m);
    b.sectors = SECTORS + 100000;
    plog_init(&bi, &b);
    plog_poll(true);
    assert(calls == 5 && plog_save_ok());
    b.sectors = SECTORS;
    /* A fragmented payload is written through its chain and read next boot. */
    setup(&bi, m);
    relocated_lba = TEST_DATA + 50000;
    test_fragment(FIRST, relocated_lba);
    plog_init(&bi, &b);
    plog_poll(true);
    assert(plog_save_ok() && !memcmp(disk + relocated_lba * 512, "hello\n", 6));
    plog_init(&bi, &b);
    plog_poll(true);
    assert(plog_save_ok() && get64(disk + SECOND * 512 + 16) == 2);
    /* A read failure at any selection/verification stage is never empty. */
    setup(&bi, m);
    record(m, FIRST, 10, false);
    record(m, SECOND, 9, false);
    plog_init(&bi, &b);
    unsigned total_reads = reads;
    for (unsigned fail = 1; fail <= total_reads; ++fail) {
        setup(&bi, m);
        record(m, FIRST, 10, false);
        record(m, SECOND, 9, false);
        fail_read = fail;
        refused(&bi, &b);
    }
    for (unsigned newest = 0; newest < 2; ++newest) {
        setup(&bi, m);
        record(m, FIRST, newest ? 9 : 10, false);
        record(m, SECOND, newest ? 10 : 9, false);
        protected_lba = newest ? SECOND : FIRST;
        plog_init(&bi, &b);
        plog_poll(true);
        assert(get64(disk + (newest ? FIRST : SECOND) * 512 + 16) == 11);
    }
    /* The crash survives arbitrary normal commits and a clean subsequent boot. */
    setup(&bi, m);
    record(m, CRASH, 30, true);
    crash_text = "host exc: recovered\n";
    plog_init(&bi, &b);
    assert(get64(disk + CRASH * 512 + 16) == 31);
    unsigned char saved[1024];
    memcpy(saved, disk + CRASH * 512, sizeof(saved));
    for (unsigned j = 0; j < 6; ++j) {
        protected_lba = j ? (j & 1 ? FIRST : SECOND) : UINT64_MAX;
        ring.data[ring.write_pos++] = 'x';
        plog_poll(true);
        assert(!memcmp(saved, disk + CRASH * 512, sizeof(saved)));
    }
    protected_lba = UINT64_MAX;
    crash_text = NULL;
    calls = 0;
    plog_init(&bi, &b);
    plog_poll(true);
    assert(!memcmp(saved, disk + CRASH * 512, sizeof(saved)));
    crash_text = "host exc: next crash\n";
    uint64_t previous = get64(disk + CRASH * 512 + 16);
    plog_init(&bi, &b);
    assert(get64(disk + CRASH * 512 + 16) > previous);
    assert(!memcmp(disk + (CRASH + 1) * 512, crash_text, strlen(crash_text)));
    /* Alternating copies retain immutable prefix sectors. Only the lagging
     * copy's new sectors and a partial tail may be written. */
    setup(&bi, m);
    memset(ring.data, 'V', 65536);
    ring.write_pos = 65536;
    plog_init(&bi, &b);
    plog_poll(false);
    assert(written == 130);
    protected_lba = FIRST;
    written = 0;
    ring.write_pos += 65536;
    plog_poll(false);
    assert(written == 258);
    protected_lba = SECOND;
    written = 0;
    ring.write_pos += 65536;
    plog_poll(false);
    assert(written == 258);
    assert(!memcmp(disk + (FIRST + 1) * 512, ring.data, ring.write_pos));
    /* At capacity, wrap without rewriting retained sectors; each copy's
     * missing 128 KiB plus headers remains the write bound. */
    setup(&bi, m);
    memset(ring.data, 'R', LOG_RING_SIZE);
    plog_init(&bi, &b);
    for (unsigned j = 0; j < 34; ++j) {
        protected_lba = j ? (j & 1 ? FIRST : SECOND) : UINT64_MAX;
        memset(ring.data + j * 65536u % LOG_RING_SIZE, 'A' + j % 26, 65536);
        if (!j)
            memset(ring.data + 65536, 'Z', 17);
        ring.write_pos = (uint64_t)(j + 1) * 65536 + 17;
        written = 0;
        plog_poll(false);
        if (j > 1)
            assert(written <= 259);
    }
    unsigned char *full = disk + SECOND * 512;
    assert(get64(full + 24) == 2 * 1024 * 1024 - 512);
    uint32_t crc;
    memcpy(&crc, full + 48, 4);
    uint32_t start;
    memcpy(&start, full + 52, 4);
    unsigned capacity = 2 * 1024 * 1024 - 512;
    static unsigned char ordered[2 * 1024 * 1024 - 512];
    memcpy(ordered, full + 512 + start, capacity - start);
    memcpy(ordered + capacity - start, full + 512, start);
    assert(crc == plog_crc(ordered, capacity));
    /* Boot selection validates a wrapped record too, then protects the
     * newest copy while starting this boot's archive. */
    ring.write_pos = 6;
    ring.flushed_pos = 0;
    memcpy(ring.data, "hello\n", 6);
    protected_lba = SECOND;
    plog_init(&bi, &b);
    plog_poll(true);
    assert(plog_save_ok() && get64(disk + FIRST * 512 + 16) == 35);
    setup(&bi, m);
    plog_init(&bi, &b);
    plog_poll(false);
    assert(!calls);
    mock_now += 3000;
    plog_poll(false);
    assert(ring.flushed_pos == 6 && calls == 5);
    puts("PLOG writer: passed");
}
