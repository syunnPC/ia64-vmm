/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FAT32 BPB and directory layouts from Microsoft's FAT specification. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "persist/fat32.h"
#define FIRST 2048u
#define RESERVED 32u
#define FATSZ 600u
#define CLUSTERS 65530u
#define TOTAL (RESERVED + 2u * FATSZ + CLUSTERS)
#define LAST (FIRST + TOTAL - 1)
#define CAPACITY 100000u
#define FAT (FIRST + RESERVED)
#define DATA (FAT + 2u * FATSZ)
static uint8_t disk[(FIRST + TOTAL) * 512u];
static unsigned reads, writes, flushes, fail_read, fail_write, fail_flush, largest_write;
static struct fat32 fs;
static struct fat32_file file;
static uint8_t *sector(unsigned lba)
{
    return disk + (size_t)lba * 512;
}
static void p16(uint8_t *p, unsigned x)
{
    p[0] = x;
    p[1] = x >> 8;
}
static void p32(uint8_t *p, uint32_t x)
{
    p16(p, x);
    p16(p + 2, x >> 16);
}
static uint32_t g32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool rd(uint64_t lba, unsigned n, void *out)
{
    assert(n && n <= 32 && lba >= FIRST && lba <= LAST && n <= LAST - lba + 1);
    if (++reads == fail_read)
        return false;
    memcpy(out, sector(lba), n * 512);
    return true;
}
static bool wr(uint64_t lba, unsigned n, const void *in)
{
    assert(n && n <= 32 && lba >= FIRST && lba <= LAST && n <= LAST - lba + 1);
    if (n > largest_write)
        largest_write = n;
    if (++writes == fail_write)
        return false;
    memcpy(sector(lba), in, n * 512);
    return true;
}
static bool fl(void)
{
    return ++flushes != fail_flush;
}
static const struct plog_block block = {CAPACITY, rd, wr, fl};
static void fat(unsigned c, unsigned value)
{
    for (unsigned f = 0; f < 2; ++f)
        p32(sector(FAT + f * FATSZ) + c * 4, value);
}
static uint8_t *entry(unsigned i)
{
    return sector(DATA) + i * 32;
}
static void fixture(void)
{
    memset(disk, 0, sizeof(disk));
    reads = writes = flushes = fail_read = fail_write = fail_flush = largest_write = 0;
    uint8_t *b = sector(FIRST);
    p16(b + 11, 512);
    b[13] = 1;
    p16(b + 14, RESERVED);
    b[16] = 2;
    b[21] = 0xf8;
    p32(b + 32, TOTAL);
    p32(b + 36, FATSZ);
    p32(b + 44, 2);
    p16(b + 48, 1);
    p16(b + 50, 6);
    p16(b + 510, 0xaa55);
    memcpy(sector(FIRST + 6), b, 512);
    b = sector(FIRST + 1);
    p32(b, 0x41615252);
    p32(b + 484, 0x61417272);
    p32(b + 488, CLUSTERS - 4);
    p32(b + 492, 3);
    p32(b + 508, 0xaa550000);
    memcpy(sector(FIRST + 7), b, 512);
    fat(0, 0x0ffffff8);
    fat(1, 0x0fffffff);
    fat(2, 0x0fffffff);
    fat(5, 9);
    fat(9, 6);
    fat(6, 0x0fffffff);
    b = entry(0);
    memcpy(b, "RECORD  BIN", 11);
    b[11] = 0x20;
    p16(b + 26, 5);
    p32(b + 28, 1300);
    memset(sector(DATA + 3), 'A', 512);
    memset(sector(DATA + 7), 'B', 512);
    memset(sector(DATA + 4), 'C', 512);
    assert(fat32_mount(&fs, &block, FIRST, LAST));
}
static void geometry(void)
{
    fixture();
    assert(fs.clusters == CLUSTERS);
    assert(fat32_lookup(&fs, "record.bin", &file));
    assert(file.size == 1300 && file.first_cluster == 5);
    assert(!fat32_lookup(&fs, "absent", &file) && !fs.failed);
    assert(!fat32_mount(&fs, &block, FIRST, LAST - 1));
    fixture();
    p32(sector(FIRST) + 32, RESERVED + 2 * FATSZ + 65524);
    assert(!fat32_mount(&fs, &block, FIRST, LAST));
    fixture();
    p32(sector(FIRST) + 36, 1);
    assert(!fat32_mount(&fs, &block, FIRST, LAST));
    fixture();
    p16(sector(FIRST) + 42, 1);
    assert(!fat32_mount(&fs, &block, FIRST, LAST));
    fixture();
    fail_read = reads + 1;
    assert(!fat32_lookup(&fs, "record.bin", &file));
    assert(fs.failed);
}
static void fragmented(void)
{
    uint8_t out[1300], patch[600];
    fixture();
    assert(fat32_lookup(&fs, "RECORD.BIN", &file));
    assert(fat32_read(&fs, &file, 0, out, sizeof(out)));
    for (unsigned i = 0; i < sizeof(out); ++i)
        assert(out[i] == (i < 512 ? 'A' : i < 1024 ? 'B' : 'C'));
    memset(patch, 0x57, sizeof(patch));
    assert(fat32_write(&fs, &file, 400, patch, sizeof(patch)));
    assert(fat32_flush(&fs));
    assert(fat32_read(&fs, &file, 0, out, sizeof(out)));
    for (unsigned i = 0; i < sizeof(out); ++i)
        assert(out[i] == (i >= 400 && i < 1000 ? 0x57 : i < 512 ? 'A' : i < 1024 ? 'B' : 'C'));
    unsigned before = writes;
    assert(!fat32_write(&fs, &file, 1299, patch, 2));
    assert(writes == before);
    fixture();
    entry(0)[11] |= 1;
    assert(fat32_lookup(&fs, "record.bin", &file));
    assert(!fat32_write(&fs, &file, 0, patch, 1) && !writes);
}
static void invalid_chains(void)
{
    unsigned bad[] = {0, 1, 0x0ffffff7, CLUSTERS + 2, 5};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        fixture();
        fat(9, bad[i]);
        assert(!fat32_lookup(&fs, "record.bin", &file));
        assert(fs.failed && !writes);
    }
    fixture();
    fat(9, 0x0fffffff);
    assert(!fat32_lookup(&fs, "record.bin", &file));
    fixture();
    fat(2, 2);
    assert(!fat32_lookup(&fs, "record.bin", &file));
    fixture();
    p32(sector(FAT + FATSZ) + 5 * 4, 7);
    assert(!fat32_lookup(&fs, "record.bin", &file));
}
static unsigned checksum(const uint8_t *n)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < 11; ++i)
        sum = (((sum & 1) << 7) + (sum >> 1) + n[i]) & 255;
    return sum;
}
static void long_names(void)
{
    static const unsigned at[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    fixture();
    memcpy(entry(1), entry(0), 32);
    memset(entry(0), 0xff, 32);
    uint8_t *e = entry(0);
    e[0] = 0x41;
    e[11] = 0x0f;
    e[12] = 0;
    e[13] = checksum(entry(1));
    p16(e + 26, 0);
    const char *name = "Saved Log";
    for (unsigned i = 0; i < 13; ++i)
        p16(e + at[i], i < 9 ? (unsigned)name[i] : i == 9 ? 0 : 0xffff);
    assert(fat32_lookup(&fs, "saved log", &file));
    e[13] ^= 1;
    assert(!fat32_lookup(&fs, "saved log", &file) && !fs.failed);
    assert(fat32_lookup(&fs, "record.bin", &file));
}
static void long_boundary(void)
{
    static const unsigned at[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    static const char name[] = "nvram-new-00a.bin";
    fixture();
    fat(2, 4);
    fat(4, 0x0fffffff);
    uint8_t saved[32];
    memcpy(saved, entry(0), 32);
    memcpy(saved, "NVRAMN~1BIN", 11);
    for (unsigned i = 0; i < 15; ++i) {
        memset(entry(i), ' ', 11);
        entry(i)[0] = 'A' + i;
        entry(i)[11] = 0x20;
    }
    for (unsigned seq = 1; seq <= 2; ++seq) {
        uint8_t *e = seq == 2 ? entry(15) : sector(DATA + 2);
        memset(e, 0, 32);
        e[0] = seq | (seq == 2 ? 0x40 : 0);
        e[11] = 0x0f;
        e[13] = checksum(saved);
        for (unsigned j = 0; j < 13; ++j) {
            unsigned i = (seq - 1) * 13 + j;
            p16(e + at[j], i < strlen(name) ? (unsigned)name[i] : i == strlen(name) ? 0 : 0xffff);
        }
    }
    memcpy(sector(DATA + 2) + 32, saved, 32);
    assert(fat32_mount(&fs, &block, FIRST, LAST));
    assert(fat32_lookup(&fs, "NVRAM-NEW-00A.BIN", &file) && file.first_cluster == 5);
    sector(DATA + 2)[0] = 2;
    assert(!fat32_lookup(&fs, name, &file) && !fs.failed);
    assert(fat32_lookup(&fs, "nvramn~1.bin", &file));
    sector(DATA + 2)[0] = 1;
    p16(sector(DATA + 2) + 1, 0x1234);
    assert(!fat32_lookup(&fs, name, &file) && !fs.failed);
    fixture();
    fat(2, 4);
    fat(4, 0x0fffffff);
    for (unsigned i = 0; i < 15; ++i) {
        memset(entry(i), ' ', 11);
        entry(i)[0] = 'A' + i;
        entry(i)[11] = 0x20;
    }
    entry(15)[0] = 0;
    memcpy(sector(DATA + 2), saved, 32);
    assert(fat32_mount(&fs, &block, FIRST, LAST));
    assert(fat32_create(&fs, "new.bin", &file));
    assert(!sector(DATA + 2)[0]);
    assert(!fat32_lookup(&fs, "nvramn~1.bin", &file) && !fs.failed);
}
static void create_grow(void)
{
    uint8_t out[1800];
    fixture();
    assert(fat32_create(&fs, "new.bin", &file));
    assert(!file.size && !file.first_cluster);
    assert(fat32_grow(&fs, &file, sizeof(out)));
    assert(file.size == sizeof(out));
    memset(out, 1, sizeof(out));
    assert(fat32_read(&fs, &file, 0, out, sizeof(out)));
    for (unsigned i = 0; i < sizeof(out); ++i)
        assert(!out[i]);
    assert(!memcmp(sector(FAT), sector(FAT + FATSZ), FATSZ * 512));
    assert(fat32_lookup(&fs, "new.bin", &file) && file.size == sizeof(out));
    assert(g32(sector(FIRST + 1) + 488) == CLUSTERS - 8 ||
           g32(sector(FIRST + 1) + 488) == UINT32_MAX);
    assert(!fat32_create(&fs, "new.bin", &file));
    fixture();
    assert(fat32_lookup(&fs, "record.bin", &file));
    assert(fat32_grow(&fs, &file, 1800));
    assert(fat32_read(&fs, &file, 0, out, sizeof(out)));
    for (unsigned i = 0; i < sizeof(out); ++i)
        assert(out[i] == (i >= 1300 ? 0 : i < 512 ? 'A' : i < 1024 ? 'B' : 'C'));
}
static void directory_and_allocation(void)
{
    fixture();
    /* A directory occupies the same namespace as regular files. */
    entry(0)[11] = 0x10;
    assert(!fat32_create(&fs, "record.bin", &file) && !writes);
    fixture();
    for (unsigned i = 0; i < 16; ++i) {
        memset(entry(i), ' ', 11);
        entry(i)[0] = 'A' + i;
        entry(i)[11] = 0x20;
    }
    assert(fat32_create(&fs, "extra.dat", &file));
    assert(fs.root_tail != 2 && file.dir_lba != DATA);
    assert(fat32_lookup(&fs, "extra.dat", &file));
    struct fat32_file other = file;
    assert(fat32_disjoint(&file, &other)); /* empty files have no clusters */
    assert(fat32_grow(&fs, &file, 17));
    other = file;
    assert(!fat32_disjoint(&file, &other));
    fixture();
    /* Root and file chains must be disjoint, including merged tails. */
    fat(6, 2);
    assert(!fat32_lookup(&fs, "record.bin", &file) && fs.failed);
    fixture();
    /* High reserved FAT bits are preserved independently in each copy. */
    for (unsigned i = 0; i < 2; ++i)
        p32(sector(FAT + i * FATSZ) + 3 * 4, (i + 1) << 28);
    assert(fat32_create(&fs, "new.bin", &file));
    assert(fat32_grow(&fs, &file, 1));
    assert((g32(sector(FAT) + 3 * 4) >> 28) == 1);
    assert((g32(sector(FAT + FATSZ) + 3 * 4) >> 28) == 2);
    fixture();
    /* With mirroring disabled only the selected FAT is authoritative. */
    p16(sector(FIRST) + 40, 0x81);
    p32(sector(FAT) + 5 * 4, 0);
    assert(fat32_mount(&fs, &block, FIRST, LAST));
    assert(fat32_lookup(&fs, "record.bin", &file));
    assert(fat32_create(&fs, "new.bin", &file));
    assert(fat32_grow(&fs, &file, 1));
    assert(!g32(sector(FAT) + 3 * 4));
    assert(g32(sector(FAT + FATSZ) + 3 * 4));
    fixture();
    /* Invalid FSInfo hints do not cause out-of-range allocation. */
    p32(sector(FIRST + 1) + 488, 0xfffffffe);
    p32(sector(FIRST + 1) + 492, 0xfffffffe);
    assert(fat32_mount(&fs, &block, FIRST, LAST));
    assert(fat32_create(&fs, "new.bin", &file));
    assert(fat32_grow(&fs, &file, 1));
}
static void coalescing_and_exhaustion(void)
{
    uint8_t data[32 * 512];
    fixture();
    for (unsigned c = 20; c < 52; ++c)
        fat(c, c == 51 ? 0x0fffffff : c + 1);
    p16(entry(0) + 26, 20);
    p32(entry(0) + 28, sizeof(data));
    assert(fat32_lookup(&fs, "record.bin", &file));
    memset(data, 0x42, sizeof(data));
    assert(fat32_write(&fs, &file, 0, data, sizeof(data)));
    assert(largest_write == 32);
    assert(fat32_read(&fs, &file, 0, data, sizeof(data)));
    for (unsigned i = 0; i < sizeof(data); ++i)
        assert(data[i] == 0x42);
    fixture();
    for (unsigned c = 3; c < CLUSTERS + 2; ++c)
        if (c != 5 && c != 9 && c != 6)
            fat(c, 0x0fffffff);
    fat(3, 0);
    assert(fat32_lookup(&fs, "record.bin", &file));
    assert(!fat32_grow(&fs, &file, 2600) && fs.failed);
    unsigned before = writes;
    assert(!fat32_write(&fs, &file, 0, data, 1) && writes == before);
}
static void io_failures(void)
{
    uint8_t patch[2] = {1, 2};
    fixture();
    assert(fat32_lookup(&fs, "record.bin", &file));
    fail_write = writes + 1;
    assert(!fat32_write(&fs, &file, 0, patch, 2));
    assert(fs.failed);
    unsigned before = writes;
    assert(!fat32_write(&fs, &file, 0, patch, 2) && writes == before);
    fixture();
    assert(fat32_lookup(&fs, "record.bin", &file));
    fail_flush = flushes + 1;
    assert(!fat32_flush(&fs) && fs.failed);
    for (unsigned f = 1; f <= 24; ++f) {
        fixture();
        assert(fat32_lookup(&fs, "record.bin", &file));
        fail_write = f;
        bool ok = fat32_grow(&fs, &file, 2200);
        if (!ok) {
            assert(fs.failed);
            before = writes;
            assert(!fat32_write(&fs, &file, 0, patch, 2) && before == writes);
            assert(g32(entry(0) + 28) == 1300 || g32(entry(0) + 28) == 2200);
        }
    }
    for (unsigned f = 1; f <= 12; ++f) {
        fixture();
        assert(fat32_lookup(&fs, "record.bin", &file));
        fail_flush = f;
        if (!fat32_grow(&fs, &file, 2200)) {
            assert(fs.failed);
            before = writes;
            assert(!fat32_write(&fs, &file, 0, patch, 2) && before == writes);
        }
    }
    for (unsigned r = 1; r <= 20; ++r) {
        fixture();
        assert(fat32_lookup(&fs, "record.bin", &file));
        fail_read = reads + r;
        if (!fat32_grow(&fs, &file, 2200)) {
            assert(fs.failed);
            before = writes;
            assert(!fat32_write(&fs, &file, 0, patch, 2) && before == writes);
        }
    }
}
int main(void)
{
    geometry();
    fragmented();
    invalid_chains();
    long_names();
    long_boundary();
    create_grow();
    directory_and_allocation();
    coalescing_and_exhaustion();
    io_failures();
    puts("fat32: ok");
    return 0;
}
