/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/cache/background.h"
#include "engine/cache/coherence-inline.h"
#include "engine/tier0/internal.h"
#include "engine/interp/interp.h"
#include "ia64/debug.h"
#include "ia64/fault-count.h"
#include "ia64/interrupt.h"
#include "ia64/mmu.h"
#include "ia64/pmu.h"
#include "ia64/tlb.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/platform.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#include <stdatomic.h>

struct tier0_stats tier0_stats;
static struct {
    struct tier0_stats counters;
    _Atomic(unsigned) active;
} IA64VMM_ALIGNED(64) executor_stats[256];
struct tier0_stats *tier0_local_stats(void)
{
    return &executor_stats[code_counter_index()].counters;
}
bool tier0_execution_active(void)
{
    if (tier0_arena.executing)
        return true; /* raw native-code test pin */
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(executor_stats); ++i)
        if (atomic_load_explicit(&executor_stats[i].active, memory_order_acquire))
            return true;
    return false;
}
static uint64_t region_serial;
/* The upper tier (tier0.h); NULL: none. Stopped executors only. */
static const struct tier0_upper_ops *upper;
void tier0_set_upper(const struct tier0_upper_ops *ops)
{
    upper = ops;
}
/* Indices plus one keep zero-initialized buckets empty. Source buckets
 * avoid scanning thousands of regions on each ordinary memory access. */
static unsigned key_hash(const struct tier0_key *key)
{
    uint64_t x = key->ip ^ (key->gpa >> 4) ^ key->cfm ^ (key->psr << 7) ^
                 ((uint64_t)key->slot << 3) ^ ((uint64_t)key->abi << 29) ^
                 ((uint64_t)key->checking << 1) ^ key->icount ^ ((uint64_t)key->tracing << 2) ^
                 ((uint64_t)key->linked << 4) ^ key->fetch_rr ^ key->fetch_pta ^ key->fetch_pte;
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    return x & (tier0_arena.hash_size - 1);
}
static unsigned source_hash(gpa_t page)
{
    return ((page >> 12) ^ (page >> 24)) & (tier0_arena.hash_size - 1);
}
void tier0_unlink(struct tier0_region *r)
{
    struct tier0_arena *a = &tier0_arena;
    if (r->hash_previous) {
        *r->hash_previous = r->hash_next;
        if (r->hash_next)
            a->regions[r->hash_next - 1].hash_previous = r->hash_previous;
    }
    if (r->source_previous) {
        *r->source_previous = r->source_next;
        if (r->source_next)
            a->regions[r->source_next - 1].source_previous = r->source_previous;
    }
    r->hash_previous = NULL;
    r->source_previous = NULL;
    /* Keep hash_next intact for readers that acquired this link before
     * unlinking. Reclamation clears it only after their epoch boundary. */
    r->source_next = 0;
}
static void invalidate_region(struct code_dependency *dependency)
{
    tier0_region_retire(
        (struct tier0_region *)((char *)dependency - offsetof(struct tier0_region, dependency)),
        false);
}

static struct vcpu_exit exit_at(const struct vcpu *v, enum vcpu_exit_reason reason, uint64_t info,
                                const char *detail)
{
    return (struct vcpu_exit){reason, v->s.ip, v->s.slot, info, detail};
}

static void tier0_invalidate_locked(gpa_t gpa, uint64_t len)
{
    struct tier0_arena *a = &tier0_arena;
    if (!len || !a->regions)
        return;
    uint64_t last = len - 1 > UINT64_MAX - gpa ? UINT64_MAX : gpa + len - 1;
    gpa_t first_page = gpa & ~UINT64_C(4095), last_page = last & ~UINT64_C(4095);
    if (first_page == last_page) {
        unsigned link = a->sources[source_hash(first_page)];
        while (link) {
            struct tier0_region *r = &a->regions[link - 1];
            link = r->source_next;
            if ((r->key.gpa & ~UINT64_C(4095)) == first_page) {
                uint64_t mask[4];
                code_bundle_mask(first_page, gpa, len, mask);
                bool hit = false;
                for (unsigned word = 0; word < 4; ++word)
                    hit |= (mask[word] & r->dependency.mask[word]) != 0;
                if (hit)
                    tier0_region_retire(r, false);
            }
        }
    } else
        for (unsigned i = 0; i < a->capacity; ++i) {
            struct tier0_region *r = &a->regions[i];
            gpa_t page = r->key.gpa & ~UINT64_C(4095);
            if (r->valid && page >= first_page && page <= last_page) {
                uint64_t mask[4];
                code_bundle_mask(page, gpa, len, mask);
                bool hit = false;
                for (unsigned word = 0; word < 4; ++word)
                    hit |= (mask[word] & r->dependency.mask[word]) != 0;
                if (hit)
                    tier0_region_retire(r, false);
            }
        }
}
static void tier0_flush_locked(void)
{
    struct tier0_arena *a = &tier0_arena;
    if (!a->regions)
        return;
    for (unsigned i = 0; i < a->capacity; ++i)
        tier0_region_retire(&a->regions[i], false);
}
bool tier0_page_has_code(gpa_t page)
{
    struct tier0_arena *a = &tier0_arena;
    if (!a->regions)
        return false;
    for (unsigned link = a->sources[source_hash(page)]; link;
         link = a->regions[link - 1].source_next)
        if ((a->regions[link - 1].key.gpa & ~UINT64_C(4095)) == page)
            return true;
    return false;
}
/* Forms that change no fetch, frame or key premise: I-unit moves of ar.lc
 * and ar.pfs, branch-register moves, hints and chk.s. With through (Tier0
 * runs without an upper tier) also mov ar.ccv, I-unit mov ar.ec, AR reads,
 * predicate moves, mov r=ip, mf, qualified branches that do not rotate
 * and br.cloop: a taken one leaves the region, a not-taken one changed
 * nothing (an unqualified branch is always taken), and alloc, whose frame
 * the translator gives the instructions after it. An upper tier counts
 * Tier0 region entries as its heads (tier0.h), so with one
 * the code after a branch or a system form stays a region entry. */
static bool ends_region(const Ia64Instruction *i, bool through)
{
    enum ia64_family f = ia64_opcode_info(i->opcode)->family;
    unsigned ar = i->operands.system.source;
    switch (i->opcode) {
    case IA64_OP_MOV_GRAR:
    case IA64_OP_MOV_IMMAR:
    case IA64_OP_MOV_ARGR:
        if (i->unit == IA64_UNIT_I && (ar == AR_LC || ar == AR_PFS))
            return false;
        if (through && (i->opcode == IA64_OP_MOV_ARGR || (i->unit == IA64_UNIT_I && ar == AR_EC) ||
                        (i->unit == IA64_UNIT_M && ar == AR_CCV)))
            return false;
        break;
    case IA64_OP_MOV_GRBR:
    case IA64_OP_MOV_BRGR:
    case IA64_OP_HINT_M:
    case IA64_OP_HINT_I:
    case IA64_OP_HINT_F:
    case IA64_OP_HINT_X:
        return false;
    case IA64_OP_MOV_PRGR:
    case IA64_OP_MOV_GRPR:
    case IA64_OP_MOV_PR_ROT_IMM:
    case IA64_OP_MOV_IP:
    case IA64_OP_MOV_CURRENT_IP:
    case IA64_OP_MF:
    case IA64_OP_MF_A:
    case IA64_OP_BR_CLOOP:
    case IA64_OP_ALLOC:
        if (through)
            return false;
        break;
    case IA64_OP_BR_COND:
    case IA64_OP_BR_CALL:
    case IA64_OP_BR_RET:
    case IA64_OP_BR_INDIRECT:
    case IA64_OP_BR_CALL_INDIRECT:
    case IA64_OP_BRL_COND:
    case IA64_OP_BRL_CALL:
        if (through)
            return !i->qp;
        break;
    default:
        break;
    }
    if (f == IA64_FAMILY_BRANCH || (f == IA64_FAMILY_SYSTEM && i->opcode != IA64_OP_NOP))
        return true;
    switch (i->opcode) {
    case IA64_OP_CHK_A:
    case IA64_OP_CHK_A_CLR:
    case IA64_OP_FC:
    case IA64_OP_FC_I:
    case IA64_OP_SYNC_I:
        return true;
    default:
        return false;
    }
}
static struct tier0_key entry_key(const struct vcpu *v, const struct ia64_translation *fetch)
{
    /* Runtime homes use BOF/rrb; CFM shape and PSR without its run-time
     * fields (CODE_PSR_RUNTIME) retain every static premise. The alias IP
     * preserves br.cond and mov-IP bit 63 semantics. */
    bool virt = v->s.psr & IA64_PSR_IT;
    return (struct tier0_key){.gpa = fetch ? fetch->pa : v->s.ip & ~(UINT64_C(1) << 63),
                              .ip = v->s.ip,
                              .fetch_rr = virt ? v->s.rr[v->s.ip >> 61] : 0,
                              .fetch_pta = virt ? v->s.cr[CR_PTA] : 0,
                              .fetch_pte = fetch ? fetch->pte : 0,
                              .cfm = v->s.cfm & ~TIER0_CFM_ROTATION_MASK,
                              .psr = v->s.psr & ~CODE_PSR_RUNTIME,
                              .slot = v->s.slot,
                              .abi = TIER0_ABI_VERSION,
                              .checking = v->cfg.engine == ENGINE_LOCKSTEP,
                              .icount = v->cfg.itc_icount,
                              .tracing = v->trace_count != 0,
                              .cpu_model = v->cfg.cpu_model,
                              .linked = upper != NULL};
}
static inline __attribute__((always_inline)) bool key_equal(const struct tier0_key *a,
                                                            const struct tier0_key *b)
{
    return a->cpu_model == b->cpu_model && a->gpa == b->gpa && a->ip == b->ip && a->cfm == b->cfm &&
           a->psr == b->psr && a->slot == b->slot && a->abi == b->abi &&
           a->checking == b->checking && a->icount == b->icount && a->tracing == b->tracing &&
           a->linked == b->linked && a->fetch_rr == b->fetch_rr && a->fetch_pta == b->fetch_pta &&
           a->fetch_pte == b->fetch_pte;
}
/* Diagnostic counting deliberately retains the
 * operand-byte approximation for comparable baselines. */
#define BOFM_SIZE (1u << 18)
#define BOFM_PROBES 64u
struct bofm {
    struct tier0_key k;
    uint64_t bofs[2];
    uint64_t translations;
    bool used, stacked;
};
static struct bofm *bofm_tab;
static uint64_t bofm_translations, bofm_overflow;
static bool bofm_same(const struct tier0_key *a, const struct tier0_key *b)
{
    return a->cpu_model == b->cpu_model && a->gpa == b->gpa && a->ip == b->ip && a->cfm == b->cfm &&
           a->psr == b->psr && a->slot == b->slot && a->abi == b->abi &&
           a->checking == b->checking && a->icount == b->icount && a->tracing == b->tracing &&
           a->fetch_rr == b->fetch_rr && a->fetch_pta == b->fetch_pta &&
           a->fetch_pte == b->fetch_pte;
}
static bool bofm_uses_stacked(const struct tier0_region *r)
{
    for (unsigned i = 0; i < r->count; ++i) {
        const Ia64Instruction *in = &r->insns[i];
        enum ia64_family f = ia64_opcode_info(in->opcode)->family;
        if (f == IA64_FAMILY_BRANCH || f == IA64_FAMILY_FP || f == IA64_FAMILY_NONE)
            continue;
        const IA64CommonOperands *c = &in->operands.common;
        if (c->destination >= 32 || c->source1 >= 32 || c->source2 >= 32)
            return true;
    }
    return false;
}
static void bofm_note(const struct tier0_key *key, const struct tier0_region *r, unsigned bof)
{
    if (!IA64VMM_TIER0_STATS)
        return;
    if (!bofm_tab) {
        bofm_tab = hal_alloc_pages((BOFM_SIZE * sizeof(struct bofm) + 4095) / 4096, HAL_PAGE_ZERO);
        if (!bofm_tab)
            return;
    }
    ++bofm_translations;
    uint64_t h = key->ip ^ (key->gpa >> 4) ^ key->cfm ^ (key->psr << 7) ^
                 ((uint64_t)key->slot << 3) ^ key->fetch_rr ^ key->fetch_pta ^ key->fetch_pte;
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    /* Bounded probing: a full table counts overflow instead of scanning
     * all of it at every translation. */
    for (unsigned n = 0; n < BOFM_PROBES; ++n) {
        struct bofm *e = &bofm_tab[(h + n) & (BOFM_SIZE - 1)];
        if (!e->used) {
            e->used = true;
            e->k = *key;
            e->stacked = bofm_uses_stacked(r);
        } else if (!bofm_same(&e->k, key))
            continue;
        e->bofs[bof / 64] |= UINT64_C(1) << (bof % 64);
        ++e->translations;
        return;
    }
    ++bofm_overflow;
}
static unsigned bofm_popcount(uint64_t bits)
{
    unsigned count = 0;
    while (bits) {
        ++count;
        bits &= bits - 1;
    }
    return count;
}
/* The diagnostic measures variants only; the product key retains its full PSR and
 * frame shape. Reuse the diagnostic distinct-key table, including its
 * explicit overflow count, without another per-translation allocation. */
static void state_variants(void)
{
    struct variant_group {
        unsigned first, count;
    };
    size_t pages = (BOFM_SIZE * sizeof(struct variant_group) + 4095) / 4096;
    struct variant_group *tab = hal_alloc_pages(pages, HAL_PAGE_ZERO);
    if (!tab) {
        LOGW("STATE_VARIANTS unavailable");
        return;
    }
    /* PSR bits in which a key differs from the first key of its group
     * when PSR is omitted. */
    uint64_t psr_bits[64] = {0};
    for (unsigned mask = 1; mask <= 3; ++mask) {
        memset(tab, 0, pages * 4096);
        uint64_t full = 0, groups = 0, max = 0, hist[5] = {0}, overflow = 0;
        for (unsigned i = 0; i < BOFM_SIZE; ++i) {
            if (!bofm_tab[i].used)
                continue;
            struct tier0_key k = bofm_tab[i].k;
            if (mask & 1)
                k.psr = 0;
            if (mask & 2)
                k.cfm = 0;
            uint64_t h = k.ip ^ (k.gpa >> 4) ^ k.cfm ^ (k.psr << 7) ^ ((uint64_t)k.slot << 3) ^
                         k.fetch_rr ^ k.fetch_pta ^ k.fetch_pte;
            h ^= h >> 33;
            h *= UINT64_C(0xff51afd7ed558ccd);
            h ^= h >> 33;
            unsigned n;
            for (n = 0; n < BOFM_SIZE; ++n) {
                struct variant_group *g = &tab[(h + n) & (BOFM_SIZE - 1)];
                if (!g->first) {
                    g->first = i + 1;
                    ++g->count;
                    break;
                }
                struct tier0_key other = bofm_tab[g->first - 1].k;
                if (mask & 1)
                    other.psr = 0;
                if (mask & 2)
                    other.cfm = 0;
                if (bofm_same(&k, &other)) {
                    ++g->count;
                    for (uint64_t d = bofm_tab[i].k.psr ^ bofm_tab[g->first - 1].k.psr;
                         mask == 1 && d; d &= d - 1)
                        ++psr_bits[__builtin_ctzll(d)];
                    break;
                }
            }
            if (n == BOFM_SIZE)
                ++overflow;
            ++full;
        }
        for (unsigned i = 0; i < BOFM_SIZE; ++i) {
            unsigned n = tab[i].count;
            if (!n)
                continue;
            ++groups;
            if (n > max)
                max = n;
            ++hist[n == 1 ? 0 : n == 2 ? 1 : n <= 7 ? 2 : n <= 31 ? 3 : 4];
        }
        LOGI("STATE_VARIANTS omit=%s full=%llu groups=%llu max=%llu hist_1=%llu hist_2=%llu "
             "hist_3_7=%llu hist_8_31=%llu hist_32p=%llu overflow=%llu source_overflow=%llu",
             mask == 1   ? "psr"
             : mask == 2 ? "cfm"
                         : "psr+cfm",
             (unsigned long long)full, (unsigned long long)groups, (unsigned long long)max,
             (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
             (unsigned long long)hist[3], (unsigned long long)hist[4], (unsigned long long)overflow,
             (unsigned long long)bofm_overflow);
    }
    char line[200];
    size_t used = 0;
    for (unsigned bit = 0; bit < 64; ++bit)
        if (psr_bits[bit] && used < sizeof(line) - 24)
            used += (size_t)vmm_snprintf(line + used, sizeof(line) - used, " %u:%llu", bit,
                                         (unsigned long long)psr_bits[bit]);
    line[used] = 0;
    LOGI("STATE_VARIANTS psr_bits%s", line);
    hal_free_pages(tab, pages);
}
static void bofm_dump(void)
{
    if (!bofm_tab)
        return;
    uint64_t keys = 0, full = 0, hist[5] = {0}, max = 0, skeys = 0, sfull = 0, retrans = 0;
    for (unsigned i = 0; i < BOFM_SIZE; ++i) {
        const struct bofm *e = &bofm_tab[i];
        if (!e->used)
            continue;
        uint64_t n = (uint64_t)bofm_popcount(e->bofs[0]) + (uint64_t)bofm_popcount(e->bofs[1]);
        ++keys;
        full += n;
        if (n > max)
            max = n;
        retrans += e->translations - n;
        hist[n == 1 ? 0 : n == 2 ? 1 : n <= 7 ? 2 : n <= 31 ? 3 : 4]++;
        if (e->stacked) {
            ++skeys;
            sfull += n;
        }
    }
    LOGI(
        "BOFM translations=%llu keys_without_bof=%llu keys_with_bof=%llu bof_duplicates=%llu "
        "retranslations_same_key=%llu max_bofs_per_key=%llu hist_1=%llu hist_2=%llu hist_3_7=%llu "
        "hist_8_31=%llu hist_32p=%llu stacked_keys_without_bof=%llu stacked_keys_with_bof=%llu overflow=%llu",
        (unsigned long long)bofm_translations, (unsigned long long)keys, (unsigned long long)full,
        (unsigned long long)(full - keys), (unsigned long long)retrans, (unsigned long long)max,
        (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
        (unsigned long long)hist[3], (unsigned long long)hist[4], (unsigned long long)skeys,
        (unsigned long long)sfull, (unsigned long long)bofm_overflow);
}

/* After the region is identified, compare its own bundles
 * not yet compared in this stay of v on its entry page. A mismatch has
 * already retired every overlapping dependent through its callback. A
 * boundary-cache entry passes its stay hint. */
static bool enter(struct vcpu *v, struct tier0_region *r, struct code_stay_hint *hint)
{
    if (!r->valid)
        return false;
    if (hint && code_dependency_reenter(v, &r->dependency, hint) && r->valid)
        return true;
    const struct code_fetch fetch = {
        .psr = r->key.psr, .rr = r->key.fetch_rr, .pta = r->key.fetch_pta, .pte = r->key.fetch_pte};
    if ((hint ? code_dependency_enter_hint(v, &r->dependency, &fetch, hint)
              : code_dependency_enter(v, &r->dependency, &fetch)) &&
        r->valid)
        return true;
    tier0_region_retire(r, false);
    return false;
}
static struct tier0_region *translate(struct vcpu *v, const struct tier0_key *entry, unsigned hash,
                                      bool *retry);
static inline __attribute__((always_inline)) struct tier0_region *
lookup_chain(struct vcpu *v, const struct tier0_key *entry, unsigned hash, bool *retry,
             struct tier0_stats *local)
{
    struct tier0_arena *a = &tier0_arena;
    for (unsigned link = a->buckets[hash]; link; link = a->regions[link - 1].hash_next) {
        struct tier0_region *r = &a->regions[link - 1];
        TIER0_ADD(local, lookup_probes, 1);
        if (r->valid && key_equal(&r->key, entry)) {
            if (!a->chunks[r->chunk].executable)
                tier0_seal(v, r->chunk);
            if (!enter(v, r, NULL)) {
                *retry = true;
                return NULL;
            }
            TIER0_ADD(local, hits, 1);
            return r;
        }
    }
    return NULL;
}
/* Translation and the locked second probe are cold. Keeping them outside
 * the entry loop avoids copying its key and reserving translation scratch
 * on every lock-free hit. */
static __attribute__((noinline)) struct tier0_region *lookup_slow(struct vcpu *v,
                                                                  const struct tier0_key *entry,
                                                                  unsigned hash, bool *retry,
                                                                  struct tier0_stats *local)
{
    code_lock();
    /* Another translator may have published while we waited. */
    struct tier0_region *r = lookup_chain(v, entry, hash, retry, local);
    if (!r && !*retry) {
        uint64_t start = IA64VMM_TIER0_STATS ? hal_counter() : 0;
        r = translate(v, entry, hash, retry);
        if (IA64VMM_TIER0_STATS)
            TIER0_ADD(local, translate_ticks, hal_counter() - start);
    }
    code_unlock();
    return r;
}
static struct tier0_region *lookup_attempt(struct vcpu *v, const struct tier0_key *entry,
                                           bool *retry, struct tier0_stats *local)
{
    unsigned hash = key_hash(entry);
    struct tier0_region *r = lookup_chain(v, entry, hash, retry, local);
    return r || *retry ? r : lookup_slow(v, entry, hash, retry, local);
}
/* A lookup miss: a new region for *entry, published and entered. */
static struct tier0_region *translate(struct vcpu *v, const struct tier0_key *entry, unsigned hash,
                                      bool *retry)
{
    struct tier0_arena *a = &tier0_arena;
    struct tier0_key key = *entry;
    struct tier0_region *r = tier0_region_allocate();
    if (!r) {
        *retry = true;
        return NULL;
    }
    *r = (struct tier0_region){
        .key = key, .dependency.invalidate = invalidate_region, .serial = ++region_serial};
    uint64_t generation = 0;
    Ia64Instruction insns[TIER0_MAX_INSNS];
    uint64_t ip = key.ip;
    unsigned slot = key.slot;
    while (r->count < TIER0_MAX_INSNS) {
        const struct ia64_bundle *bundle;
        /* All supported translations are at least 4 KiB. Stay inside this
         * physical/virtual page and never walk a future instruction's VHPT.
         * Every instruction that can change fetch context ends the region. */
        struct ia64_result result = code_get_bundle(v, key.gpa + (ip - key.ip), &bundle);
        if (result.kind != IA64_STEP_COMPLETE)
            break;
        uint64_t current = code_source_generation(key.gpa);
        if (!r->count)
            generation = current;
        else if (generation != current) {
            *retry = true;
            break;
        }
        unsigned granule = ((key.gpa + (ip - key.ip)) & 4095) >> 4;
        code_dependency_extend(&r->dependency, granule);
        Ia64Instruction i = bundle->insn[slot];
        i.address = ip;
        if (!ia64_template_info(bundle->template_code)->defined ||
            (bundle->long_instruction && slot == 2)) {
            i.valid = false;
            i.opcode = IA64_OP_ILLEGAL;
        }
        /* A global purge may release the serialized run lock. End before
         * it: no region, homes slot or helper frame may survive that wait. */
        if (i.opcode == IA64_OP_PTC_G || i.opcode == IA64_OP_PTC_GA)
            break;
        if (v->cfg.engine == ENGINE_LOCKSTEP && !tier0_check_safe(&i, v))
            break;
        unsigned steps = bundle->long_instruction && slot == 1 ? 2 : 1;
        r->slots[r->count] = steps;
        insns[r->count++] = i;
        if (ends_region(&i, !key.linked) || !i.valid)
            break;
        slot += steps;
        if (slot == 3) {
            slot = 0;
            ip += 16;
        }
        if ((ip & ~UINT64_C(4095)) != (key.ip & ~UINT64_C(4095)))
            break;
    }
    r->valid = true;
    r->source_generation = generation;
    if (*retry || !r->count || !code_dependency_publish(v, key.gpa, generation, &r->dependency)) {
        tier0_region_retire(r, false);
        *retry = r->count != 0;
        return NULL;
    }
    if (!tier0_stage(r, insns, &v->s)) {
        tier0_region_retire(r, false);
        *retry = true;
        return NULL;
    }
#ifdef IA64VMM_HOSTED
    code_validity_test_hook(key.gpa & ~UINT64_C(4095), CODE_TEST_PUBLISH);
#endif
    /* Emission may overlap DMA. The chunk seal checks again after W^X;
     * a candidate must be valid before even its cold helper entry is linked. */
    if (!code_dependency_publish(v, key.gpa, generation, &r->dependency)) {
        tier0_region_retire(r, false);
        *retry = true;
        return NULL;
    }
    code_dependency_attach(key.gpa & ~UINT64_C(4095), &r->dependency);
    unsigned at = (unsigned)(r - a->regions) + 1;
    atomic_store_explicit(&a->tags[at - 1], r->serial, memory_order_release);
    r->hash_next = a->buckets[hash];
    r->hash_previous = &a->buckets[hash];
    if (r->hash_next)
        a->regions[r->hash_next - 1].hash_previous = &r->hash_next;
    a->buckets[hash] = at;
    unsigned source = source_hash(key.gpa & ~UINT64_C(4095));
    r->source_next = a->sources[source];
    r->source_previous = &a->sources[source];
    if (r->source_next)
        a->regions[r->source_next - 1].source_previous = &r->source_next;
    a->sources[source] = at;
    bofm_note(&key, r, v->s.rse_bof);
    /* The first execution is a region entry like any later hit. */
    if (!enter(v, r, NULL)) {
        *retry = true;
        return NULL;
    }
    return r;
}
static struct tier0_region *lookup(struct vcpu *v, const struct ia64_translation *fetch,
                                   struct tier0_stats *local)
{
    TIER0_ADD(local, lookups, 1);
    struct tier0_key key = entry_key(v, fetch);
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (attempt)
            code_quiescent_fast(v);
        bool retry = false;
        struct tier0_region *r = lookup_attempt(v, &key, &retry, local);
        if (!retry)
            return r;
    }
    return NULL;
}

static void advance(struct vcpu *v, const Ia64Instruction *i, unsigned slots,
                    const struct ia64_result *result)
{
    /* rfi resumes at IPSR.ri; ordinary branches report slot 0. */
    if (result->kind == IA64_STEP_BRANCH) {
        v->s.ip = result->info;
        v->s.slot = result->next_slot;
    } else {
        v->s.slot = i->slot + slots;
        v->s.ip = i->address;
        if (v->s.slot == 3) {
            v->s.slot = 0;
            v->s.ip += 16;
        }
    }
    ++v->retired;
}
/* PSR.db reaches a region only through its fetches and inline accesses, at
 * its entry CPL: helpers, RSE references (RSC.pl) and VHPT walks (PL 0) keep
 * the interpreter's checks (ia64/debug.c, rse.c, mmu.c). Every IBR/DBR, CPL
 * or PSR.db write is a system or branch form and ends its region. The armed
 * levels (SDM Vol.2 §7.1.1) are cached per executor; tier0_run
 * entry, mov ibr/dbr (tier0_breakpoint_helper) and opaque interpreter steps
 * drop them. */
static struct {
    const struct vcpu *owner;
    bool valid;
    unsigned levels;
} debug_armed[IA64VMM_MAX_CPUS];
#if defined(IA64VMM_TIER0_TEST_HOOKS) && IA64VMM_TIER0_TEST_HOOKS
static tier0_test_hook_fn test_hook;
void tier0_set_test_hook(tier0_test_hook_fn hook)
{
    test_hook = hook;
}
static void test_event(const struct vcpu *v, enum tier0_test_stage stage)
{
    if (test_hook)
        test_hook(v, stage);
}
#else
#define test_event(v, stage) ((void)0)
#endif
static unsigned executor_index(const struct vcpu *v)
{
    if (v->cfg.index >= IA64VMM_MAX_CPUS)
        hal_panic("Tier0 executor index out of range");
    return v->cfg.index;
}
static unsigned armed_levels(const struct vcpu *v)
{
    test_event(v, TIER0_TEST_DEBUG_BEFORE);
    unsigned at = executor_index(v);
    if (debug_armed[at].owner != v || !debug_armed[at].valid) {
        debug_armed[at].levels = ia64_debug_armed(&v->s);
        debug_armed[at].owner = v;
        debug_armed[at].valid = true;
    }
    test_event(v, TIER0_TEST_DEBUG_READY);
    return debug_armed[at].levels;
}
unsigned tier0_helper(struct vcpu *v, const Ia64Instruction *i, struct tier0_execution *e)
{
    /* Inline prefixes keep their PC implicit. Every helper/fault site sees
     * the exact source PC before any semantic or lockstep side effect;
     * lockstep observes helper memory/clock through the vCPU hooks. */
    v->s.ip = i->address;
    v->s.slot = i->slot;
    TIER0_STAT(TIER0_ADD(tier0_local_stats(), helpers[i->opcode], 1));
    uint64_t old_psr = v->s.psr;
    e->result = ia64_execute(v, i);
    if (e->result.kind == IA64_STEP_FAULT) {
        ia64_trace_record(v, i);
        e->fault = true;
        e->fault_insn = i;
        return 1;
    }
    ia64_trace_record(v, i);
    if (e->result.kind == IA64_STEP_EXIT) {
        e->out = exit_at(v, e->result.exit, e->result.info, e->result.detail);
        e->stop = true;
        return 1;
    }
    /* IIPA names the last bundle that completed while PSR.ic was 1
     * (SDM Vol.2 §3.3.5.6), exactly as the interpreter records it. */
    if (old_psr & IA64_PSR_IC)
        v->last_successful_bundle = i->address;
    unsigned at = (unsigned)(i - e->region->insns);
    advance(v, i, e->region->slots[at], &e->result);
    unsigned requested = tier0_take_notification(&v->requested_exit);
    if (requested) {
        e->out = (struct vcpu_exit){requested - 1, i->address, i->slot, 0, "requested exit"};
        e->stop = true;
        return 1;
    }
    return !e->region->valid || e->result.kind == IA64_STEP_BRANCH;
}
unsigned tier0_breakpoint_helper(struct vcpu *v, const Ia64Instruction *i,
                                 struct tier0_execution *e)
{
    unsigned end = tier0_helper(v, i, e);
    debug_armed[executor_index(v)].valid = false;
    return end;
}
/* A virtual reference that missed the cache: the exact translation
 * (ia64_translate, as the helper's, so a VHPT walk happens only where the
 * helper's would) and the host pointer, or NULL for the exact helper. A
 * granted write-back RAM/firmware page with lasting rights is cached. */
void *tier0_data_pointer(struct vcpu *v, const Ia64Instruction *i, struct tier0_execution *e)
{
    v->s.ip = i->address;
    v->s.slot = i->slot;
    if (v->memory_hook || v->access_hook)
        return NULL;
    const IA64MemoryOperands *op = &i->operands.memory;
    bool store = ia64_opcode_is_store(i->opcode);
    unsigned size = tier0_access_size(i->opcode);
    uint64_t address = ia64_gr_read(&v->s, op->base);
    if (ia64_nat_read(&v->s, op->base) || (store && ia64_nat_read(&v->s, op->source)) ||
        (address & (size - 1)))
        return NULL;
    unsigned level = (v->s.psr >> 32) & 3, rights = store ? IA64_ACCESS_WRITE : IA64_ACCESS_READ;
    struct ia64_translation t;
    struct ia64_result result = ia64_translate(v, address, IA64_REF_DATA, rights, level, false, &t);
    if (result.kind != IA64_STEP_COMPLETE || !t.granted || t.deferred || t.ma != IA64_MA_WB ||
        !t.entry || (t.pa >> (IA64_IMPL_PA_MSB + 1)))
        return NULL;
    const struct gm_region *r = ia64_physical_region(v, t.pa, size);
    gpa_t page = t.pa & ~UINT64_C(4095);
    if (!r || (r->kind != GM_RAM && r->kind != GM_FIRMWARE) || (store && code_page_has_code(page)))
        return NULL;
    uint8_t *ptr = r->hva + (size_t)(t.pa - r->base);
    if ((uintptr_t)ptr & (size - 1))
        return NULL;
    /* An 8-byte aligned host page keeps every aligned guest reference of
     * it aligned on the host. */
    unsigned lasting = ia64_lasting_rights(&v->s, t.entry, level);
    if (lasting && page >= r->base && r->size >= 4096 && page - r->base <= r->size - 4096 &&
        !((uintptr_t)(r->hva + (page - r->base)) & 7))
        e->dtc[(address >> 12) & (TIER0_DTC_SLOTS - 1)] =
            (struct tier0_dtc_entry){.page = address >> 12,
                                     .owner = v,
                                     .mmu_dseq = v->mmu_dseq,
                                     .host = r->hva + (size_t)(page - r->base),
                                     .pa = page,
                                     .rights = (uint8_t)lasting,
                                     .psr_class = (uint8_t)tier0_dtc_class(v->s.psr)};
    return ptr;
}
void *tier0_memory_pointer(struct vcpu *v, const Ia64Instruction *i, struct tier0_execution *e)
{
    v->s.ip = i->address;
    v->s.slot = i->slot;
    if (v->access_hook)
        return NULL; /* SMP checks use exact helpers. */
    const IA64MemoryOperands *op = &i->operands.memory;
    unsigned size = tier0_access_size(i->opcode);
    bool store = ia64_opcode_is_store(i->opcode);
    uint64_t address = ia64_gr_read(&v->s, op->base);
    const struct gm_region *r = v->physical_region;
    if ((v->memory_hook && !e->check) || ia64_nat_read(&v->s, op->base) ||
        (store && ia64_nat_read(&v->s, op->source)) || (v->s.psr & IA64_PSR_DT) ||
        (address >> (IA64_IMPL_PA_MSB + 1)) || (address & (size - 1)) || !r ||
        (r->kind != GM_RAM && r->kind != GM_FIRMWARE) || address < r->base ||
        address - r->base >= r->size || size > r->size - (address - r->base) ||
        (store && code_page_has_code(address & ~UINT64_C(4095))))
        return NULL;
    if (e->check && !tier0_check_capture(e, r, address))
        return NULL;
    void *ptr = r->hva + (size_t)(address - r->base);
    return (uintptr_t)ptr & (size - 1) ? NULL : ptr;
}
/* Every PSR bit needs_interpreter tests; with none set and slot <= 2 it
 * finds no reason for any opcode but a global purge. */
#define TIER0_PSR_INTERP                                                                           \
    (IA64_PSR_BE | IA64_PSR_IS | IA64_PSR_DEBUG_MODES | IA64_PSR_ID | IA64_PSR_DA | IA64_PSR_DD |  \
     IA64_PSR_ED | IA64_PSR_IA)
static int needs_interpreter(const struct vcpu *v, Ia64Opcode opcode)
{
    if ((v->s.psr & (IA64_PSR_BE | IA64_PSR_IS)) || v->s.slot > 2)
        return TIER0_INTERP_UNSUPPORTED_STATE;
    /* Lower-privilege, taken-branch and single-step traps complete single
     * instructions (SDM Vol.2 §7.1, Table 8-3); PSR.db needs the interpreter
     * only with a pair armed at CPL. */
    if ((v->s.psr & IA64_PSR_DEBUG_MODES) &&
        ((v->s.psr & (IA64_PSR_LP | IA64_PSR_TB | IA64_PSR_SS)) ||
         ((armed_levels(v) >> ((v->s.psr >> 32) & 3)) & (1u | 1u << IA64_DEBUG_ARMED_DATA_SHIFT))))
        return TIER0_INTERP_DEBUG;
    /* Single-instruction PSR bits (id, da, dd, ed, ia; usually restored by
     * rfi) apply to one instruction and are then cleared by the
     * interpreter's retirement path; Tier0 regions do not model that. */
    if (v->s.psr & (IA64_PSR_ID | IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_ED | IA64_PSR_IA))
        return TIER0_INTERP_SINGLE_INSTRUCTION;
    if (opcode == IA64_OP_PTC_G || opcode == IA64_OP_PTC_GA)
        return TIER0_INTERP_GLOBAL_PURGE;
    return -1;
}
/* ia64_gr_index/ia64_pr_index (ia64/state.c) for the names the region uses,
 * at its entry: no in-region instruction changes BOF or a rotating base (all
 * of them end regions), alloc changes SOR only while every base is zero
 * (the reserved fault otherwise), and interruptions leave through the helpers.
 * Rotation applies below SOR, then the frame offset wraps the 96-register
 * ring (SDM Vol.1 §4.1.2, §4.5.1); the divisions are reduced to subtractions
 * once per entry. Physical numbering is unchanged, so RSE spill/fill, cover
 * and the rse_missing prefix see exactly the interpreter's homes. */
static __attribute__((noinline)) void
resolve_homes(const struct ia64_state *s, const struct tier0_region *r, struct tier0_homes *h)
{
    unsigned sor = ((s->cfm >> 14) & 15) * 8, bof = s->rse_bof % IA64_N_STACKED_PHYS;
    unsigned rrb = sor ? (unsigned)((s->cfm >> 18) & 127) % sor : 0;
    for (unsigned word = 0; word < 2; ++word)
        for (uint64_t used = r->gr_used[word]; used; used &= used - 1) {
            unsigned offset = (unsigned)__builtin_ctzll(used) + 64 * word, at = offset;
            if (at < sor && (at += rrb) >= sor)
                at -= sor;
            /* SOR beyond 96 is reachable only through a reserved IFS. */
            at += bof;
            while (at >= IA64_N_STACKED_PHYS)
                at -= IA64_N_STACKED_PHYS;
            h->gr_index[offset] = 32 + at;
        }
    unsigned rrb_pr = (unsigned)((s->cfm >> 32) & 63) % 48;
    for (uint64_t used = r->pr_used; used; used &= used - 1) {
        unsigned at = (unsigned)__builtin_ctzll(used) - 16 + rrb_pr;
        if (at >= 48)
            at -= 48;
        h->pr_mask[(unsigned)__builtin_ctzll(used) - 16] = UINT64_C(1) << (16 + at);
    }
}
/* A region re-entered in the same frame (every loop iteration) reuses its
 * resolved homes. Direct-mapped by serial; its tag tells collisions and
 * reused arena slots apart. Slots belong to the executor identified by cfg.index. No
 * Tier0 execution nests inside another, so a slot is stable during a call. */
#define TIER0_HOMES_CACHE 16u
struct tier0_homes_slot {
    uint64_t serial, rotation;
    unsigned bof;
    struct tier0_homes homes;
};
static const struct tier0_homes *homes_for(const struct vcpu *v, const struct tier0_region *r,
                                           struct tier0_homes_slot *cache)
{
    const struct ia64_state *s = &v->s;
    if (!(r->gr_used[0] | r->gr_used[1] | r->pr_used))
        return NULL;
    struct tier0_homes_slot *slot = &cache[r->serial % TIER0_HOMES_CACHE];
    uint64_t rotation = s->cfm & (TIER0_CFM_ROTATION_MASK | (UINT64_C(15) << 14));
    if (!r->serial || slot->serial != r->serial || slot->rotation != rotation ||
        slot->bof != s->rse_bof) {
        resolve_homes(s, r, &slot->homes);
        slot->serial = r->serial;
        slot->rotation = rotation;
        slot->bof = s->rse_bof;
    }
    return &slot->homes;
}
static void execute_region(struct vcpu *v, uint64_t budget, struct tier0_execution *e,
                           struct tier0_homes_slot *cache)
{
    /* SysV: R12=vCPU, R13=budget, R14=execution, R15=homes, all saved by
     * generated code. */
    if (tier0_arena.chunks[e->region->chunk].executable) {
        void (*fn)(struct vcpu *, uint64_t, struct tier0_execution *, const struct tier0_homes *) =
            e->region->entry;
        test_event(v, TIER0_TEST_HOMES_BEFORE);
        const struct tier0_homes *homes = homes_for(v, e->region, cache);
        test_event(v, TIER0_TEST_HOMES_READY);
        fn(v, budget, e, homes);
    } else {
        /* First-use cold code stays on the RW/NX staging chunk. The same
         * helpers retire it once while later regions fill the batch. */
        for (unsigned i = 0; i < e->region->count && budget; ++i) {
            const Ia64Instruction *insn = &e->region->insns[i];
            if (tier0_helper_for(insn)(v, insn, e))
                break;
            --budget;
        }
    }
}
static struct vcpu_exit interpreter_step(struct vcpu *v, unsigned reason, bool safe)
{
    uint64_t before = v->retired;
    bool checking = v->cfg.engine == ENGINE_LOCKSTEP && safe;
    struct vcpu_exit out =
        checking ? tier0_check_debug_boundary(v, &checking) : interp_run_fallback(v, 1);
    debug_armed[executor_index(v)].valid = false;
    uint64_t retired = v->retired - before;
    TIER0_ADD(tier0_local_stats(), interpreter_instructions[reason], retired);
    if (v->cfg.engine == ENGINE_LOCKSTEP && !checking)
        TIER0_ADD(tier0_local_stats(), unchecked_instructions, retired);
    return out;
}
static bool interpreter_safe(struct vcpu *v, gpa_t pa)
{
    const struct ia64_bundle *bundle;
    if (code_page_enter(v, pa, false).kind != IA64_STEP_COMPLETE ||
        code_get_bundle(v, pa, &bundle).kind != IA64_STEP_COMPLETE)
        return false;
    Ia64Instruction i = bundle->insn[v->s.slot];
    i.address = v->s.ip;
    return tier0_check_safe(&i, v);
}
/* Shared upper-tier policy fields are relaxed hints. */
#define UPPER_GET(field) __atomic_load_n(&(field), __ATOMIC_RELAXED)
#define UPPER_SET(field, value) __atomic_store_n(&(field), (value), __ATOMIC_RELAXED)

/* The boundary cache. Per executor (cfg.index, as
 * the homes cache), sets of two entries chosen by IP and slot; each entry
 * keeps the last lookup hit of its IP and slot (tag: IP | slot): the vCPU,
 * its instruction-side MMU sequence, and the region as arena slot + 1 (0:
 * empty) with its serial. A fill for an IP and slot without an entry in the
 * set moves the set's first entry to the second. A hit needs every key field
 * the fetch translation does not produce to match the region's key; the
 * translation itself (PA, PTE) is a function of the IP, PSR and the logged
 * I-side TLB, RR and PKR state, so in virtual mode an equal
 * instruction-side sequence gives the region's own; in physical mode it
 * depends on none of them. One entry per 64-byte line. */
#define TIER0_BOUNDARY_SETS 2048u
#define TIER0_BOUNDARY_WAYS 2u
struct tier0_boundary {
    const struct vcpu *owner;
    uint64_t iseq, tag, serial;
    struct code_stay_hint stay; /* the region's last entry in v's stay */
    uint32_t region;
} IA64VMM_ALIGNED(64);
_Static_assert(sizeof(struct tier0_boundary) == 64, "one boundary entry per cache line");
/* Links are bounded hints; collisions re-probe without losing the region's
 * shared promotion progress or pending hold. Owner and serial distinguish
 * vCPUs and reused arena records. The upper tier checks every contract. */
#define TIER0_UPPER_SLOTS 256u
struct tier0_upper_slot {
    const struct vcpu *owner;
    uint64_t serial, link, sequence;
};
/* Executor-local state by cfg.index: homes, boundary, upper and data translation
 * caches. Allocated from the VMM reserve when a vCPU of that index attaches
 * (engine_vcpu_attach), or at its first Tier0 use for fixtures that never
 * attach; freed with the arena (tier0_destroy, stopped executors). Only
 * that executor uses it after publication. */
struct tier0_executor {
    struct tier0_homes_slot homes[TIER0_HOMES_CACHE];
    struct tier0_boundary boundaries[TIER0_BOUNDARY_SETS * TIER0_BOUNDARY_WAYS];
    struct tier0_dtc_entry dtc[TIER0_DTC_SLOTS];
    struct tier0_code_span span;
    struct tier0_upper_slot upper[TIER0_UPPER_SLOTS];
};
#define EXECUTOR_PAGES ((sizeof(struct tier0_executor) + 4095) / 4096)
static struct tier0_executor *executors[IA64VMM_MAX_CPUS];
static struct tier0_executor *executor_state(const struct vcpu *v)
{
    unsigned at = executor_index(v);
    struct tier0_executor *x = __atomic_load_n(&executors[at], __ATOMIC_ACQUIRE);
    if (x)
        return x;
    bool take = !code_lock_held();
    if (take)
        code_lock();
    x = executors[at];
    if (!x && (x = hal_alloc_pages(EXECUTOR_PAGES, HAL_PAGE_ZERO)))
        __atomic_store_n(&executors[at], x, __ATOMIC_RELEASE);
    if (take)
        code_unlock();
    return x;
}
bool tier0_executor_attach(const struct vcpu *v)
{
    return executor_state(v) != NULL;
}
uint64_t tier0_store_tracked(gpa_t page, struct tier0_code_span *span)
{
    if (!code_validity_active())
        return code_page_has_code(page);
    struct code_page_span s;
    if (!code_page_span(page, &s))
        return 0;
    *span = (struct tier0_code_span){s.first, s.pages, s.bits};
    uint64_t at = (page >> 12) - s.first;
    return (__atomic_load_n(&s.bits[at / 64], __ATOMIC_ACQUIRE) >> (at % 64)) & 1;
}
void tier0_code_spans_reset(void)
{
    for (unsigned at = 0; at < IA64VMM_MAX_CPUS; ++at)
        if (executors[at])
            executors[at]->span = (struct tier0_code_span){0};
}
void tier0_executors_destroy(void)
{
    if (!code_lock_held())
        hal_panic("Tier0 executor teardown without code_lock");
    for (unsigned at = 0; at < IA64VMM_MAX_CPUS; ++at)
        if (executors[at]) {
            hal_free_pages(executors[at], EXECUTOR_PAGES);
            executors[at] = NULL;
        }
}
#ifdef IA64VMM_HOSTED
static enum tier0_boundary_mode boundary_mode;
void tier0_test_set_boundary_mode(enum tier0_boundary_mode mode)
{
    boundary_mode = mode;
}
/* Every hit against the full path: the translation must hit the TLB (no
 * walk, so no side effect) and give the region's key. */
static void boundary_check(struct vcpu *v, const struct tier0_region *r)
{
    if (v->s.rse_cfle)
        hal_panic("tier0: boundary cache hit with an incomplete frame");
    if ((v->s.psr & IA64_PSR_IT) && !ia64_tlb_lookup(v, v->s.ip, 0))
        hal_panic("tier0: boundary cache hit without an I-TLB entry");
    struct ia64_translation fetch;
    struct ia64_result translated = ia64_translate(v, v->s.ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                                                   (v->s.psr >> 32) & 3, false, &fetch);
    struct tier0_key key = entry_key(v, &fetch);
    if (translated.kind != IA64_STEP_COMPLETE || !key_equal(&key, &r->key))
        hal_panic("tier0: boundary cache hit differs from the full lookup");
}
#endif
/* The first entry of v's set: the bundles of eight consecutive aligned
 * pages map to distinct sets; slots 1 and 2 of a bundle land in other
 * quarters. */
static struct tier0_boundary *boundary_set(struct tier0_boundary *cache, const struct vcpu *v)
{
    unsigned set = (unsigned)((v->s.ip >> 4) ^ (v->s.ip >> 15) ^ ((uint64_t)v->s.slot << 9)) &
                   (TIER0_BOUNDARY_SETS - 1);
    return &cache[set * TIER0_BOUNDARY_WAYS];
}
static uint64_t boundary_tag(const struct vcpu *v)
{
    return v->s.ip | v->s.slot;
}
/* The entry of v's IP and slot in its set, or NULL. */
static struct tier0_boundary *boundary_find(struct tier0_boundary *set, const struct vcpu *v)
{
    uint64_t tag = boundary_tag(v);
    for (unsigned way = 0; way < TIER0_BOUNDARY_WAYS; ++way)
        if (set[way].tag == tag && set[way].owner == v)
            return &set[way];
    return NULL;
}
/* The region for v's boundary, or NULL (the full path runs). Read after the
 * loop top's quiescent publication: the slot's tag, not the slot, which
 * another executor may be reinitializing. A tag equal to the entry's serial
 * means the region was not retired then, so it is not reclaimed before the
 * next publication and its fields are stable. enter() validates as a
 * lookup hit does. */
static struct tier0_region *boundary_hit(struct vcpu *v, struct tier0_boundary *set,
                                         struct tier0_stats *local)
{
#ifdef IA64VMM_HOSTED
    if (boundary_mode == TIER0_BOUNDARY_OFF)
        return NULL;
#endif
    if (v->cfg.engine == ENGINE_LOCKSTEP || v->s.rse_cfle || v->trace_count)
        return NULL;
    struct tier0_arena *a = &tier0_arena;
    struct tier0_boundary *b = boundary_find(set, v);
    if (!b) {
        TIER0_ADD(local, boundary_conflicts, 1);
        return NULL;
    }
    if (!b->region || b->region > a->capacity) {
        TIER0_ADD(local, boundary_misses, 1);
        return NULL;
    }
    if (atomic_load_explicit(&a->tags[b->region - 1], memory_order_acquire) != b->serial ||
        ((v->s.psr & IA64_PSR_IT) && b->iseq != v->mmu_iseq)) {
        TIER0_ADD(local, boundary_stale, 1);
        return NULL;
    }
    struct tier0_region *r = &a->regions[b->region - 1];
    const struct tier0_key *k = &r->key;
    bool virt = (v->s.psr & IA64_PSR_IT) != 0;
    if (k->psr != (v->s.psr & ~CODE_PSR_RUNTIME) ||
        k->cfm != (v->s.cfm & ~TIER0_CFM_ROTATION_MASK) || k->abi != TIER0_ABI_VERSION ||
        k->fetch_rr != (virt ? v->s.rr[v->s.ip >> 61] : 0) ||
        k->fetch_pta != (virt ? v->s.cr[CR_PTA] : 0) || k->cpu_model != v->cfg.cpu_model ||
        k->icount != v->cfg.itc_icount || k->checking || k->tracing ||
        k->linked != (upper != NULL)) {
        TIER0_ADD(local, boundary_misses, 1);
        return NULL;
    }
#ifdef IA64VMM_HOSTED
    if (boundary_mode == TIER0_BOUNDARY_CHECKED)
        boundary_check(v, r);
#endif
    test_event(v, TIER0_TEST_BOUNDARY_HIT);
    if (!a->chunks[r->chunk].executable)
        tier0_seal(v, r->chunk);
    if (!enter(v, r, &b->stay)) {
        TIER0_ADD(local, boundary_stale, 1);
        return NULL;
    }
    TIER0_ADD(local, boundary_hits, 1);
    TIER0_ADD(local, hits, 1);
    return r;
}
static void boundary_fill(const struct vcpu *v, struct tier0_boundary *set,
                          const struct tier0_region *r)
{
#ifdef IA64VMM_HOSTED
    if (boundary_mode == TIER0_BOUNDARY_OFF)
        return;
#endif
    if (v->cfg.engine == ENGINE_LOCKSTEP || v->s.rse_cfle || v->trace_count)
        return;
    struct tier0_boundary *b = boundary_find(set, v);
    if (!b) {
        for (unsigned way = TIER0_BOUNDARY_WAYS - 1; way; --way)
            set[way] = set[way - 1];
        b = set;
    }
    *b = (struct tier0_boundary){.owner = v,
                                 .iseq = v->mmu_iseq,
                                 .tag = boundary_tag(v),
                                 .serial = r->serial,
                                 .region = (uint32_t)(r - tier0_arena.regions) + 1};
    code_stay_hint_take(v, &r->dependency, &b->stay);
}
void tier0_vcpu_state_restored(const struct vcpu *v)
{
    struct tier0_executor *x = __atomic_load_n(&executors[executor_index(v)], __ATOMIC_ACQUIRE);
    if (!x)
        return;
    for (unsigned k = 0; k < TIER0_BOUNDARY_SETS * TIER0_BOUNDARY_WAYS; ++k)
        if (x->boundaries[k].owner == v)
            x->boundaries[k] = (struct tier0_boundary){0};
    for (unsigned k = 0; k < TIER0_DTC_SLOTS; ++k)
        if (x->dtc[k].owner == v)
            x->dtc[k] = (struct tier0_dtc_entry){0};
}
/* The upper tier's counter of the head at v's ip/slot,
 * which is r's head at its lookup hit. */
static uint64_t upper_head(const struct vcpu *v)
{
    return __atomic_load_n(&upper->heads[((v->s.ip >> 2) | v->s.slot) & upper->head_mask],
                           __ATOMIC_ACQUIRE);
}
static struct tier0_upper_slot *upper_slot(struct tier0_executor *x, const struct vcpu *v,
                                           const struct tier0_region *r)
{
    struct tier0_upper_slot *u = &x->upper[r->serial % TIER0_UPPER_SLOTS];
    if (u->owner != v || u->serial != r->serial)
        *u = (struct tier0_upper_slot){.owner = v, .serial = r->serial, .sequence = ~upper_head(v)};
    return u;
}
/* Refresh this executor's link on publication or hold expiry. Only a
 * changed head sequence or an aged hold releases shared pending work. */
static void upper_sync(struct vcpu *v, struct tier0_region *r, gpa_t pa, struct tier0_upper_slot *u)
{
    uint64_t seq = upper_head(v);
    uint32_t hold = UPPER_GET(r->upper_hold);
    bool release =
        hold && (UPPER_GET(r->upper_seq) != seq ||
                 (uint32_t)__atomic_load_n(upper->sequence, __ATOMIC_ACQUIRE) - (hold & ~1u) >=
                     TIER0_UPPER_HOLD_RELEASE);
    if (u->sequence == seq && !release)
        return;
    uint64_t before_link = u->link;
    u->link = upper->probe(v, pa);
    u->sequence = seq;
    if (UPPER_GET(r->upper_seq) != seq)
        UPPER_SET(r->upper_seq, seq);
    /* A link that died restarts the hotness schedule. */
    if (before_link && !u->link)
        UPPER_SET(r->entries, 0);
    if (release)
        UPPER_SET(r->upper_hold, 0);
}
/* Promote at entries == threshold << k for some k >= 0. */
static bool upper_due(uint32_t entries, uint32_t threshold)
{
    if (!threshold || entries < threshold || entries % threshold)
        return false;
    uint32_t q = entries / threshold;
    return !(q & (q - 1));
}
/* Boundaries without a host counter read after a host-clock timer poll:
 * a match is taken within this many region entries plus one (asynchronous
 * delivery). */
#define TIER0_TIMER_SKIP 15u
static struct vcpu_exit run(struct vcpu *v, uint64_t budget)
{
    if (!tier0_arena_ensure())
        return exit_at(v, VCPU_EXIT_BAD_ACCESS, 0, "Tier0 arena unavailable");
    /* Upper-tier state of this call (tier0.h): the one-shot skip after a
     * before-effect exit, and whether the previous
     * region ended in a taken backward branch (its target is a loop
     * head). */
    struct tier0_stats *local = tier0_local_stats();
    /* Registered product executors retain their epoch through this entire
     * turn. Only legacy single-executor fixtures need the diagnostic pin. */
    bool diagnostic = !code_vcpu_is_registered(v);
    /* Placement is immutable during a turn; avoid resolving the executor
     * array base again at every native region entry. */
    struct tier0_executor *x = executor_state(v);
    if (!x)
        return exit_at(v, VCPU_EXIT_BAD_ACCESS, 0, "Tier0 executor state unavailable");
    struct tier0_homes_slot *homes = x->homes;
    struct tier0_boundary *boundaries = x->boundaries;
    struct tier0_dtc_entry *dtc = x->dtc;
    bool skip = false, loop_head = false;
    uint64_t skip_ip = 0;
    unsigned skip_slot = 0, timer_skip = 0;
    while (budget) {
        bool back = loop_head;
        loop_head = false;
        code_quiescent_fast(v);
        if (tier0_arena.retired)
            tier0_reclaim();
        if (upper && __atomic_load_n(upper->reclaim_due, __ATOMIC_RELAXED))
            upper->reclaim();
        unsigned requested = tier0_take_notification(&v->requested_exit);
        if (requested)
            return exit_at(v, requested - 1, 0, "requested exit");
        if (tier0_take_notification(&v->irq_kick))
            return exit_at(v, VCPU_EXIT_BUDGET, 0, "interrupt mailbox kick");
        if (ia64_break_ips)
            ia64_break_check(v);
        /* System-state writers end regions, so recheck before every cached
         * Tier0/Tier1 entry, including counters enabled by the last writer. */
        if (ia64_pmu_active(v)) {
            uint64_t before = v->retired;
            struct vcpu_exit out = interpreter_step(v, TIER0_INTERP_UNSUPPORTED_STATE, false);
            budget -= v->retired - before;
            if (out.reason != VCPU_EXIT_BUDGET)
                return out;
            continue;
        }
        int reason =
            (v->s.psr & TIER0_PSR_INTERP) || v->s.slot > 2 ? needs_interpreter(v, IA64_OP_NOP) : -1;
        if (reason == TIER0_INTERP_UNSUPPORTED_STATE) {
            uint64_t before = v->retired;
            struct vcpu_exit out = interpreter_step(v, reason, false);
            budget -= v->retired - before;
            if (out.reason != VCPU_EXIT_BUDGET)
                return out;
            continue;
        }
        /* The poll has work only for an armed timer that has not fired. A
         * host-clock poll reads the host counter; while its cache stays
         * valid (no ITM or ITC write, reset or restore since), the next
         * TIER0_TIMER_SKIP boundaries of this call skip it. Deterministic
         * and hooked clocks never cache. */
        if (v->timer_armed && !v->timer_fired) {
            if (timer_skip && v->timer_cache.valid)
                --timer_skip;
            else {
                ia64_timer_poll(v);
                timer_skip = v->timer_cache.valid ? TIER0_TIMER_SKIP : 0;
            }
        }
        if (ia64_external_requested(v) && ia64_external_pending(v)) {
            bool delivered;
            if (v->cfg.engine == ENGINE_LOCKSTEP) {
                if (!tier0_check_interrupt(v, &delivered))
                    return exit_at(v, VCPU_EXIT_DEBUG, 0, "interrupt boundary lockstep divergence");
            } else
                delivered = ia64_external_deliver(v);
            if (delivered) {
                TIER0_ADD(local, interrupts, 1);
                continue;
            }
        }
        /* SDM Vol.2 §4.1: permission/context checks precede all effects.
         * Full entry checks cover TC/TR purges and replacement, RR/PKR,
         * PTA/VHPT and PSR.it/cpl/pk without derived generation bookkeeping.
         * The boundary cache stands for the translation
         * and the lookup only where their result cannot differ. */
        struct ia64_translation fetch;
        struct tier0_boundary *cached = boundary_set(boundaries, v);
        struct tier0_region *r = reason < 0 ? boundary_hit(v, cached, local) : NULL;
        if (r)
            fetch.pa = r->key.gpa;
        else {
            TIER0_ADD(local, fetch_checks, 1);
            struct ia64_result translated =
                v->cfg.engine == ENGINE_LOCKSTEP
                    ? tier0_check_fetch(v, &fetch)
                    : ia64_translate(v, v->s.ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                                     (v->s.psr >> 32) & 3, false, &fetch);
            if (translated.kind == IA64_STEP_EXIT)
                return exit_at(v, translated.exit, translated.info, translated.detail);
            if (translated.kind == IA64_STEP_FAULT) {
                TIER0_ADD(local, fetch_faults, 1);
                v->fault = (struct ia64_fault_site){translated.fault, v->s.ip,   0,
                                                    translated.info,  v->s.slot, false};
                Ia64Instruction i = {.opcode = IA64_OP_NOP, .address = v->s.ip, .slot = v->s.slot};
                ia64_deliver_fault(v, &i, translated);
                continue;
            }
            if (reason >= 0) {
                uint64_t before = v->retired;
                struct vcpu_exit out = interpreter_step(v, reason, interpreter_safe(v, fetch.pa));
                budget -= v->retired - before;
                if (out.reason != VCPU_EXIT_BUDGET)
                    return out;
                continue;
            }
            r = lookup(v, &fetch, local);
            if (!r) {
                uint64_t before = v->retired;
                bool safe = interpreter_safe(v, fetch.pa);
                const struct ia64_bundle *bundle;
                int boundary = code_get_bundle(v, fetch.pa, &bundle).kind == IA64_STEP_COMPLETE
                                   ? needs_interpreter(v, bundle->insn[v->s.slot].opcode)
                                   : -1;
                struct vcpu_exit out = interpreter_step(v,
                                                        boundary >= 0 ? (unsigned)boundary
                                                        : safe        ? TIER0_INTERP_CACHE_FAILURE
                                                                      : TIER0_INTERP_OPAQUE_HELPER,
                                                        safe);
                budget -= v->retired - before;
                if (out.reason != VCPU_EXIT_BUDGET)
                    return out;
                continue;
            }
            boundary_fill(v, cached, r);
        }
        /* Outcome records are written by the helper/stop that selects them.
         * Only the flags and result kind are read unconditionally. Helpers
         * assign the whole result before fault/exit/advance reads; emitted
         * branches assign kind/info. Clearing unused result fields adds a
         * string-store startup per region in the general-registers build. */
        struct tier0_execution e;
        e.region = r;
        e.stats = local;
        e.check = NULL;
        e.dtc = dtc;
        e.span = &x->span;
        e.result.kind = IA64_STEP_COMPLETE;
        e.stop = false;
        e.fault = false;
        uint64_t before = v->retired;
        if (v->cfg.engine == ENGINE_LOCKSTEP) {
            e.check = tier0_check_begin(v);
            if (!e.check)
                return exit_at(v, VCPU_EXIT_DEBUG, 0, "lockstep allocation failed");
        }
        uint64_t region_budget = budget;
        /* Deterministic ITC must stop at the exact match even inside a
         * region. Polling at the following entry raises the timer vector. */
        if (v->cfg.itc_icount && v->timer_armed && !v->timer_fired) {
            uint64_t delta = v->s.cr[CR_ITM] - ia64_itc_read_unhooked(v);
            if (delta && delta < region_budget)
                region_budget = delta;
        }
        /* tier0.h: the upper tier at a lookup hit, before the region runs
         * and with `executing` clear (a synchronous compile runs here). */
        bool skipped = skip && v->s.ip == skip_ip && v->s.slot == skip_slot;
        skip = false;
        if (upper && !skipped && !(v->s.psr & upper->lower_psr)) {
            struct tier0_upper_slot *u = upper_slot(x, v, r);
            /* Linked entries discard their count on link loss. */
            if (!u->link)
                UPPER_SET(r->entries, UPPER_GET(r->entries) + 1);
            upper_sync(v, r, fetch.pa, u);
            if (!u->link && !UPPER_GET(r->upper_hold) &&
                upper_due(UPPER_GET(r->entries),
                          back ? upper->loop_threshold : upper->trace_threshold)) {
                enum tier0_upper_hot hot = upper->hot(v, fetch.pa, back);
                if (hot == TIER0_HOT_PENDING)
                    UPPER_SET(r->upper_hold,
                              TIER0_UPPER_HOLD(__atomic_load_n(upper->sequence, __ATOMIC_ACQUIRE)));
                else if (hot == TIER0_HOT_DEFERRED)
                    UPPER_SET(r->entries, 0);
                /* A compile reads code snapshots and may have retired r. */
                if (!r->valid) {
                    tier0_check_destroy(e.check);
                    continue;
                }
                /* LINKED: probe now, also when the region existed already
                 * (no publication moved the head's counter). */
                if (hot == TIER0_HOT_LINKED)
                    u->sequence = ~upper_head(v);
                upper_sync(v, r, fetch.pa, u);
            }
            uint64_t link = u->link;
            if (link) {
                struct vcpu_exit out;
                uint64_t serial = r->serial;
                unsigned at = (unsigned)(r - tier0_arena.regions);
                enum tier0_upper_status st = upper->enter(v, &link, region_budget, &out);
                /* Chaining may publish quiescence and let r be reclaimed.
                 * Only the executor's link survives that publication. */
                bool held =
                    atomic_load_explicit(&tier0_arena.tags[at], memory_order_acquire) == serial;
                u->link = link;
                /* A link that died restarts the hotness schedule. */
                if (held && !link)
                    UPPER_SET(r->entries, 0);
                if (st == TIER0_UPPER_STOP) {
                    tier0_check_destroy(e.check);
                    return out;
                }
                if (st != TIER0_UPPER_DECLINED) {
                    tier0_check_destroy(e.check);
                    budget -= v->retired - before;
                    if (st == TIER0_UPPER_RAN_PENDING) {
                        skip = true;
                        skip_ip = v->s.ip;
                        skip_slot = v->s.slot;
                    }
                    continue;
                }
                if (!held || !r->valid) {
                    tier0_check_destroy(e.check);
                    continue;
                }
            }
        }
        if (diagnostic)
            atomic_store_explicit(&tier0_arena.executing, 1, memory_order_release);
        execute_region(v, region_budget, &e, homes);
        uint64_t retired = v->retired - before;
        /* A taken branch is the last instruction the region retired; a
         * target at or below it is a back-edge. */
        loop_head = upper && e.result.kind == IA64_STEP_BRANCH && retired &&
                    e.result.info <= r->insns[retired - 1].address;
        budget -= retired;
        TIER0_ADD(local, executed_instructions, retired);
        if (r->key.psr & IA64_PSR_IT)
            TIER0_ADD(local, virtual_instructions, retired);
        if (!(r->key.psr & IA64_PSR_IC))
            TIER0_ADD(local, handler_instructions, retired);
        bool same = !e.check || tier0_check_finish(&e, v);
        if (e.check)
            tier0_check_destroy(e.check);
        if (diagnostic)
            atomic_store_explicit(&tier0_arena.executing, 0, memory_order_release);
        if (!same)
            return exit_at(v, VCPU_EXIT_DEBUG, 0, "tier0 lockstep divergence");
        if (e.stop)
            return e.out;
        if (e.fault) {
            /*
             * The helper already executed: deliver its result without repeating RSE
             * or FP completion effects (SDM Vol.2 §5.1.1).
             */
            const Ia64Instruction *i = e.fault_insn;
            bool committed = e.result.fault == IA64_FAULT_FP_TRAP;
            if (committed) {
                if (v->s.psr & IA64_PSR_IC)
                    v->last_successful_bundle = i->address;
                unsigned at = (unsigned)(i - r->insns);
                advance(v, i, r->slots[at], &e.result);
                --budget;
                TIER0_ADD(local, executed_instructions, 1);
                if (r->key.psr & IA64_PSR_IT)
                    TIER0_ADD(local, virtual_instructions, 1);
                if (!(r->key.psr & IA64_PSR_IC))
                    TIER0_ADD(local, handler_instructions, 1);
            }
            v->fault = (struct ia64_fault_site){e.result.fault, i->address, i->raw,
                                                e.result.info,  i->slot,    committed};
            ia64_deliver_fault(v, i, e.result);
        }
    }
    return exit_at(v, VCPU_EXIT_BUDGET, 0, NULL);
}
struct vcpu_exit tier0_run(struct vcpu *v, uint64_t budget)
{
    debug_armed[executor_index(v)].valid = false;
    struct vcpu_exit out = run(v, budget);
    /* Budget-only turns perform no device work in the present VMM loop.
     * HALT and requested MMIO/debug exits may expose the current page to
     * DMA while stopped: their resume starts a new page-entry interval. */
    if (out.reason != VCPU_EXIT_BUDGET)
        v->code_entry_generation = 0;
    return out;
}
void tier0_get_stats(struct tier0_stats *out)
{
    /* All counter fields are uint64_t, including opcode arrays. Gauges are
     * shared under the mutation lock; executor blocks have one writer. */
    bool take = !code_lock_held();
    if (take)
        code_lock();
    uint64_t *dst = (void *)out;
    const uint64_t *shared = (const void *)&tier0_stats;
    for (unsigned word = 0; word < sizeof(*out) / sizeof(*dst); ++word) {
        uint64_t sum = __atomic_load_n(&shared[word], __ATOMIC_RELAXED);
        for (unsigned cpu = 0; cpu < 256; ++cpu) {
            const uint64_t *src = (const void *)&executor_stats[cpu].counters;
            sum += __atomic_load_n(&src[word], __ATOMIC_RELAXED);
        }
        dst[word] = sum;
    }
    if (take)
        code_unlock();
}
void tier0_count_page_entry(unsigned slot)
{
    TIER0_ADD(&executor_stats[slot].counters, cross_page_validations, 1);
}
void tier0_dump_stats(void)
{
    code_lock();
    bofm_dump();
    if (IA64VMM_TIER0_STATS && bofm_tab)
        state_variants();
    code_unlock();
    struct tier0_stats totals;
    tier0_get_stats(&totals);
    LOGI("tier0 regions=%llu hits=%llu bytes=%llu invalidations=%llu checked=%llu unchecked=%llu",
         (unsigned long long)totals.translations, (unsigned long long)totals.hits,
         (unsigned long long)totals.bytes, (unsigned long long)totals.invalidations,
         (unsigned long long)totals.checked_regions,
         (unsigned long long)totals.unchecked_instructions);
    LOGI(
        "tier0 publications=%llu evictions=%llu arena_bytes=%llu arena_high_water=%llu protection_changes=%llu lookups=%llu cross_page_validations=%llu reclaimed=%llu",
        (unsigned long long)totals.publications, (unsigned long long)totals.evictions,
        (unsigned long long)totals.arena_bytes, (unsigned long long)totals.arena_high_water,
        (unsigned long long)totals.protection_changes, (unsigned long long)totals.lookups,
        (unsigned long long)totals.cross_page_validations, (unsigned long long)totals.reclaimed);
    LOGI(
        "tier0 boundary_hits=%llu boundary_misses=%llu boundary_conflicts=%llu boundary_stale=%llu",
        (unsigned long long)totals.boundary_hits, (unsigned long long)totals.boundary_misses,
        (unsigned long long)totals.boundary_conflicts, (unsigned long long)totals.boundary_stale);
    LOGI(
        "tier0 executed=%llu virtual=%llu handler=%llu fetch_checks=%llu fetch_faults=%llu interrupts=%llu checked_boundaries=%llu",
        (unsigned long long)totals.executed_instructions,
        (unsigned long long)totals.virtual_instructions,
        (unsigned long long)totals.handler_instructions, (unsigned long long)totals.fetch_checks,
        (unsigned long long)totals.fetch_faults, (unsigned long long)totals.interrupts,
        (unsigned long long)totals.checked_boundaries);
    static const char *const reasons[] = {"unsupported-state",  "debug",
                                          "single-instruction", "opaque-helper",
                                          "cache-failure",      "global-purge"};
    for (unsigned i = 0; i < TIER0_INTERP_REASON_COUNT; ++i)
        LOGI("tier0 interpreter reason=%s instructions=%llu", reasons[i],
             (unsigned long long)totals.interpreter_instructions[i]);
    if (!IA64VMM_TIER0_STATS)
        return;
    for (unsigned i = 0; i < IA64_OP_COUNT; ++i)
        if (totals.inlined[i] || totals.helpers[i])
            LOGI("tier0 %-18s inline=%llu helper=%llu", ia64_opcode_info(i)->name,
                 (unsigned long long)totals.inlined[i], (unsigned long long)totals.helpers[i]);
}
struct ia64_result tier0_test_instruction(struct vcpu *v, const Ia64Instruction *i)
{
    if (!tier0_arena_ensure())
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, 0, "test arena failed");
    code_lock();
    struct tier0_region *r = tier0_region_allocate();
    if (!r) {
        code_unlock();
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, 0, "test metadata failed");
    }
    *r = (struct tier0_region){.key = entry_key(v, NULL),
                               .count = 1,
                               .valid = true,
                               .slots = {i->opcode == IA64_OP_MOVL ? 2 : 1}};
    if (!tier0_stage(r, i, &v->s)) {
        tier0_region_retire(r, false);
        code_unlock();
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, 0, "test translation failed");
    }
    tier0_seal(v, r->chunk);
    struct tier0_executor *x = executor_state(v);
    code_unlock();
    if (!x) {
        tier0_region_retire(r, false);
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, 0, "test executor state failed");
    }
    struct tier0_execution e = {
        .region = r, .stats = tier0_local_stats(), .dtc = x->dtc, .span = &x->span};
    atomic_store_explicit(&executor_stats[code_counter_index()].active, 1, memory_order_release);
    execute_region(v, 1, &e, x->homes);
    atomic_store_explicit(&executor_stats[code_counter_index()].active, 0, memory_order_release);
    tier0_region_retire(r, false);
    return e.result;
}
#ifdef IA64VMM_HOSTED
bool tier0_test_mapping(const struct vcpu *v, const void **allocation, const void **entry,
                        bool *executable)
{
    if (!tier0_arena.regions)
        return false;
    /* Inspection must not insert an ITC entry through a VHPT walk. */
    if ((v->s.psr & IA64_PSR_IT) && !ia64_tlb_find(&v->s, v->s.ip, 0))
        return false;
    size_t pages = (sizeof(*v) + 4095) / 4096;
    struct vcpu *copy = hal_alloc_pages(pages, 0);
    if (!copy)
        return false;
    *copy = *v;
    struct ia64_translation fetch;
    struct ia64_result result = ia64_translate(copy, v->s.ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                                               (v->s.psr >> 32) & 3, false, &fetch);
    hal_free_pages(copy, pages);
    if (result.kind != IA64_STEP_COMPLETE)
        return false;
    struct tier0_key key = entry_key(v, &fetch);
    for (unsigned link = tier0_arena.buckets[key_hash(&key)]; link;
         link = tier0_arena.regions[link - 1].hash_next) {
        const struct tier0_region *r = &tier0_arena.regions[link - 1];
        if (key_equal(&key, &r->key)) {
            *allocation = r->allocation;
            *entry = r->entry;
            *executable = tier0_arena.chunks[r->chunk].executable;
            return true;
        }
    }
    return false;
}
#endif

void tier0_invalidate(gpa_t gpa, uint64_t len)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    tier0_invalidate_locked(gpa, len);
    if (take)
        code_unlock();
}
void tier0_flush(void)
{
    code_background_quiesce();
    bool take = !code_lock_held();
    if (take)
        code_lock();
    tier0_flush_locked();
    if (take)
        code_unlock();
    code_background_resume();
}

void tier0_diagnostics_destroy(void)
{
    if (!code_lock_held())
        hal_panic("Tier0 diagnostics teardown without code_lock");
    if (bofm_tab)
        hal_free_pages(bofm_tab, (BOFM_SIZE * sizeof(struct bofm) + 4095) / 4096);
    bofm_tab = NULL;
    bofm_translations = bofm_overflow = 0;
}
