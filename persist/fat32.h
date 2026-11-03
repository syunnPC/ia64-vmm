/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_FAT32_H
#define VMM_FAT32_H
#include "persist/persist.h"
/* Management CPU only. Caller owns the volume and file handles.
 * Media/I/O errors stop writes until remount. */
struct fat32 {
    const struct plog_block *block;
    uint64_t first, last, fat_lba, data_lba;
    uint32_t total_sectors, fat_sectors, clusters, root_cluster, free_count, next_free;
    uint32_t root_tail, cursor_first, cursor_cluster, cursor_index;
    uint64_t cached_fat_lba;
    uint16_t reserved, fsinfo;
    uint8_t sectors_per_cluster, fat_count, active_fat;
    bool mirrored, failed;
    uint8_t work[512], fat_work[512];
};
struct fat32_file {
    uint64_t dir_lba;
    uint32_t first_cluster, size, tail_cluster, chain_clusters;
    uint16_t dir_offset;
    uint8_t attributes;
};
/* Bounds are inclusive device LBAs. The BPB may describe a smaller volume. */
bool fat32_mount(struct fat32 *, const struct plog_block *, uint64_t first, uint64_t last);
/* Root only; ASCII names, case insensitive SFN and validated ASCII LFN.
 * False with !fs->failed denotes a missing name. */
bool fat32_lookup(struct fat32 *, const char *name, struct fat32_file *);
bool fat32_create(struct fat32 *, const char *name, struct fat32_file *);
/* Validated chains intersect exactly when their terminal clusters agree. */
bool fat32_disjoint(const struct fat32_file *, const struct fat32_file *);
/* Byte ranges; writes cannot extend the file. Lookup validates the full chain.
 * Handles require exclusive ownership of the medium until remount. */
bool fat32_read(struct fat32 *, const struct fat32_file *, uint32_t offset, void *, uint32_t bytes);
bool fat32_write(struct fat32 *, const struct fat32_file *, uint32_t offset, const void *,
                 uint32_t bytes);
/* Grow only; newly exposed bytes are zero. Flushes data/FAT before publishing size.
 * A failed allocation can leak clusters; it never publishes unwritten bytes. */
bool fat32_grow(struct fat32 *, struct fat32_file *, uint32_t size);
bool fat32_flush(struct fat32 *);
#endif
