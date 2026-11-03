/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_BLOCK_H
#define IA64VMM_BLOCK_H
#include "ia64vmm/types.h"

/* A separately selected guest medium, in native logical sectors.
 * Callbacks run on the management CPU and receive only VMM-owned buffers.
 * A successful flush makes preceding writes durable. */
struct platform_block {
    void *context;
    uint64_t sectors;
    bool read_only;
    bool (*read)(void *, uint64_t lba, unsigned count, void *buffer);
    bool (*write)(void *, uint64_t lba, unsigned count, const void *buffer);
    bool (*flush)(void *);
    unsigned sector_bytes; /* 0, 512 or 4096: disk; 2048: read-only optical medium. */
};
#endif
