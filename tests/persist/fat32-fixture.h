/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FAT32 geometry has at least 65525 data clusters; a primary MBR ESP. */
#define TEST_PARTITION 2048u
#define TEST_FAT (TEST_PARTITION + 32u)
#define TEST_FAT_SIZE 1024u
#define TEST_DATA (TEST_FAT + 2u * TEST_FAT_SIZE)
#define TEST_DIRECTORY TEST_DATA
#define TEST_MANIFEST (TEST_DATA + 40u)
static unsigned test_entries;
static void test_put16(void *out, unsigned n)
{
    unsigned char *p = out;
    p[0] = n;
    p[1] = n >> 8;
}
static void test_put32(void *out, uint32_t n)
{
    memcpy(out, &n, 4);
}
static unsigned test_cluster(uint64_t lba)
{
    return (unsigned)(lba - TEST_DATA + 2);
}
static void test_link(unsigned cluster, unsigned next)
{
    for (unsigned copy = 0; copy < 2; ++copy)
        test_put32(disk + (TEST_FAT + copy * TEST_FAT_SIZE) * 512 + cluster * 4, next);
}
static void test_chain(uint64_t lba, unsigned sectors)
{
    unsigned first = test_cluster(lba);
    for (unsigned i = 0; i < sectors; ++i)
        test_link(first + i, i + 1 == sectors ? 0x0fffffff : first + i + 1);
}
static void test_volume(unsigned sectors)
{
    unsigned char *e = disk + 446, *boot = disk + TEST_PARTITION * 512;
    disk[510] = 0x55;
    disk[511] = 0xaa;
    e[4] = 0xef;
    test_put32(e + 8, TEST_PARTITION);
    test_put32(e + 12, sectors - TEST_PARTITION);
    test_put16(boot + 11, 512);
    boot[13] = 1;
    test_put16(boot + 14, 32);
    boot[16] = 2;
    test_put32(boot + 28, TEST_PARTITION);
    test_put32(boot + 32, sectors - TEST_PARTITION);
    test_put32(boot + 36, TEST_FAT_SIZE);
    test_put32(boot + 44, 2);
    memcpy(boot + 82, "FAT32   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xaa;
    test_link(0, 0x0ffffff8);
    test_link(1, 0x0fffffff);
    test_chain(TEST_DIRECTORY, 24);
    test_entries = 0;
}
/* Return the short entry; long names use standard UTF-16 LFN entries. */
static unsigned char *test_file(const char *name, uint64_t lba, unsigned bytes)
{
    static const unsigned char offsets[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    unsigned char shortname[11];
    memset(shortname, ' ', 11);
    char alias[9];
    snprintf(alias, sizeof(alias), "T%07u", test_entries);
    memcpy(shortname, alias, 8);
    memcpy(shortname + 8, "BIN", 3);
    unsigned checksum = 0;
    for (unsigned i = 0; i < 11; ++i)
        checksum = ((checksum >> 1) | ((checksum & 1) << 7)) + shortname[i], checksum &= 255;
    unsigned length = (unsigned)strlen(name), slots = (length + 12) / 13;
    for (unsigned i = slots; i; --i) {
        unsigned char *e = disk + TEST_DIRECTORY * 512 + test_entries++ * 32;
        memset(e, 0xff, 32);
        e[0] = i | (i == slots ? 0x40 : 0);
        e[11] = 15;
        e[12] = 0;
        e[13] = checksum;
        e[26] = e[27] = 0;
        for (unsigned j = 0; j < 13; ++j) {
            unsigned at = (i - 1) * 13 + j;
            test_put16(e + offsets[j], at < length    ? (unsigned char)name[at]
                                       : at == length ? 0
                                                      : 0xffff);
        }
    }
    unsigned char *e = disk + TEST_DIRECTORY * 512 + test_entries++ * 32;
    memset(e, 0, 32);
    memcpy(e, shortname, 11);
    e[11] = 32;
    unsigned cluster = test_cluster(lba);
    test_put16(e + 20, cluster >> 16);
    test_put16(e + 26, cluster);
    test_put32(e + 28, bytes);
    test_chain(lba, (bytes + 511) / 512);
    return e;
}
/* Move the second cluster outside the original contiguous extent. */
static void test_fragment(uint64_t first, uint64_t replacement)
{
    memcpy(disk + replacement * 512, disk + (first + 1) * 512, 512);
    memset(disk + (first + 1) * 512, 0x5a, 512);
    test_link(test_cluster(first), test_cluster(replacement));
    test_link(test_cluster(replacement), test_cluster(first + 2));
    test_link(test_cluster(first + 1), 0);
}
