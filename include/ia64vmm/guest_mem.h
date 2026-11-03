/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Guest physical address space.
 *
 * Ordinary guest RAM is identity-laid-out: GPA == HPA on bare metal. The
 * hosted harness cannot reproduce that, so every region carries the host
 * pointer of its base and all accesses go through gm_*; nothing outside this
 * module computes a host pointer from a GPA on its own.
 *
 * The region table is built once by core/ before the first vCPU runs and is
 * read-only afterwards (no locking on the access path).
 */
#ifndef IA64VMM_GUEST_MEM_H
#define IA64VMM_GUEST_MEM_H

#include "ia64vmm/types.h"

struct vcpu;

enum gm_kind {
    GM_RAM,      /* guest-owned ordinary RAM: direct WB access */
    GM_FIRMWARE, /* RAM holding guest firmware code/data: direct, reported as runtime memory */
    GM_DIRECT_MMIO, /* real device range exposed to the guest (e.g. inherited GOP FB): direct, uncached semantics */
    GM_EMULATED, /* VMM-mediated range: every access calls ops */
};

struct gm_mmio_ops {
    /* size is 1, 2, 4 or 8; offset is relative to the region base.
     * The access completes before the call returns. A handler that needs
     * the vCPU to leave the execution loop (shutdown, halt, ...) calls
     * vcpu_request_exit() (engine.h); the current instruction still
     * completes. */
    uint64_t (*read)(void *opaque, struct vcpu *v, uint64_t offset, unsigned size);
    void (*write)(void *opaque, struct vcpu *v, uint64_t offset, uint64_t value, unsigned size);
};

struct gm_region {
    gpa_t base;
    uint64_t size;
    enum gm_kind kind;
    const char *name;
    /* GM_RAM / GM_FIRMWARE / GM_DIRECT_MMIO */
    uint8_t *hva; /* host pointer corresponding to base */
    /* GM_EMULATED */
    const struct gm_mmio_ops *ops;
    void *opaque;
};

#define GM_MAX_REGIONS 512

/* Registration (core/ and platform/ only, before vCPUs start). Regions must
 * not overlap; returns false and logs on overlap or table exhaustion. */
bool gm_add_region(const struct gm_region *r);

/* Lookup for the access path. Returns NULL when gpa is in no region; the
 * caller (ia64 memory access) then raises the architectural consequence or
 * stops the vCPU with a diagnostic, never silently ignores the access. */
const struct gm_region *gm_lookup(gpa_t gpa);

/* Direct host pointer for [gpa, gpa+len) if it lies entirely inside one
 * direct region (RAM/FIRMWARE/DIRECT_MMIO), else NULL. */
void *gm_direct_ptr(gpa_t gpa, uint64_t len);

/* VMM-originated copies into/out of guest memory (firmware placement,
 * descriptor generation, service requests). gm_write notifies the code
 * cache (code_notify_vmm_write) before writing. Return false if any byte is
 * outside direct regions. */
bool gm_write(gpa_t gpa, const void *src, uint64_t len);
bool gm_read(gpa_t gpa, void *dst, uint64_t len);

/* Iteration for memory-map generation and diagnostics. */
unsigned gm_region_count(void);
const struct gm_region *gm_region_at(unsigned index);

/* gm_read, gm_write and gm_direct_ptr are byte copies for RAM and
 * firmware memory: memcpy chooses the access widths, count and order. A
 * GM_DIRECT_MMIO range is direct too, so callers that may see one check the
 * region kind first; device registers are accessed only through
 * ia64_physical_read/write (one access of the guest's width) or the
 * region's ops. Every product caller copies to or from GM_RAM
 * (platform_ram_write, the PAL TR buffer) or reads RAM/firmware code for
 * diagnostics (ia64/diagnostics.c excludes MMIO kinds). */

#endif
