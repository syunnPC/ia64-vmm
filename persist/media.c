/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FAT32 persistence discovery and the backend's periodic duty. */
#include "persist/persist.h"
#include "persist/fat32.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

static uint32_t u32(const uint8_t *b)
{
    uint32_t n;
    memcpy(&n, b, 4);
    return n;
}

/* Log and EFI variable files must occupy separate FAT allocations. */
static bool allocations(struct fat32 *fs)
{
    struct fat32_file files[69];
    unsigned count = 0;
    const char *const fixed[] = {"persistence-manifest.bin", "log-a.bin", "log-b.bin", "crash.bin",
                                 "nvram-new.bin"};
    for (unsigned i = 0; i < 59; ++i) {
        char name[32];
        const char *path;
        if (i < 5)
            path = fixed[i];
        else {
            unsigned at = i - 5;
            vmm_snprintf(name, sizeof(name), "nvram-%s-%02u%c.bin", "new", (at % 64) / 2,
                         'a' + at % 2);
            path = name;
        }
        struct fat32_file file;
        if (!fat32_lookup(fs, path, &file)) {
            if (fs->failed)
                return false;
            continue;
        }
        for (unsigned j = 0; j < count; ++j)
            if (!fat32_disjoint(files + j, &file))
                return false;
        files[count++] = file;
    }
    return true;
}

bool persist_mount(struct fat32 *fs, const struct plog_block *block, const char *name,
                   const uint8_t manifest[512], struct fat32_file *file)
{
    if (!block || !block->read || !block->write || !block->flush || !block->sectors)
        return false;
    uint8_t mbr[512], contents[512];
    if (!block->read(0, 1, mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa)
        return false;
    bool found = false;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *entry = mbr + 446 + i * 16;
        uint64_t first = u32(entry + 8), count = u32(entry + 12);
        if (!entry[4] && !first && !count)
            continue;
        if (!first || !count || first >= block->sectors || count > block->sectors - first)
            return false;
        for (unsigned j = 0; j < i; ++j) {
            const uint8_t *other = mbr + 446 + j * 16;
            uint64_t start = u32(other + 8), length = u32(other + 12);
            if (length && first < start + length && start < first + count)
                return false;
        }
        if (entry[4] != 0xef && entry[4] != 0x0b && entry[4] != 0x0c)
            continue;
        struct fat32 candidate;
        struct fat32_file mf;
        if (!fat32_mount(&candidate, block, first, first + count - 1))
            return false;
        if (!fat32_lookup(&candidate, name, &mf)) {
            if (candidate.failed)
                return false;
            continue;
        }
        if (mf.size != 512 || !fat32_read(&candidate, &mf, 0, contents, 512))
            return false;
        if (memcmp(contents, manifest, 512))
            continue;
        if (found)
            return false;
        *fs = candidate;
        *file = mf;
        found = true;
    }
    return found && allocations(fs);
}

/* FAT32 handles require these callbacks even for read-only traversal. */
static bool identify_write(uint64_t at, unsigned n, const void *in)
{
    (void)at;
    (void)n;
    (void)in;
    return false;
}
static bool identify_flush(void)
{
    return false;
}
bool persist_identify(const struct plog_block *block, const char *name, const uint8_t manifest[512])
{
    if (!block || !block->read || !block->sectors || !name || !manifest)
        return false;
    const struct plog_block probe = {block->sectors, block->read, identify_write, identify_flush};
    uint8_t mbr[512], contents[512];
    if (!probe.read(0, 1, mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa)
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *entry = mbr + 446 + i * 16;
        uint64_t first = u32(entry + 8), count = u32(entry + 12);
        if ((entry[4] != 0xef && entry[4] != 0x0b && entry[4] != 0x0c) || !first || !count ||
            first >= probe.sectors || count > probe.sectors - first)
            continue;
        struct fat32 fs;
        struct fat32_file file;
        if (!fat32_mount(&fs, &probe, first, first + count - 1) ||
            !fat32_lookup(&fs, name, &file) || file.size != 512 ||
            !fat32_read(&fs, &file, 0, contents, 512))
            continue;
        /* Identity reserves the disk even when other files or partitions
         * make it unsuitable for persistence writes. */
        if (!memcmp(contents, manifest, 512))
            return true;
    }
    return false;
}

bool persist_files(struct fat32 *fs, const struct fat32_file *manifest, const char *const *names,
                   struct fat32_file *files, unsigned count, uint32_t bytes)
{
    for (unsigned i = 0; i < count; ++i) {
        if (!fat32_lookup(fs, names[i], files + i) || files[i].size != bytes ||
            (files[i].attributes & 0x01) || !fat32_disjoint(manifest, files + i))
            return false;
        for (unsigned j = 0; j < i; ++j)
            if (!fat32_disjoint(files + j, files + i))
                return false;
    }
    return true;
}

static uint64_t (*duty)(void);
void media_watch_set(uint64_t (*watch)(void))
{
    duty = watch;
}
uint64_t media_watch(void)
{
    return duty ? duty() : UINT64_MAX;
}
