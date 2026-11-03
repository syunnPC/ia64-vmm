/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_TIER0_INTERNAL_H
#define IA64VMM_TIER0_INTERNAL_H
#include "engine/tier0/tier0.h"
#include "engine/tier0/x86emit.h"

#define TIER0_CFM_ROTATION_MASK                                                                    \
    ((UINT64_C(127) << 18) | (UINT64_C(127) << 25) | (UINT64_C(63) << 32))
#define TIER0_CHUNK_BYTES 65536u
/* Retired regions before an executor posts background reclamation. */
#define TIER0_RECLAIM_POST 64u
/* Ring distance at which opening a chunk evicts an older one, and free
 * region records below which allocation evicts one (arena.c). */
#define TIER0_EVICT_AHEAD(count) ((count) / 16u ? (count) / 16u : 1u)
struct tier0_key {
    gpa_t gpa;
    uint64_t ip, cfm, psr;
    /* Fetch permissions are checked on every entry; these distinguish
     * instruction-side contexts even when they alias the same source. */
    uint64_t fetch_rr, fetch_pta, fetch_pte;
    /* Only the frame shape specializes destination legality and rotation size. */
    unsigned slot, abi;
    unsigned cpu_model; /* CPU model: no cross-model generated-code reuse. */
    bool checking, icount, tracing;
    bool linked; /* an upper tier is registered: region shapes differ (ends_region) */
};
/* A boundary-cache hit reads the key, valid, chunk, entry and the homes
 * fields, then the dependency's page: they lead, in the first three cache
 * lines of a cache-line-aligned record. */
struct tier0_region {
    struct tier0_key key;
    _Atomic(bool) valid;
    unsigned chunk;
    void *entry;
    /* Stacked GR offsets and rotating predicates the generated code names;
     * serial (never 0 for lookup regions) tags cached homes of reused slots. */
    uint64_t gr_used[2], pr_used, serial;
    struct code_dependency dependency;
    unsigned count, pages;
    _Atomic(unsigned) hash_next;
    _Atomic(unsigned) *hash_previous;
    unsigned *source_previous;
    unsigned source_next, chunk_next, chunk_previous, retired_next;
    size_t bytes, arena_bytes;
    uint64_t source_generation, retirement;
    /* Shared promotion progress and pending-request hold (tier0.h).
     * Links and their probe sequences belong to each executor. */
    uint64_t upper_seq;
    uint32_t entries, upper_hold;
    void *allocation;
    const Ia64Instruction *insns;
    uint8_t slots[TIER0_MAX_INSNS];
} IA64VMM_ALIGNED(64);
/* regions: the regions staged in the chunk, linked by chunk_next (retired
 * ones stay linked until reclaimed); live: how many of them are valid. */
struct tier0_chunk {
    size_t used;
    unsigned regions, live;
    _Atomic(bool) executable;
    uint64_t retirement;
};
/* Shared mutations hold code_lock. Readers retain immutable region bytes
 * and links until their next published quiescent boundary. */
struct tier0_arena {
    uint8_t *code;
    void *metadata;
    size_t code_pages, metadata_pages;
    struct tier0_region *regions;
    /* Per slot: the region's serial while lookups can reach it, else 0.
     * The boundary cache (tier0.c) reads it instead of a slot that another
     * executor may be reinitializing. */
    _Atomic(uint64_t) *tags;
    struct tier0_chunk *chunks;
    _Atomic(unsigned) *buckets;
    unsigned *sources;
    unsigned capacity, hash_size, nchunks, next_chunk, open_chunk;
    unsigned available, free_regions;
    _Atomic(unsigned) retired, retired_count;
    _Atomic(unsigned) executing; /* legacy single-executor pin, not a grace period */
};
extern struct tier0_arena tier0_arena;
bool tier0_arena_ensure(void);
struct tier0_region *tier0_region_allocate(void);
void tier0_region_retire(struct tier0_region *r, bool eviction);
void tier0_reclaim(void);
bool tier0_stage(struct tier0_region *r, const Ia64Instruction *insns, const struct ia64_state *s);
void tier0_seal(struct vcpu *v, unsigned chunk);
void tier0_unlink(struct tier0_region *r);
struct tier0_check;
/* Physical homes of the stacked GR/NaT names (offset r - 32) and the bit
 * of each rotating predicate (p - 16) a region uses, resolved at its entry;
 * the generated code's fourth argument (R15). Unused entries stay unset. */
struct tier0_homes {
    uint32_t gr_index[96];
    uint64_t pr_mask[48];
};
/* Data translation cache of an executor (translate.c virtual references;
 * tier0.c tier0_data_pointer fills it). An entry holds a 4 KiB page
 * translated for owner at its data-side MMU sequence (D-TLB, RR and PKR
 * writes are logged) and PSR class (cpl, pk, dt: the PSR fields a TLB-hit
 * data translation reads), with the lasting rights (ia64_lasting_rights)
 * of a write-back RAM or firmware page. Direct-mapped by page; 64 bytes. */
#define TIER0_DTC_SLOTS 64u
struct tier0_dtc_entry {
    uint64_t page;            /* virtual address >> 12 */
    const struct vcpu *owner; /* NULL: empty */
    uint64_t mmu_dseq;
    uint8_t *host; /* host address of the page */
    gpa_t pa;      /* physical page */
    uint8_t rights, psr_class;
    uint8_t pad[22];
};
_Static_assert(sizeof(struct tier0_dtc_entry) == 64, "Tier0 DTC entry size");
/* Bytes of a native scalar reference (ld1-8 with .s, .a, .sa, .c.clr and
 * .c.nc, st1-8, st1-8.rel); lfetch references one byte, unaligned. */
static inline unsigned tier0_access_size(Ia64Opcode op)
{
    switch (op) {
    case IA64_OP_LD1:
    case IA64_OP_LD1S:
    case IA64_OP_LD1A:
    case IA64_OP_LD1SA:
    case IA64_OP_LD1C_CLR:
    case IA64_OP_LD1C_NC:
    case IA64_OP_ST1:
    case IA64_OP_ST1REL:
    case IA64_OP_LFETCH:
    case IA64_OP_LFETCH_FAULT:
        return 1;
    case IA64_OP_LD2:
    case IA64_OP_LD2S:
    case IA64_OP_LD2A:
    case IA64_OP_LD2SA:
    case IA64_OP_LD2C_CLR:
    case IA64_OP_LD2C_NC:
    case IA64_OP_ST2:
    case IA64_OP_ST2REL:
        return 2;
    case IA64_OP_LD4:
    case IA64_OP_LD4S:
    case IA64_OP_LD4A:
    case IA64_OP_LD4SA:
    case IA64_OP_LD4C_CLR:
    case IA64_OP_LD4C_NC:
    case IA64_OP_ST4:
    case IA64_OP_ST4REL:
        return 4;
    default:
        return 8;
    }
}
static inline unsigned tier0_dtc_class(uint64_t psr)
{
    return (unsigned)((psr >> 32) & 3) | (psr & IA64_PSR_PK ? 4u : 0u) |
           (psr & IA64_PSR_DT ? 8u : 0u);
}
/* The tracked-page bitmap span the executor's stores tested last
 * (validity.h code_page_span); pages == 0: none. */
struct tier0_code_span {
    uint64_t first, pages;
    const uint64_t *bits;
};
/* A store's code check outside the cached span (or with code validity
 * inactive): whether page holds code; caches page's span. Returns 0 or 1. */
uint64_t tier0_store_tracked(gpa_t page, struct tier0_code_span *span);
struct tier0_execution {
    struct tier0_region *region;
    struct tier0_check *check;
    struct tier0_stats *stats;
    struct tier0_dtc_entry *dtc;  /* the executor's data translation cache */
    struct tier0_code_span *span; /* the executor's store-check span */
    struct ia64_result result;
    const Ia64Instruction *fault_insn;
    struct vcpu_exit out;
    bool stop, fault;
};
/* The executor is the sole consumer. A producer arriving after a zero
 * acquire load leaves its notification pending for the next checkpoint. */
static inline unsigned tier0_take_notification(unsigned *word)
{
    return __atomic_load_n(word, __ATOMIC_ACQUIRE) ? __atomic_exchange_n(word, 0, __ATOMIC_ACQ_REL)
                                                   : 0;
}
extern struct tier0_stats tier0_stats; /* arena totals, under code_lock */
struct tier0_stats *tier0_local_stats(void);
bool tier0_execution_active(void);
void tier0_diagnostics_destroy(void);
void tier0_executors_destroy(void);
/* One writer per executor block (or code_lock for arena gauges).
 * Relaxed stores let live diagnostic dumps read without a counter RMW. */
#define TIER0_ADD(block, field, value)                                                             \
    do {                                                                                           \
        uint64_t *counter = (uint64_t *)&(block)->field;                                           \
        __atomic_store_n(counter, __atomic_load_n(counter, __ATOMIC_RELAXED) + (value),            \
                         __ATOMIC_RELAXED);                                                        \
    } while (0)
#define TIER0_STAT(expr)                                                                           \
    do {                                                                                           \
        if (IA64VMM_TIER0_STATS) {                                                                 \
            expr;                                                                                  \
        }                                                                                          \
    } while (0)
void tier0_emit(struct x86_buffer *b, struct tier0_region *r, const struct ia64_state *s);
typedef unsigned tier0_helper_fn(struct vcpu *v, const Ia64Instruction *insn,
                                 struct tier0_execution *e);
tier0_helper_fn tier0_helper;
/* tier0_helper for mov ibr/dbr, which also drops the cached armed levels
 * (tier0.c debug_armed); no other helper pays for them. */
tier0_helper_fn tier0_breakpoint_helper;
static inline tier0_helper_fn *tier0_helper_for(const Ia64Instruction *i)
{
    return i->opcode == IA64_OP_MOV_GRIBR || i->opcode == IA64_OP_MOV_GRIBR_INDEXED ||
                   i->opcode == IA64_OP_MOV_GRDBR || i->opcode == IA64_OP_MOV_GRDBR_INDEXED
               ? tier0_breakpoint_helper
               : tier0_helper;
}
void *tier0_memory_pointer(struct vcpu *v, const Ia64Instruction *insn, struct tier0_execution *e);
void *tier0_data_pointer(struct vcpu *v, const Ia64Instruction *insn, struct tier0_execution *e);
struct tier0_check *tier0_check_begin(struct vcpu *v);
bool tier0_check_capture(struct tier0_execution *e, const struct gm_region *r, gpa_t gpa);
bool tier0_check_finish(struct tier0_execution *e, const struct vcpu *v);
struct vcpu_exit tier0_check_debug_boundary(struct vcpu *v, bool *checked);
struct ia64_translation;
struct ia64_result tier0_check_fetch(struct vcpu *v, struct ia64_translation *translated);
bool tier0_check_interrupt(struct vcpu *v, bool *delivered);
void tier0_check_destroy(struct tier0_check *c);
bool tier0_check_safe(const Ia64Instruction *insn, const struct vcpu *v);
#endif
