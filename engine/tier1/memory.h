/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_TIER1_MEMORY_H
#define IA64VMM_TIER1_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The Tier1 pool. Code arena storage is
 * fixed at setup and accounted separately. Metadata (region tables and
 * records, restore and fixup tables, arena descriptors, compile contexts)
 * and compiler workspace (stage arrays freed by the end of a compile) share
 * one cap; metadata never takes the workspace reservation of the compiles
 * that can run at once. Admission is one atomic update. A refusal records
 * the request for maintenance (t1_reclaim) and returns NULL: allocation
 * never evicts. */
struct t1_memory_stats {
    uint64_t budget_bytes, reserve_bytes, used_bytes, workspace_bytes;
    uint64_t peak_bytes, workspace_peak, refusals;
};
void *t1_memory_alloc(size_t pages, unsigned flags);
void t1_memory_free(void *p, size_t pages);
void *t1_workspace_alloc(size_t pages, unsigned flags);
void t1_workspace_free(void *p, size_t pages);

/* A compile context's workspace: one block of
 * t1_compile_workspace_bytes, taken from the pool once (t1_ws_acquire) and
 * kept across the context's compiles. Taking it is an attempt: when the cap
 * does not admit it or no contiguous run is free, t1_ws_acquire returns
 * false and records nothing, and the arrays come from the pool (ws->base
 * NULL). t1_ws_alloc carves arrays from it in
 * stack order: t1_ws_free of the newest array pops it, any other free waits
 * for t1_ws_reset, which frees every carved array. Only HAL_PAGE_ZERO
 * requests are cleared. A request beyond the room, or with ws NULL (stages
 * run without a compile context), is a pool allocation as with
 * t1_workspace_alloc, and t1_ws_free returns it there. */
struct t1_workspace {
    uint8_t *base;     /* NULL: no block */
    size_t pages, top; /* block size and carved pages */
};
bool t1_ws_acquire(struct t1_workspace *ws); /* false: no block, nothing held or recorded */
void t1_ws_release(struct t1_workspace *ws);
void t1_ws_reset(struct t1_workspace *ws);
void *t1_ws_alloc(struct t1_workspace *ws, size_t pages, unsigned flags);
void t1_ws_free(struct t1_workspace *ws, void *p, size_t pages);
/* Refusals on the calling CPU: a compile stage that failed after one was
 * refused memory is deferred, not rejected. */
uint64_t t1_memory_refusals_here(void);
/* Metadata bytes that would be admitted now. */
uint64_t t1_memory_headroom(void);
/* Records a refusal of `bytes` the caller found without allocating. */
void t1_memory_refuse(uint64_t bytes);
/* The largest refused request maintenance has not covered (0: none). */
uint64_t t1_memory_wanted(void);
void t1_memory_satisfied(uint64_t wanted);
void t1_memory_get_stats(struct t1_memory_stats *out);
/* Stopped setup: free_bytes excludes existing allocations; unavailable
 * covers the validator budget and remaining lazy lower-tier allocations. */
void t1_memory_configure(uint64_t free_bytes, uint64_t unavailable, uint64_t reserve_bytes);
/* The bytes t1_memory_configure would add to the cap above Tier1's use. */
uint64_t t1_memory_allowance(uint64_t free_bytes, uint64_t unavailable, uint64_t reserve_bytes);
/* t1_init: statistics and outstanding requests only; the cap stays. */
void t1_memory_reset(void);
#ifdef IA64VMM_HOSTED
/* When set, workspace arrays not requested zeroed are filled with
 * T1_WORKSPACE_POISON: a stage that reads one before writing it differs
 * from the reference (test binaries set it; benches do not). */
#define T1_WORKSPACE_POISON 0xa5u
extern bool t1_test_workspace_poison;
/* The block size of later t1_ws_acquire calls in pages: 0 takes no block
 * (every array from the pool), SIZE_MAX t1_compile_workspace_bytes. An
 * armed t1_memory_test_refuse also counts a block attempt. */
void t1_memory_test_block(size_t pages);
void t1_memory_set_limit(uint64_t bytes);
/* Refuse the nth following allocation (1: the next one); 0 disarms. */
void t1_memory_test_refuse(unsigned nth);
void t1_memory_test_reset_peaks(void);
#endif
#endif
