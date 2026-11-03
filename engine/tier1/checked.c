/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Checked native stores retain pre-images for single-vCPU page replay.
 * ACCESS_LOG also records native loads and replays ordered RAM accesses,
 * suppressing shadow memory effects so peer stores need no page comparison.
 * Collect native records before C can change window shadows. C accesses
 * retain their RAM/firmware region kind and use the CPU's access hooks.
 * Ordered replay uses retained source instructions; page replay uses its
 * own I-TLB. Neither polls events or clocks. Bounds fail explicitly.
 */
#include <stdarg.h>
#include "engine/tier1/region.h"
#include "engine/hostwin/hostwin.h"
#include "engine/interp/interp.h"
#include "engine/cache/cache.h"
#include "ia64/decoder.h"
#include "ia64/mmu.h"
#include "ia64/semantics.h"
#include "ia64/tlb.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
_Static_assert(sizeof(struct lockstep_access) == 56, "lockstep record layout");

#define CK_RECORDS 4096u /* native accesses and long-format walker snapshots */
#define CK_PAGES 1024u
#define PA_LIMIT (UINT64_C(1) << (IA64_IMPL_PA_MSB + 1))

struct ck_page {
    gpa_t base; /* first GPA of the page inside its region */
    uint64_t len;
    const struct gm_region *real;
    struct gm_region region; /* the shadow's view: same kind, private bytes */
    bool captured;           /* by the C side: bytes hold the page at entry */
    bool ready;              /* bytes hold the page at entry (overlay built) */
    uint8_t bytes[4096 + 16] IA64VMM_ALIGNED(16);
};

struct ck_store {
    struct lockstep_access a;
    uint64_t gva;
    bool native;
};

struct t1_check {
    struct vcpu shadow;
    struct ia64_state entry;
    uint64_t entry_retired;
    const struct t1_region *r;
    /* The vCPU's hooks at entry (Tier0's lockstep workspace under
     * ENGINE_LOCKSTEP), restored at the exit. */
    const struct gm_region *(*memory_hook)(struct vcpu *, gpa_t, unsigned);
    bool (*access_hook)(struct vcpu *, struct lockstep_access *, bool);
    bool ordered;
    uint64_t step_gva;
    const struct gm_region *physical_region;
    uint64_t physical_page;
    uint64_t timer_last;
    unsigned nrec, ring_done, nshadow, npages;
    struct ck_store rec[CK_RECORDS], sh[T1_SLOG_ENTRIES];
    /* The shadow's last RAM access in the current step. */
    bool last_valid;
    gpa_t last_gpa;
    unsigned last_size;
    bool failed;
    char why[400];
    struct ck_page page[CK_PAGES];
};

static size_t check_pages(void)
{
    return (sizeof(struct t1_check) + 4095) / 4096;
}

/* t1_test_check_mutate (tier1.h): one armed mutation. */
#ifdef IA64VMM_HOSTED
static enum t1_check_mutation mutation;
static unsigned mutation_index;
void t1_test_check_mutate(enum t1_check_mutation m, unsigned index)
{
    mutation_index = index;
    __atomic_store_n(&mutation, m, __ATOMIC_RELEASE);
}
static bool mutate(struct t1_check *c, enum t1_check_mutation m)
{
    if (__atomic_load_n(&mutation, __ATOMIC_ACQUIRE) != m ||
        (m != T1_MUTATE_STATE && c->nrec <= mutation_index))
        return false;
    return __atomic_compare_exchange_n(&mutation, &m, T1_MUTATE_NONE, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}
#else
static const unsigned mutation_index;
static bool mutate(struct t1_check *c, enum t1_check_mutation m)
{
    (void)c;
    (void)m;
    return false;
}
#endif

static void fail(struct t1_check *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(struct t1_check *c, const char *fmt, ...)
{
    if (c->failed)
        return;
    c->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vmm_vsnprintf(c->why, sizeof(c->why), fmt, ap);
    va_end(ap);
}

static struct t1_check *check_of(const struct vcpu *v)
{
    const struct t1_vcpu *tv = v->upper;
    return tv ? tv->check : NULL;
}

static struct ck_page *find_page(struct t1_check *c, gpa_t gpa)
{
    for (unsigned n = 0; n < c->npages; ++n)
        if (gpa >= c->page[n].base && gpa - c->page[n].base < c->page[n].len)
            return &c->page[n];
    return NULL;
}

/* Undoes the records [0, upto) that lie in p, newest first. */
static void undo(struct t1_check *c, struct ck_page *p, unsigned upto)
{
    for (unsigned k = upto; k-- > 0;) {
        const struct lockstep_access *a = &c->rec[k].a;
        if (a->gpa < p->base || a->gpa - p->base >= p->len)
            continue;
        memcpy(p->region.hva + (a->gpa - p->base), &a->read[0], a->width);
    }
}

/* A new page for gpa in r (RAM or firmware): the bytes now, at the
 * backing's low address bits (the 16-byte forms require aligned HVAs). */
static struct ck_page *new_page(struct t1_check *c, const struct gm_region *r, gpa_t gpa)
{
    if (c->npages == CK_PAGES) {
        fail(c, "checked pages full (%u)", CK_PAGES);
        return NULL;
    }
    struct ck_page *p = &c->page[c->npages++];
    gpa_t base = gpa & ~UINT64_C(4095);
    gpa_t first = base > r->base ? base : r->base;
    uint64_t len = r->size - (first - r->base);
    if (len > 4096 - (first - base))
        len = 4096 - (first - base);
    p->base = first;
    p->len = len;
    p->real = r;
    p->region = *r;
    p->region.base = first;
    p->region.size = len;
    p->region.hva = p->bytes + ((uintptr_t)(r->hva + (first - r->base)) & 15);
    memcpy(p->region.hva, r->hva + (first - r->base), len);
    p->captured = p->ready = false;
    return p;
}

static const struct gm_region *ram_region(gpa_t gpa, unsigned size)
{
    const struct gm_region *r = gm_lookup(gpa);
    if (!r || size > r->size - (gpa - r->base))
        return NULL;
    return r;
}

/* The real vCPU's memory hook during a checked execution: the C side's
 * accesses (step 3 above). The access itself is the real one. */
static const struct gm_region *capture_hook(struct vcpu *v, gpa_t gpa, unsigned size)
{
    struct t1_check *c = check_of(v);
    const struct gm_region *r = ram_region(gpa, size);
    if (!r)
        return NULL;
    if (r->kind != GM_RAM && r->kind != GM_FIRMWARE) {
        /* A device transaction cannot be replayed by the shadow without a
         * second side effect: no Tier1 execution may make one. */
        fail(c, "device access at %#llx by the C side of a checked Tier1 execution",
             (unsigned long long)gpa);
        return r;
    }
    if (c->ordered) {
        t1_ck_collect(v->upper);
        return r;
    }
    if (!find_page(c, gpa)) {
        struct ck_page *p = new_page(c, r, gpa);
        if (p) {
            if (!mutate(c, T1_MUTATE_CAPTURE_UNDO))
                undo(c, p, c->nrec);
            p->captured = p->ready = true;
            T1_COUNT(check_captures, 1);
        }
    }
    return r;
}

/* The shadow's memory hook: its private overlay (step 4); same region
 * kinds as the real accesses, so every kind-dependent path of the CPU
 * behaves as it would on the real memory. */
static const struct gm_region *overlay_hook(struct vcpu *v, gpa_t gpa, unsigned size)
{
    struct t1_check *c = check_of(v);
    const struct gm_region *r = ram_region(gpa, size);
    if (!r)
        return NULL;
    if (r->kind != GM_RAM && r->kind != GM_FIRMWARE) {
        fail(c, "device access at %#llx by the replay", (unsigned long long)gpa);
        return NULL;
    }
    if (c->ordered)
        return r;
    struct ck_page *p = find_page(c, gpa);
    if (!p)
        p = new_page(c, r, gpa);
    if (!p)
        return NULL;
    if (!p->ready) {
        undo(c, p, c->nrec);
        p->ready = true;
    }
    c->last_valid = true;
    c->last_gpa = gpa;
    c->last_size = size;
    return &p->region;
}

/* RAM access interception preserves the region kind and suppresses replay effects. */
static bool ordered_access(struct vcpu *v, struct lockstep_access *a, bool completed)
{
    struct t1_check *c = check_of(v);
    if (v != &c->shadow) {
        if (completed) {
            const struct t1_exec *x = &((struct t1_vcpu *)v->upper)->x;
            const struct t1_point *p = &c->r->point[x->point];
            a->ip = p->ip;
            a->slot = p->slot;
            if (c->nrec == CK_RECORDS)
                fail(c, "access log full (%u records)", CK_RECORDS);
            else
                c->rec[c->nrec++] = (struct ck_store){.a = *a};
        }
        return false;
    }
    unsigned k = c->nshadow;
    if (k == c->nrec) {
        fail(c, "access %u: unexpected kind=%u gpa=%llx width=%u at %llx:%u", k, a->kind,
             (unsigned long long)a->gpa, a->width, (unsigned long long)a->ip, a->slot);
        return true;
    }
    const struct ck_store *record = &c->rec[k];
    const struct lockstep_access *n = &record->a;
    bool same = n->kind == a->kind && n->gpa == a->gpa && n->width == a->width && n->ip == a->ip &&
                n->slot == a->slot && (!record->native || record->gva == c->step_gva);
    if (!completed) {
        a->read[0] = n->read[0];
        a->read[1] = n->read[1];
    } else {
        same &= (n->flags & ~LOCKSTEP_PRE) == a->flags;
        if (a->kind != LOCKSTEP_STORE)
            same &= !memcmp(n->read, a->read, a->width);
        if (a->kind == LOCKSTEP_STORE || (a->flags & LOCKSTEP_WROTE))
            same &= !memcmp(n->written, a->written, a->width);
        ++c->nshadow;
    }
    if (!same)
        fail(c,
             "access %u: expected kind=%u gpa=%llx width=%u at %llx:%u r=%llx w=%llx; "
             "replay kind=%u gpa=%llx width=%u at %llx:%u r=%llx w=%llx completed=%u",
             k, n->kind, (unsigned long long)n->gpa, n->width, (unsigned long long)n->ip, n->slot,
             (unsigned long long)n->read[0], (unsigned long long)n->written[0], a->kind,
             (unsigned long long)a->gpa, a->width, (unsigned long long)a->ip, a->slot,
             (unsigned long long)a->read[0], (unsigned long long)a->written[0], completed);
    return true;
}

/* No clock is read inside a Tier1 region; the shadow's stands still at the
 * last sample of the entry boundary. */
static uint64_t frozen_clock(const struct vcpu *v, uint64_t value)
{
    (void)value;
    const struct t1_check *c = check_of(v);
    return c ? c->timer_last : 0;
}

bool t1_ck_begin(struct t1_vcpu *tv, const struct t1_region *r)
{
    struct vcpu *v = tv->v;
    struct t1_check *c = tv->check;
    if (!c) {
        c = hal_alloc_pages(check_pages(), 0);
        if (!c)
            return false;
        tv->check = c;
    }
    c->r = r;
    c->ordered = (r->c.flags & T1_COMPILE_ACCESS_LOG) != 0;
    c->nrec = c->ring_done = c->nshadow = c->npages = 0;
    c->failed = false;
    c->why[0] = 0;
    c->shadow = *v;
    c->entry = v->s;
    c->entry_retired = v->retired;
    struct vcpu *s = &c->shadow;
    s->native = NULL; /* A replay clone cannot own the real hardware container. */
    s->trace = NULL;
    s->trace_count = s->trace_used = s->trace_next = 0;
    s->requested_exit = 0;
    s->irq_kick = 0;
    s->check_workspace = NULL;
    s->check_workspace_pages = 0;
    s->check_context = NULL;
    s->physical_region = NULL;
    s->memory_hook = overlay_hook;
    s->clock_hook = frozen_clock;
    c->timer_last = v->timer_last;
    c->memory_hook = v->memory_hook;
    c->access_hook = v->access_hook;
    if (c->ordered)
        v->access_hook = s->access_hook = ordered_access;
    c->physical_region = v->physical_region;
    c->physical_page = v->physical_page;
    v->memory_hook = capture_hook;
    tv->checking = true;
    return true;
}

void t1_ck_collect(struct t1_vcpu *tv)
{
    if (!tv->checking)
        return;
    struct t1_check *c = tv->check;
    const struct t1_exec *x = &tv->x;
    const struct t1_region *r = x->region;
    if (!c || !r || r != c->r)
        return;
    if (x->slog_total > T1_SLOG_ENTRIES) {
        fail(c, "store log ring overflow");
        return;
    }
    for (uint64_t k = c->ring_done; k < x->slog_total; ++k) {
        uint64_t i = k & T1_SLOG_MASK;
        uint32_t point = x->slog_point[i];
        if (point >= r->npoints || r->point[point].kind != T1_PT_SITE ||
            (!r->point[point].access_write && !c->ordered)) {
            fail(c, "access record %llu names point %u, not a logged site", (unsigned long long)k,
                 point);
            return;
        }
        if (c->nrec == CK_RECORDS) {
            fail(c, "access log full (%u records)", CK_RECORDS);
            return;
        }
        const struct t1_point *p = &r->point[point];
        gpa_t gpa;
        if (!hostwin_window_pa(tv->hw, x->slog_addr[i], &gpa)) {
            fail(c, "access record %llu: no window shadow serves %#llx", (unsigned long long)k,
                 (unsigned long long)x->slog_addr[i]);
            return;
        }
        uint64_t mask = ia64_low_mask(p->access_size * 8u);
        c->rec[c->nrec++] = (struct ck_store){
            .a = {.kind = p->access_write ? LOCKSTEP_STORE : LOCKSTEP_LOAD,
                  .width = p->access_size,
                  .slot = p->slot,
                  .flags = p->access_write ? LOCKSTEP_PRE : 0,
                  .ip = p->ip,
                  .gpa = gpa,
                  .read = {(p->access_write ? x->slog_pre[i] : x->slog_val[i]) & mask, 0},
                  .written = {p->access_write ? x->slog_val[i] & mask : 0, 0}},
            .gva = x->slog_addr[i],
            .native = true};
    }
    c->ring_done = (unsigned)x->slog_total;
}

/* The instruction at the shadow's ip/slot through its own I-TLB (a TLB
 * hit, no walk: the real execution fetched it the same way) and the code
 * bytes in memory, which no native store changes (hostwin never maps a
 * code page writable). */
static bool shadow_insn(struct t1_check *c, Ia64Instruction *out, unsigned *steps)
{
    struct vcpu *s = &c->shadow;
    uint64_t ip = s->s.ip;
    if (c->ordered) {
        for (unsigned k = 0; k < c->r->ninsns; ++k) {
            const struct t1_check_insn *ci = &c->r->check_insn[k];
            if (ci->insn.address != ip || ci->insn.slot != s->s.slot)
                continue;
            *out = ci->insn;
            *steps = ci->steps;
            c->step_gva = ia64_gr_read(&s->s, ci->memory_base);
            return true;
        }
        fail(c, "replay at %#llx:%u: no selected source instruction", (unsigned long long)ip,
             s->s.slot);
        return false;
    }
    gpa_t pa = ip & ~(UINT64_C(1) << 63);
    if (s->s.psr & IA64_PSR_IT) {
        if (!ia64_tlb_find(&s->s, ip, 0)) {
            fail(c, "replay at %#llx: no I-TLB entry", (unsigned long long)ip);
            return false;
        }
        struct ia64_translation t;
        struct ia64_result res = ia64_translate(s, ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                                                (unsigned)(s->s.psr >> 32) & 3, false, &t);
        if (res.kind != IA64_STEP_COMPLETE) {
            fail(c, "replay at %#llx: fetch does not translate", (unsigned long long)ip);
            return false;
        }
        pa = t.pa;
    }
    const struct gm_region *r = ram_region(pa & ~UINT64_C(15), 16);
    if (!r || (r->kind != GM_RAM && r->kind != GM_FIRMWARE) || s->s.slot > 2) {
        fail(c, "replay at %#llx:%u: no code at %#llx", (unsigned long long)ip, s->s.slot,
             (unsigned long long)pa);
        return false;
    }
    uint64_t low, high;
    const uint8_t *bytes = r->hva + ((pa & ~UINT64_C(15)) - r->base);
    memcpy(&low, bytes, 8);
    memcpy(&high, bytes + 8, 8);
    struct ia64_bundle b;
    ia64_decode_bundle(low, high, pa & ~UINT64_C(15), &b);
    *out = b.insn[s->s.slot];
    out->address = ip;
    *steps = b.long_instruction && s->s.slot == 1 ? 2 : 1;
    if (!ia64_template_info(b.template_code)->defined || (b.long_instruction && s->s.slot == 2) ||
        !out->valid || t1_opcode_class(out->opcode) < 0) {
        fail(c, "replay at %#llx:%u reached %s, outside the Tier1 subset", (unsigned long long)ip,
             s->s.slot, ia64_opcode_info(out->opcode)->name);
        return false;
    }
    return true;
}

/* The stores of the Tier1 subset (form.c t1_opcode_class: T1_IC_STORE). */
static unsigned store_width(Ia64Opcode op)
{
    switch (op) {
    case IA64_OP_ST1:
    case IA64_OP_ST1REL:
        return 1;
    case IA64_OP_ST2:
    case IA64_OP_ST2REL:
        return 2;
    case IA64_OP_ST4:
    case IA64_OP_ST4REL:
        return 4;
    case IA64_OP_ST8:
    case IA64_OP_ST8REL:
        return 8;
    case IA64_OP_STFS:
        return 4;
    case IA64_OP_STFD:
    case IA64_OP_STF8:
        return 8;
    default:
        return 0;
    }
}

static void replay(struct t1_check *c, uint64_t consumed)
{
    struct vcpu *s = &c->shadow;
    for (uint64_t n = 0; n < consumed && !c->failed; ++n) {
        Ia64Instruction insn;
        unsigned steps;
        if (!shadow_insn(c, &insn, &steps))
            return;
        unsigned width = c->ordered ? 0 : store_width(insn.opcode);
        uint64_t gva = 0;
        if (width && ia64_pr_read(&s->s, insn.qp))
            gva = ia64_gr_read(&s->s, insn.operands.memory.base);
        else
            width = 0;
        c->last_valid = false;
        uint64_t old_psr = s->s.psr, ip = s->s.ip;
        unsigned slot = s->s.slot;
        struct ia64_result res = ia64_execute(s, &insn);
        if (res.kind == IA64_STEP_FAULT || res.kind == IA64_STEP_EXIT) {
            fail(c, "replay of %#llx:%u (%s) %s", (unsigned long long)ip, slot,
                 ia64_opcode_info(insn.opcode)->name,
                 res.kind == IA64_STEP_FAULT ? "faulted" : "exited");
            return;
        }
        if (old_psr & IA64_PSR_IC)
            s->last_successful_bundle = insn.address;
        ++s->retired;
        if (res.kind == IA64_STEP_BRANCH) {
            s->s.ip = res.info;
            s->s.slot = res.next_slot;
        } else {
            s->s.ip = insn.address;
            s->s.slot = insn.slot + steps;
            if (s->s.slot == 3) {
                s->s.slot = 0;
                s->s.ip += 16;
            }
        }
        s->requested_exit = 0;
        if (!width)
            continue;
        if (!c->last_valid || c->last_size != width) {
            fail(c, "replay of the store at %#llx:%u made no %u-byte access",
                 (unsigned long long)ip, slot, width);
            return;
        }
        if (c->nshadow == T1_SLOG_ENTRIES) {
            fail(c, "replay stores full");
            return;
        }
        const struct ck_page *p = find_page(c, c->last_gpa);
        uint64_t value = 0;
        memcpy(&value, p->region.hva + (c->last_gpa - p->base), width);
        c->sh[c->nshadow++] = (struct ck_store){.a = {.kind = LOCKSTEP_STORE,
                                                      .width = (uint8_t)width,
                                                      .slot = (uint8_t)slot,
                                                      .ip = ip,
                                                      .gpa = c->last_gpa,
                                                      .written = {value, 0}},
                                                .gva = gva};
    }
}

static void report(const struct t1_check *c, const struct vcpu *v, uint64_t consumed,
                   const char *what)
{
    const struct t1_region *r = c->r;
    const struct vcpu *s = &c->shadow;
    LOGE(
        "tier1 lockstep divergence: region %u entry=%llx:%u consumed=%llu actual=%llx:%u reference=%llx:%u: %s",
        r->id, (unsigned long long)r->c.ip, r->c.slot, (unsigned long long)consumed,
        (unsigned long long)v->s.ip, v->s.slot, (unsigned long long)s->s.ip, s->s.slot, what);
    const struct ia64_state *e = &c->entry;
    LOGE("entry retired=%llu psr=%llx cfm=%llx bof=%u pr=%llx lc=%llx fpsr=%llx",
         (unsigned long long)c->entry_retired, (unsigned long long)e->psr,
         (unsigned long long)e->cfm, e->rse_bof, (unsigned long long)e->pr,
         (unsigned long long)e->ar[AR_LC], (unsigned long long)e->ar[AR_FPSR]);
    for (unsigned n = 0; n < 128; ++n) {
        if (e->gr[n] || e->nat[n])
            LOGE("entry physical r%u=%llx nat=%u", n, (unsigned long long)e->gr[n], e->nat[n]);
        if (n >= 2 && (e->fr[n].significand || e->fr[n].exponent || e->fr[n].sign))
            LOGE("entry physical f%u=%llx exp=%x sign=%u", n,
                 (unsigned long long)e->fr[n].significand, e->fr[n].exponent, e->fr[n].sign);
    }
    for (unsigned p = 0; p < r->npages; ++p)
        for (unsigned b = 0; b < 256; ++b) {
            if (!(r->dep[p].mask[b / 64] & (UINT64_C(1) << (b % 64))))
                continue;
            gpa_t pa = r->page[p].pa + b * 16;
            const struct gm_region *mem = ram_region(pa, 16);
            if (!mem || !mem->hva)
                continue;
            uint64_t words[2];
            memcpy(words, mem->hva + pa - mem->base, sizeof(words));
            LOGE("source ip=%llx pa=%llx bundle=%016llx %016llx",
                 (unsigned long long)(r->page[p].ip_base + b * 16), (unsigned long long)pa,
                 (unsigned long long)words[0], (unsigned long long)words[1]);
        }
    for (unsigned n = 0; n < 128; ++n)
        if (v->s.gr[n] != s->s.gr[n] || v->s.nat[n] != s->s.nat[n])
            LOGE("physical r%u tier1=%llx nat=%u interp=%llx nat=%u", n,
                 (unsigned long long)v->s.gr[n], v->s.nat[n], (unsigned long long)s->s.gr[n],
                 s->s.nat[n]);
    if (v->s.pr != s->s.pr)
        LOGE("pr tier1=%llx interp=%llx", (unsigned long long)v->s.pr, (unsigned long long)s->s.pr);
    const uint8_t *a = (const uint8_t *)&v->s, *b = (const uint8_t *)&s->s;
    for (unsigned n = 0; n < sizeof(v->s); ++n)
        if (a[n] != b[n]) {
            LOGE("state byte %u: tier1=%02x interp=%02x", n, a[n], b[n]);
            break;
        }
}

static const char *compare(struct t1_check *c, const struct vcpu *v, char *buf, size_t size)
{
    const struct vcpu *s = &c->shadow;
    if (c->failed)
        return c->why;
    if (v->retired != s->retired)
        return "retired count";
    if (memcmp(&v->s, &s->s, sizeof(v->s)))
        return "architectural state";
    if (v->last_successful_bundle != s->last_successful_bundle ||
        v->clock_offset != s->clock_offset || v->timer_last != s->timer_last ||
        v->timer_armed != s->timer_armed || v->timer_fired != s->timer_fired ||
        memcmp(v->in_service, s->in_service, sizeof(v->in_service)) ||
        v->pal_tr_selector != s->pal_tr_selector || v->pal_tr_buffer != s->pal_tr_buffer ||
        v->pal_tr_valid != s->pal_tr_valid || v->pal_tr_status != s->pal_tr_status ||
        v->pal_tr_pending != s->pal_tr_pending || v->service_port != s->service_port ||
        v->pal_state_address != s->pal_state_address ||
        v->pal_state_pending != s->pal_state_pending || v->pal_state_valid != s->pal_state_valid ||
        memcmp(v->pal_min_state, s->pal_min_state, sizeof(v->pal_min_state)))
        return "vCPU state outside struct ia64_state";
    if (c->ordered) {
        if (c->nshadow == c->nrec)
            return NULL;
        vmm_snprintf(buf, size, "access %u: %u records left after replay", c->nshadow,
                     c->nrec - c->nshadow);
        return buf;
    }
    /* Consumption: in order, every field but the pre-image. */
    for (unsigned k = 0; k < c->nrec || k < c->nshadow; ++k) {
        const struct ck_store *n = k < c->nrec ? &c->rec[k] : NULL,
                              *i = k < c->nshadow ? &c->sh[k] : NULL;
        if (n && i && n->a.kind == i->a.kind && n->a.gpa == i->a.gpa && n->a.width == i->a.width &&
            n->a.ip == i->a.ip && n->a.slot == i->a.slot && n->a.written[0] == i->a.written[0] &&
            n->gva == i->gva)
            continue;
        vmm_snprintf(buf, size,
                     "store %u: tier1%s gva=%llx gpa=%llx width=%u at %llx:%u value=%llx; "
                     "interp%s gva=%llx gpa=%llx width=%u at %llx:%u value=%llx",
                     k, n ? "" : " (none)", n ? (unsigned long long)n->gva : 0,
                     n ? (unsigned long long)n->a.gpa : 0, n ? n->a.width : 0,
                     n ? (unsigned long long)n->a.ip : 0, n ? n->a.slot : 0,
                     n ? (unsigned long long)n->a.written[0] : 0, i ? "" : " (none)",
                     i ? (unsigned long long)i->gva : 0, i ? (unsigned long long)i->a.gpa : 0,
                     i ? i->a.width : 0, i ? (unsigned long long)i->a.ip : 0, i ? i->a.slot : 0,
                     i ? (unsigned long long)i->a.written[0] : 0);
        return buf;
    }
    /* Memory: every page either side touched, the record pages included. */
    for (unsigned k = 0; k < c->nrec; ++k) {
        gpa_t gpa = c->rec[k].a.gpa;
        if (find_page(c, gpa))
            continue;
        const struct gm_region *r = ram_region(gpa, c->rec[k].a.width);
        struct ck_page *p = r ? new_page(c, r, gpa) : NULL;
        if (!p)
            return c->failed ? c->why : "record outside RAM";
        undo(c, p, c->nrec);
        p->ready = true;
    }
    if (c->failed)
        return c->why;
    for (unsigned n = 0; n < c->npages; ++n) {
        const struct ck_page *p = &c->page[n];
        const uint8_t *real = p->real->hva + (p->base - p->real->base);
        if (!memcmp(real, p->region.hva, p->len))
            continue;
        for (uint64_t j = 0; j < p->len; ++j)
            if (real[j] != p->region.hva[j]) {
                vmm_snprintf(buf, size, "memory %llx: tier1=%02x interp=%02x",
                             (unsigned long long)(p->base + j), real[j], p->region.hva[j]);
                break;
            }
        return buf;
    }
    return NULL;
}

#ifdef IA64VMM_HOSTED
static int checking;
bool t1_test_checking(void)
{
    return __atomic_load_n(&checking, __ATOMIC_SEQ_CST) != 0;
}
#endif

static bool finish(struct t1_vcpu *tv, const struct t1_point *P, uint64_t consumed);
void t1_ck_cancel(struct t1_vcpu *tv)
{
    struct t1_check *c = tv->check;
    tv->v->memory_hook = c->memory_hook;
    tv->v->access_hook = c->access_hook;
    tv->v->physical_region = c->physical_region;
    tv->v->physical_page = c->physical_page;
    tv->checking = false;
}

bool t1_ck_finish(struct t1_vcpu *tv, const struct t1_point *P, uint64_t consumed)
{
#ifdef IA64VMM_HOSTED
    __atomic_add_fetch(&checking, 1, __ATOMIC_SEQ_CST);
#endif
    bool same = finish(tv, P, consumed);
    tv->checking = false;
#ifdef IA64VMM_HOSTED
    __atomic_sub_fetch(&checking, 1, __ATOMIC_SEQ_CST);
#endif
    return same;
}

static bool finish(struct t1_vcpu *tv, const struct t1_point *P, uint64_t consumed)
{
    struct vcpu *v = tv->v;
    struct t1_check *c = tv->check;
    t1_ck_cancel(tv);
    if (mutate(c, T1_MUTATE_PRE_IMAGE))
        c->rec[mutation_index].a.read[0] ^= 0x5a;
    if (mutate(c, T1_MUTATE_STATE))
        v->s.gr[1] ^= 1;
    if (c->ordered) {
        if (mutate(c, T1_MUTATE_APPEND_RECORD)) {
            if (c->nrec == CK_RECORDS)
                fail(c, "access log full (%u records)", CK_RECORDS);
            else
                c->rec[c->nrec++] = c->rec[mutation_index];
        }
        if (mutate(c, T1_MUTATE_LOAD_VALUE))
            c->rec[mutation_index].a.read[0] ^= 1;
        if (mutate(c, T1_MUTATE_STORE_VALUE))
            c->rec[mutation_index].a.written[0] ^= 1;
        if (mutate(c, T1_MUTATE_GPA))
            c->rec[mutation_index].a.gpa ^= 0x1000;
        if (mutate(c, T1_MUTATE_DROP_RECORD)) {
            memmove(&c->rec[mutation_index], &c->rec[mutation_index + 1],
                    (c->nrec - mutation_index - 1) * sizeof(c->rec[0]));
            --c->nrec;
        }
    }
    if (!c->failed)
        replay(c, consumed);
    if (mutate(c, T1_MUTATE_STORE_VALUE))
        c->rec[mutation_index].a.written[0] ^= 1;
    if (mutate(c, T1_MUTATE_GPA))
        c->rec[mutation_index].a.gpa ^= 0x1000;
    if (mutate(c, T1_MUTATE_DROP_RECORD)) {
        memmove(&c->rec[mutation_index], &c->rec[mutation_index + 1],
                (c->nrec - mutation_index - 1) * sizeof(c->rec[0]));
        --c->nrec;
    }
    /* hostwin_resolve ran the access check of the exiting site: the same
     * translation on the shadow. */
    if (!c->failed && P->kind == T1_PT_SITE && tv->pending.translated) {
        struct ia64_translation t;
        (void)ia64_translate(&c->shadow, tv->pending.gva, IA64_REF_DATA,
                             tv->pending.write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ,
                             (unsigned)(c->shadow.s.psr >> 32) & 3, false, &t);
    }
    char buf[400];
    const char *what = compare(c, v, buf, sizeof(buf));
    if (what) {
        report(c, v, consumed, what);
        T1_COUNT(divergences, 1);
        return false;
    }
    T1_COUNT(checked, 1);
    T1_COUNT(checked_retired, consumed);
    return true;
}

void t1_ck_destroy(struct t1_vcpu *tv)
{
    if (tv->check)
        hal_free_pages(tv->check, check_pages());
    tv->check = NULL;
}
#endif
