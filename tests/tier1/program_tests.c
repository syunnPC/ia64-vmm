/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * guest program runs: every guest program runs
 * to completion under every engine (: interpreter and Tier0) and
 * compares equal with the stepping reference under the comparison relation, both
 * with the store log (lower-tier/reference GVA pairing, store-once) and without
 * it (Tier0's inline physical accesses on). Each run also satisfies the
 * program's own checks (programs.c). region_property_tests (suite "regionprops",
 * below) adds the static checks of Tier1's production-shaped regions
 * of the guest loops themselves and the steady-state counters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/engines.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/snapshot.h"
#include "tests/tier1/storelog.h"
#include "tests/tier1/provider.h"
#include "tests/tier1/bof.h"
#include "engine/interp/interp.h"
#include "engine/tier1/region.h"
#include "engine/tier1/x86enc.h"
#include "engine/hostwin/hostwin.h"
#include "engine/tier0/tier0.h"

#define BIG_BUDGET UINT64_C(50000000)

static void check_program(const struct guest_program *p, const struct guest_params *q,
                          const struct vcpu *v, const struct vcpu_exit *e, const char *who)
{
    char why[256];
    if (!guest_check(p, q, v, e, t1h_logs_of(v), why, sizeof(why))) {
        fprintf(stderr, "programs: %s under %s: %s\n", p->name, who, why);
        T1H_CHECK(!"guest program check failed");
    }
}

static void tnest_region_check(const struct guest_program *p, const struct ia64_state *s);

void program_tests(void);
void program_tests(void)
{
    struct vcpu_config cfg;
    static struct t1h_logs ref_logs, logs;
    static struct snapshot snap;
    static struct run_result reference, actual;
    snapshot_init(&snap);
    result_init(&reference);
    result_init(&actual);
    unsigned runs = 0;
    for (unsigned i = 0; i < guest_program_count; ++i) {
        const struct guest_program *p = &guest_programs[i];
        struct guest_params q = guest_completion_params(p);
        cfg = guest_vcpu_config(p);
        struct vcpu *ref = vcpu_create(&cfg), *v = vcpu_create(&cfg);
        T1H_CHECK(ref && v);
        t1h_bind(ref, &ref_logs);
        t1h_bind(v, &logs);
        bool speed = p->kind == GUEST_SPEED;
        guest_setup(p, &q, ref);
        snapshot_take(&snap, ref);

        /* The reference: stepped with the store log; speed (9M instructions)
         * as one interp_run, it has no data accesses. */
        ref_logs.stores_on = !speed;
        snapshot_restore(&snap, ref);
        struct vcpu_exit e = speed ? interp_run(ref, BIG_BUDGET) : ref_run(ref, BIG_BUDGET, NULL);
        check_program(p, &q, ref, &e, "the reference");
        T1H_CHECK(t1h_logs_ok(&ref_logs));
        result_take(&reference, &e, ref);

        for (unsigned k = 0; k < t1h_engine_count(); ++k) {
            const struct t1h_engine *engine = t1h_engine_at(k);
            struct t1h_engine_vcpu ev;
            t1h_engine_bind(&ev, engine, v);
            for (unsigned with_stores = 0; with_stores <= !speed; ++with_stores) {
                logs.stores_on = with_stores;
                snapshot_restore(&snap, v);
                t1h_engine_restored(&ev);
                e = t1h_engine_run(&ev, BIG_BUDGET);
                char label[96];
                snprintf(label, sizeof(label), "%s/%s%s", p->name, engine->name,
                         with_stores ? "+stores" : "");
                check_program(p, &q, v, &e, label);
                result_take(&actual, &e, v);
                if (result_compare(&reference, &actual, with_stores, label)) {
                    fprintf(stderr, "programs: %s differs from the reference\n", label);
                    T1H_CHECK(!"guest run differs from the reference");
                }
                ++runs;
            }
            if (p->kind == GUEST_LOOP_NEST && !strcmp(engine->name, "tier0+tier1")) {
                /* Production regions at the product thresholds. */
                t1h_tier0t1_set_flags(0);
                t1_test_set_thresholds(64, 1024);
                logs.stores_on = false;
                snapshot_restore(&snap, v);
                t1h_engine_restored(&ev);
                e = t1h_engine_run(&ev, BIG_BUDGET);
                check_program(p, &q, v, &e, "loop_nest/tier0+tier1 production");
                result_take(&actual, &e, v);
                T1H_CHECK(!result_compare(&reference, &actual, false,
                                          "loop_nest/tier0+tier1 production"));
                tnest_region_check(p, &snap.v.s);
                t1_test_set_thresholds(0, 0);
                t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
                ++runs;
            }
            storelog_detach(v);
            t1h_engine_release(&ev);
        }
        printf(
            "programs: %-16s %9llu instructions, %3zu stores (%llu paired at the stepped boundary, "
            "%llu in the callback): reference, %u engine(s) equal\n",
            p->name, (unsigned long long)reference.v.retired, reference.logs.store_n,
            (unsigned long long)reference.logs.paired_stepped,
            (unsigned long long)reference.logs.paired_callback, t1h_engine_count());
        storelog_detach(ref);
        t1h_unbind(ref);
        t1h_unbind(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
    }
    printf("programs: %u programs, %u engine runs equal to the reference\n", guest_program_count,
           runs);
    result_free(&reference);
    result_free(&actual);
    snapshot_free(&snap);
}

/* ---- direct access and register residency on production regions ----
 *
 * For each guest loop: the region at the loop header is compiled with the
 * production flags (0: no verification counters, no store log), and
 *   static   the bytes between the loop's first and last block are decoded
 *            with x86enc's length decoder: no call, no ret; every memory
 *            operand is one of the 64-bit [r12 + requested_exit] (the
 *            checkpoint: requested_exit and irq_kick),
 *            [r14 + site[k].tag / .delta] (the window check),
 *            [r14 + point] (exit blocks), [r14 + spill[s]], or a guest access
 *            [rGVA + r11] at a site's host offset, preceded by the tag check
 *            (mov, shr w, cmp [site.tag], jne, [test, jne], mov r11 <-
 *            [site.delta]); no spill slot is loaded (reloaded) in the loop,
 *            and no spill slot stored in the loop holds a main loop value
 *            in any frame of the region;
 *   alloc    the allocation report: every main loop value is loop-carried
 *            in a host register (register residency);
 *   dynamic  passes [k0, k0 + K) run under t1_run after steady state (no
 *            guest TC change, found on the reference): zero ia64_translate
 *            and probe calls from hostwin, zero fills, traps and helper
 *            exits, no compile, guest TC unchanged (direct access). Region
 *            entries, the per-entry site-cache refills (window misses) and
 *            the lower-tier steps of the pass driver are reported: they are
 *            per entry, not per iteration.
 * T1H_DUMP_CODE=prefix writes each region's code to prefix<program>.bin.
 */

#define PROP_PASSES 8u /* measured interval (gather: passes, shladd_xor: blocks of 64 iterations) */
#define PROP_MAX_MARKS 64u

struct prop_static {
    unsigned insns, guest_accesses, tag_checks, spill_loads, spill_stores, call, other;
    unsigned main_spill_stores; /* spill stores whose slot holds a main loop value in some frame */
};

static const char *element_name(const struct ia64_state *s, uint16_t e, char *buf, size_t n)
{
    if (e == T1_ELEMENT_LC) {
        snprintf(buf, n, "ar.lc");
        return buf;
    }
    if (e >= 1 && e < 128) { /* stacked elements are frame-relative */
        for (unsigned r = 1; r < 128; ++r)
            if (t1t_element_reg(s, r) == e) {
                snprintf(buf, n, "r%u", r);
                return buf;
            }
    }
    if (e >= 256 && e < 320) {
        for (unsigned p = 1; p < 64; ++p)
            if (ia64_pr_index(s, p) == (unsigned)(e - 256)) {
                snprintf(buf, n, "p%u", p);
                return buf;
            }
    }
    snprintf(buf, n, "element %u", e);
    return buf;
}

/* Main loop values: gather r14 r15 r32 r34 ar.lc; shladd_xor r16 r17 ar.lc;
 * loop-nest's inner loop r10 r14 r15 r16 ar.lc. */
static unsigned main_values(const struct guest_program *p, const struct ia64_state *s,
                            uint16_t *out)
{
    unsigned n = 0;
    if (p->kind == GUEST_LOOP_NEST) {
        static const unsigned regs[] = {10, 14, 15, 16};
        for (unsigned k = 0; k < 4; ++k)
            out[n++] = T1_ELEMENT_GR(t1t_element_reg(s, regs[k]));
    } else if (p->kind == GUEST_SHLADD_XOR) {
        out[n++] = T1_ELEMENT_GR(t1t_element_reg(s, 16));
        out[n++] = T1_ELEMENT_GR(t1t_element_reg(s, 17));
    } else {
        out[n++] = T1_ELEMENT_GR(t1t_element_reg(s, 14));
        out[n++] = T1_ELEMENT_GR(t1t_element_reg(s, 15));
        out[n++] = T1_ELEMENT_GR(t1t_element_reg(s, 32));
        out[n++] = T1_ELEMENT_GR(t1t_element_reg(s, 34));
    }
    out[n++] = T1_ELEMENT_LC;
    return n;
}

static bool is_main(const uint16_t *mv, unsigned n, uint16_t e)
{
    for (unsigned i = 0; i < n; ++i)
        if (mv[i] == e)
            return true;
    return false;
}

/* Is slot s the location of a main loop value in any frame of r? */
static bool slot_holds_main(const struct t1_region *r, unsigned slot, const uint16_t *mv,
                            unsigned n)
{
    for (unsigned i = 0; i < r->nframe; ++i) {
        const struct t1_frame_entry *f = &r->frame[i];
        if ((f->kind == T1_LOC_SPILL || f->kind == T1_LOC_SPILL_PLUS) && f->slot == slot &&
            is_main(mv, n, f->element))
            return true;
    }
    return false;
}

static bool site_at(const struct t1_region *r, uint32_t off, const struct t1_x86_decoded *d)
{
    for (unsigned i = 0; i < r->npoints; ++i) {
        const struct t1_point *P = &r->point[i];
        if (P->kind == T1_PT_SITE && P->host_offset == off)
            return d->mem.base == P->addr_reg;
    }
    return false;
}

static struct prop_static static_check(const struct guest_program *p, const struct t1_region *r,
                                       const struct ia64_state *s)
{
    struct prop_static st = {0};
    uint16_t mv[8];
    unsigned nmv = main_values(p, s, mv);
    const uint8_t *code = r->code;
    struct t1_x86_decoded last[8];
    unsigned nlast = 0;
    for (uint32_t at = r->loop_start; at < r->loop_end;) {
        struct t1_x86_decoded d;
        T1H_CHECK(t1_x86_decode(code + at, r->loop_end - at, &d));
        ++st.insns;
        if ((t1_x86_info(d.op)->attr & T1_XA_CALL) || d.op == T1_X86_RET || d.op == T1_X86_JMP_M)
            ++st.call;
        if (d.has_mem && d.op != T1_X86_LEA64) { /* lea: address arithmetic only */
            const struct t1_x86_mem *m = &d.mem;
            int32_t disp = m->disp;
            bool ok = false;
            if (m->base == T1_R12 && m->index == T1_X86_NOREG)
                ok = d.op == T1_X86_LOAD64 && disp == (int32_t)T1_VCPU_CKPT; /* checkpoint words */
            else if (m->base == T1_R14 && m->index == T1_X86_NOREG) {
                if (disp >= (int32_t)X_SITE(0) && disp < (int32_t)X_SITE(T1_SITE_SLOTS)) {
                    ok = (d.op == T1_X86_CMP64_RM && (disp - (int32_t)X_SITE(0)) % 16 == 0) ||
                         (d.op == T1_X86_LOAD64 && (disp - (int32_t)X_SITE(0)) % 16 == 8);
                    st.tag_checks += d.op == T1_X86_CMP64_RM;
                } else if (disp >= (int32_t)X_SPILL(0) && disp < (int32_t)X_SPILL(T1_SPILL_SLOTS)) {
                    unsigned slot = (unsigned)(disp - (int32_t)X_SPILL(0)) / 8;
                    if (d.op == T1_X86_LOAD64) {
                        ++st.spill_loads;
                        ok = true;
                    } else if (d.op == T1_X86_STORE64 || d.op == T1_X86_STORE64_I32) {
                        ++st.spill_stores;
                        st.main_spill_stores += slot_holds_main(r, slot, mv, nmv);
                        ok = true;
                    }
                } else {
                    ok = disp == (int32_t)X_POINT && d.op == T1_X86_STORE32_I32;
                }
            } else if (m->index == T1_R11 && m->base != T1_R14 && m->base != T1_R12 &&
                       m->scale_shift == 0 && m->disp == 0 && site_at(r, at, &d)) {
                /* The guest access, preceded by its window check. */
                unsigned k = nlast;
                bool load = d.op == T1_X86_LOAD64 || d.op == T1_X86_LOAD32 ||
                            d.op == T1_X86_LOAD16ZX || d.op == T1_X86_LOAD8ZX;
                bool store = d.op == T1_X86_STORE64 || d.op == T1_X86_STORE32 ||
                             d.op == T1_X86_STORE16 || d.op == T1_X86_STORE8;
                bool delta = k >= 1 && last[k - 1].op == T1_X86_LOAD64 &&
                             last[k - 1].mem.base == T1_R14 &&
                             (last[k - 1].mem.disp - (int32_t)X_SITE(0)) % 16 == 8;
                bool check = false;
                if (delta) {
                    unsigned j = k - 1; /* before the delta load */
                    if (j >= 2 && last[j - 1].op == T1_X86_JCC + T1_CC_NE &&
                        last[j - 2].op == T1_X86_TEST64_RI)
                        j -= 2; /* alignment test */
                    check = j >= 4 && last[j - 1].op == T1_X86_JCC + T1_CC_NE &&
                            last[j - 2].op == T1_X86_CMP64_RM && last[j - 2].mem.base == T1_R14 &&
                            last[j - 2].mem.disp + 8 == last[k - 1].mem.disp &&
                            last[j - 3].op == T1_X86_SHR64_RI && last[j - 4].op == T1_X86_MOV64_RR;
                }
                ok = (load || store) && delta && check;
                st.guest_accesses += ok;
            }
            if (!ok) {
                ++st.other;
                fprintf(
                    stderr,
                    "regionprops: %s: unexpected memory operand at loop offset %u (op %u base %u "
                    "index %u disp %d)\n",
                    p->name, (unsigned)(at - r->loop_start), d.op, m->base, m->index, disp);
            }
        }
        if (nlast == 8) {
            memmove(last, last + 1, 7 * sizeof(last[0]));
            nlast = 7;
        }
        last[nlast++] = d;
        at += d.length;
    }
    return st;
}

/* loop-nest on the product path with production regions: its outer header
 * became hot, and the region there holds the inner loop, whose checkpoint
 * resumes at the inner header. The innermost loop body
 * (loop_start/loop_end) passes the static checks above: one guest access
 * behind its tag check, no call, no reload, and no main loop value stored
 * to a spill slot. */
static void tnest_region_check(const struct guest_program *p, const struct ia64_state *s)
{
    const struct t1_region *nest = NULL;
    for (uint32_t id = 1; id <= t1_cfg.max_regions && !nest; ++id) {
        const struct t1_region *r = t1_region_get(id);
        if (!r || !r->valid || r->c.ip != T1H_CODE + 0x10 || r->c.flags)
            continue;
        for (unsigned k = 0; k < r->npoints; ++k)
            if (r->point[k].kind == T1_PT_CHECKPOINT && r->point[k].ip == T1H_CODE + 0x30)
                nest = r;
    }
    T1H_CHECK(nest && nest->entries);
    struct prop_static st = static_check(p, nest, s);
    if (st.call || st.other || st.spill_loads || st.main_spill_stores || st.guest_accesses != 1)
        fprintf(
            stderr,
            "programs: loop_nest: innermost body: %u calls, %u other, %u reloads, %u main stores, %u accesses\n",
            st.call, st.other, st.spill_loads, st.main_spill_stores, st.guest_accesses);
    T1H_CHECK(!st.call && !st.other && !st.spill_loads && !st.main_spill_stores &&
              st.guest_accesses == 1);
    printf(
        "programs: loop_nest: the outer header's region holds the inner loop (%llu entries); innermost body %u host "
        "instructions, one guest access, no reload, no main value spilled\n",
        (unsigned long long)nest->entries, st.insns);
}

/* Retired counts at the starts of marks on the stepping reference: gather the
 * header arrivals with ar.lc = n - 1 (first iteration of a pass), shladd_xor every
 * 64th header arrival; with the guest TC (both sides) at each mark. */
struct prop_mark {
    uint64_t retired;
    struct ia64_tlb_entry tc[2][IA64_TC_ENTRIES];
    unsigned tc_next[2];
};

static bool same_tc(const struct prop_mark *a, const struct prop_mark *b)
{
    if (a->tc_next[0] != b->tc_next[0] || a->tc_next[1] != b->tc_next[1])
        return false;
    for (unsigned s = 0; s < 2; ++s)
        for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i) {
            const struct ia64_tlb_entry *x = &a->tc[s][i], *y = &b->tc[s][i];
            if (x->va != y->va || x->pte != y->pte || x->itir != y->itir || x->rid != y->rid ||
                x->page_shift != y->page_shift || x->valid != y->valid)
                return false;
        }
    return true;
}

static void take_mark(struct prop_mark *m, const struct vcpu *v)
{
    m->retired = v->retired;
    memcpy(m->tc, v->s.tc, sizeof(m->tc));
    m->tc_next[0] = v->s.tc_next[0];
    m->tc_next[1] = v->s.tc_next[1];
}

static unsigned find_marks(const struct guest_program *p, const struct guest_params *q,
                           struct vcpu *ref, uint64_t header, struct prop_mark *marks, unsigned max)
{
    unsigned n = 0, arrivals = 0;
    for (uint64_t step = 0; step < 4000000 && n < max; ++step) {
        /* gather-alias alternates two code aliases (VRN 0 and 4) of one page. */
        if ((ref->s.ip << 3) == (header << 3) && ref->s.slot == 0) {
            bool mark =
                p->kind == GUEST_SHLADD_XOR ? arrivals % 64 == 0 : ref->s.ar[AR_LC] == q->n - 1;
            ++arrivals;
            if (mark)
                take_mark(&marks[n++], ref);
        }
        if (ref_run(ref, 1, NULL).reason != VCPU_EXIT_BUDGET)
            break;
    }
    return n;
}

static void mir_line(void *arg, const char *text)
{
    (void)arg;
    fprintf(stderr, "%s\n", text);
}

/* Compile attempts inside the measured interval (reported, and explained
 * with T1H_PROPS_TRACE=1). */
static unsigned interval_compiles;
static void on_compile(const struct vcpu *v, enum t1_compile_status st,
                       const struct t1_compile_report *rep, void *arg)
{
    (void)arg;
    ++interval_compiles;
    if (getenv("T1H_PROPS_TRACE"))
        fprintf(stderr, "regionprops: compile at %#llx slot %u: status %u, %u insns, %s\n",
                (unsigned long long)v->s.ip, v->s.slot, st, rep->insns,
                st == T1_COMPILED ? (rep->end_reason ? rep->end_reason : "-")
                                  : (rep->reject_reason ? rep->reject_reason : "-"));
}

/* Instructions the lower tier retired: t1_run's stepping lower tier, or,
 * on the product path (`tier0+tier1`), Tier0's regions and
 * its interpreter fallback. */
static uint64_t lower_retired(bool product, const struct t1_stats *s)
{
    if (!product)
        return s->lower_retired;
    struct tier0_stats t;
    tier0_get_stats(&t);
    uint64_t n = t.executed_instructions;
    for (unsigned i = 0; i < TIER0_INTERP_REASON_COUNT; ++i)
        n += t.interpreter_instructions[i];
    return n;
}

static void property_program(const char *name, bool product)
{
    const struct guest_program *p = guest_program(name);
    T1H_CHECK(p != NULL);
    struct guest_params q = guest_completion_params(p);
    if (p->kind == GUEST_SHLADD_XOR) {
        q.n = 64 * (PROP_PASSES + 8 + (product ? PROP_PASSES : 0)) + 64;
    } else {
        q.n = (p->flags & GUEST_STRIDE) ? 40 : 64; /* as the bench (bench.c) */
        q.bench = true;
        q.passes = PROP_PASSES + 8 + (product ? PROP_PASSES : 0);
    }
    struct vcpu_config cfg = guest_vcpu_config(p);
    struct vcpu *ref = vcpu_create(&cfg), *v = vcpu_create(&cfg);
    T1H_CHECK(ref && v);
    static struct t1h_logs ref_logs, logs;
    t1h_bind(ref, &ref_logs);
    t1h_bind(v, &logs);
    ref_logs.stores_on = logs.stores_on = false;

    /* The reference: marks and the first steady mark (no TC change from
     * there to the last mark). */
    static struct prop_mark marks[PROP_MAX_MARKS];
    guest_setup(p, &q, ref);
    uint64_t header;
    T1H_CHECK(t1h_run_to_loop_header(ref, 1000000, &header));
    struct snapshot snap;
    snapshot_init(&snap);
    snapshot_take(&snap, ref);
    unsigned nmarks = find_marks(p, &q, ref, header, marks, PROP_MAX_MARKS);
    T1H_CHECK(nmarks >= PROP_PASSES + 3);
    unsigned steady = 0;
    for (unsigned k = nmarks - 1; k > 0; --k)
        if (!same_tc(&marks[k - 1], &marks[nmarks - 1])) {
            steady = k;
            break;
        }
    unsigned k0 = steady > 4 ? steady : 4, k1 = k0 + PROP_PASSES;
    /* The product path runs each pass's first iteration in Tier0 (the
     * fall-through into the header, until header
     * splitting), except where a run starts at the header itself, as the
     * measured run does: its first iteration then runs in Tier1 and may
     * touch a page Tier1 never touched. So the product path first runs a
     * warm-up interval of the same shape (one run starting at mark k0 -
     * PROP_PASSES), then the measured one. */
    unsigned warm = k0;
    if (product) {
        k0 += PROP_PASSES;
        k1 += PROP_PASSES;
    }
    T1H_CHECK(k1 < nmarks);

    /* Tier1, production regions, from the same header state: the v1
     * harness engine, or the product path, which links the same explicitly
     * compiled region from Tier0's lookup at the header. */
    const struct t1h_engine *engine = product ? t1h_engine_tier0t1() : t1h_engine_tier1();
    const char *ename = engine->name;
    const struct t1h_provider *pv = t1h_provider_real();
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, engine, v);
    t1h_tier1_set_flags(0);
    t1h_tier0t1_set_flags(0);
    /* The product path in its production configuration (thresholds
     * 64 and 1024), not the harness's low thresholds that exercise hot(). */
    if (product)
        t1_test_set_thresholds(64, 1024);
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    struct t1_compile_report rep;
    struct t1_compile_options opt = {0};
    if (getenv("T1_UNIT_MIR_DUMP"))
        t1_test_mir_print = mir_line;
    T1H_CHECK(pv->compile(ev.ctx, &opt, &rep) == T1_COMPILED);
    t1_test_mir_print = NULL;
    const struct t1_region *r = t1_region_get(rep.region_id);
    T1H_CHECK(r != NULL && r->loop_start < r->loop_end);
    const char *dump = getenv("T1H_DUMP_CODE");
    if (dump) {
        char path[512];
        snprintf(path, sizeof(path), "%s%s.bin", dump, p->name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(r->code, 1, r->code_size, f);
            fclose(f);
            printf("regionprops: %s: code %zu bytes, loop [%u, %u), written to %s\n", p->name,
                   r->code_size, r->loop_start, r->loop_end, path);
        }
    }

    /* Static checks and the allocation report. */
    struct prop_static st = static_check(p, r, &snap.v.s);
    unsigned sites = 0, helpers = 0;
    for (unsigned i = 0; i < r->npoints; ++i) {
        sites += r->point[i].kind == T1_PT_SITE;
        helpers += r->point[i].kind == T1_PT_HELPER;
    }
    uint16_t mv[8];
    unsigned nmv = main_values(p, &snap.v.s, mv);
    char alloc[512] = "", nb[32];
    size_t used = 0;
    unsigned resident = 0;
    for (unsigned i = 0; i < rep.n_loop_carried && i < 16; ++i) {
        bool main = is_main(mv, nmv, rep.loop_carried[i]);
        bool reg = rep.loop_carried_reg[i] != T1_X86_NOREG;
        resident += main && reg;
        used += (size_t)snprintf(alloc + used, sizeof(alloc) - used, "%s%s%s=%s", used ? " " : "",
                                 element_name(&snap.v.s, rep.loop_carried[i], nb, sizeof(nb)),
                                 main ? "*" : "", reg ? "reg" : "mem");
        if (reg) {
            static const char *const names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                                  "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                                  "r12", "r13", "r14", "r15"};
            used -= 3;
            used += (size_t)snprintf(alloc + used, sizeof(alloc) - used, "%s",
                                     names[rep.loop_carried_reg[i] & 15]);
        }
        if (used >= sizeof(alloc))
            used = sizeof(alloc) - 1;
    }
    printf(
        "regionprops: %-14s static: %u insns, %u blocks, %u sites, %u helpers; loop %u host insns "
        "(%.2f per guest insn), %u guest accesses each behind a tag check, %u calls, %u spill reloads, "
        "%u spill stores (%u of a main loop value), %u other memory operands\n",
        p->name, rep.insns, rep.blocks, sites, helpers, st.insns, (double)st.insns / rep.insns,
        st.guest_accesses, st.call, st.spill_loads, st.spill_stores, st.main_spill_stores,
        st.other);
    printf(
        "regionprops: %-14s alloc: loop_value_spills %u, restore_only_spills %u; loop-carried (* = main "
        "loop value): %s; %u of %u main loop values resident\n",
        p->name, rep.loop_value_spills, rep.restore_only_spills, alloc, resident, nmv);
    /* Direct access, statically: no call, every guest access through the window. */
    T1H_CHECK(st.call == 0 && st.other == 0 && st.guest_accesses == sites && helpers == 0);
    /* Register residency for the gather body and shladd_xor. gather-load-after-store and the gather-alias
     * image change the body (S4 and its pointer r38; `mov r23 = ip` and the
     * loop-carried sum r21) and are reported only: with 11 allocatable
     * registers they spill a main loop value. */
    bool body_variant =
        (p->flags & GUEST_LOAD_AFTER_STORE) || !strcmp(p->image, "gather_alias.bin");
    if (!body_variant) {
        T1H_CHECK(st.spill_loads == 0 && st.main_spill_stores == 0);
        T1H_CHECK(rep.loop_value_spills == 0 && resident == nmv);
    } else {
        printf(
            "regionprops: %-14s register residency not met by this variant: %u spill reload(s) and %u spill store(s) "
            "of a main loop value in the loop (loop_value_spills %u)\n",
            p->name, st.spill_loads, st.main_spill_stores, rep.loop_value_spills);
    }

    /* Dynamic: to mark k0 (the product path: to the warm-up mark first),
     * then the measured interval [k0, k1). */
    struct vcpu_exit e;
    if (product) {
        e = t1h_engine_run(&ev, marks[warm].retired - v->retired);
        T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && v->retired == marks[warm].retired);
    }
    e = t1h_engine_run(&ev, marks[k0].retired - v->retired);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && v->retired == marks[k0].retired);
    struct prop_mark at0, at1;
    take_mark(&at0, v);
    T1H_CHECK(same_tc(&at0, &marks[k0]));
    struct t1_stats s0, s1;
    struct hostwin_stats h0, h1;
    pv->stats(ev.ctx, &s0);
    hostwin_get_stats(pv->hostwin(ev.ctx), &h0);
    uint64_t low0 = lower_retired(product, &s0);
    interval_compiles = 0;
    t1_test_set_compile_observer(on_compile, NULL);
    e = t1h_engine_run(&ev, marks[k1].retired - v->retired);
    t1_test_set_compile_observer(NULL, NULL);
    pv->stats(ev.ctx, &s1);
    hostwin_get_stats(pv->hostwin(ev.ctx), &h1);
    uint64_t low1 = lower_retired(product, &s1);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && v->retired == marks[k1].retired);
    take_mark(&at1, v);
    bool tc_same = same_tc(&at0, &at1);
    uint64_t iters = (uint64_t)PROP_PASSES * (p->kind == GUEST_SHLADD_XOR ? 64 : q.n);
    uint64_t retired = marks[k1].retired - marks[k0].retired;
#define D(f) ((unsigned long long)(s1.f - s0.f))
#define H(f) ((unsigned long long)(h1.f - h0.f))
    uint64_t hw_exits = 0;
    for (unsigned i = 0; i <= HW_EXIT_STALE_SLOT; ++i)
        hw_exits += h1.exits[i] - h0.exits[i];
    /* gather: the snapshot is at the second header arrival of pass 1, so mark j
     * is the start of pass j + 2; shladd_xor: mark j is header arrival 64 j + 1. */
    printf(
        "regionprops: %-14s %s dynamic: marks %u-%u (guest TC steady from mark %u), %llu iterations, %llu "
        "retired (%llu Tier1, %llu lower tier in %llu steps): translate %llu, probe %llu, fills %llu, traps %llu, "
        "hostwin exits %llu, helper exits %llu, site exits %llu, compiles %llu, guest TC %s; per entry: "
        "%llu entries (exits %llu/%llu/%llu/%llu), %llu window misses, %llu binds, %llu entry diffs; whole run "
        "before the interval: %llu translate calls, %llu fills\n",
        p->name, ename, k0, k1, steady, (unsigned long long)iters, (unsigned long long)retired,
        D(tier1_retired), (unsigned long long)(low1 - low0), D(lower_steps), H(translate_calls),
        H(probe_calls), H(fills), D(traps), (unsigned long long)hw_exits,
        D(exits_by_kind[T1_PT_HELPER]), D(site_exits), D(compiles) + D(rejects),
        tc_same ? "unchanged" : "CHANGED", D(entries), D(exits_by_kind[0]), D(exits_by_kind[1]),
        D(exits_by_kind[2]), D(exits_by_kind[3]), D(window_misses), H(binds), H(entry_diffs),
        (unsigned long long)h0.translate_calls, (unsigned long long)h0.fills);
    T1H_CHECK(tc_same);
    T1H_CHECK(h1.translate_calls == h0.translate_calls && h1.probe_calls == h0.probe_calls);
    T1H_CHECK(h1.fills == h0.fills && s1.traps == s0.traps && s1.trap_fills == s0.trap_fills &&
              hw_exits == 0);
    T1H_CHECK(s1.exits_by_kind[T1_PT_HELPER] == s0.exits_by_kind[T1_PT_HELPER] &&
              s1.site_exits == s0.site_exits);
    T1H_CHECK(s1.compiles == s0.compiles && s1.rejects == s0.rejects);
    T1H_CHECK(s1.tier1_retired - s0.tier1_retired + low1 - low0 == retired);
    /* Per entry in steady state, no
     * window miss and no bind (production regions keep their site tags
     * while the bind epoch is unchanged). gather-alias is the exception by construction: its two code
     * aliases (two regions) alternate per pass and its output goes through
     * two data aliases, so a site's one cached tag changes per entry -- at
     * most one miss per entry, still without a bind. */
    bool alias = !strcmp(p->image, "gather_alias.bin");
    uint64_t misses = s1.window_misses - s0.window_misses, entries = s1.entries - s0.entries;
    if (misses || h1.binds != h0.binds)
        printf(
            "regionprops: %-14s %s: %llu window misses and %llu binds in steady state over %llu entries%s\n",
            p->name, ename, (unsigned long long)misses, H(binds), (unsigned long long)entries,
            alias ? " (two aliases alternate: expected)" : "");
    T1H_CHECK(entries > 0 && h1.binds == h0.binds && (alias ? misses <= entries : misses == 0));
#undef D
#undef H
    /* The rest of the run, and the whole run compared with the reference
     * under the comparison relation (production regions: no store log). */
    static struct run_result reference, actual;
    result_init(&reference);
    result_init(&actual);
    e = t1h_engine_run(&ev, BIG_BUDGET);
    result_take(&actual, &e, v);
    snapshot_restore(&snap, ref);
    struct vcpu_exit re = ref_run(ref, BIG_BUDGET, NULL);
    result_take(&reference, &re, ref);
    char label[96], why[256];
    snprintf(label, sizeof(label), "regionprops %s/%s production", p->name, ename);
    T1H_CHECK(result_compare(&reference, &actual, false, label) == 0);
    T1H_CHECK(guest_check(p, &q, v, &e, &logs, why, sizeof(why)));
    printf(
        "regionprops: %-14s %s end: %llu instructions from the loop header, final state equal to the "
        "reference\n",
        p->name, ename, (unsigned long long)(v->retired - snap.v.retired));
    result_free(&reference);
    result_free(&actual);
    t1h_tier1_set_flags(T1_COMPILE_VERIFY);
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1_test_set_thresholds(0, 0);
    t1h_engine_release(&ev);
    snapshot_free(&snap);
    t1h_unbind(ref);
    t1h_unbind(v);
    vcpu_destroy(ref);
    vcpu_destroy(v);
}

void region_property_tests(void);
void region_property_tests(void)
{
    static const char *const programs[] = {
        "gather_phys",         "gather_virt",  "gather_virt_stride",      "gather_rot",
        "gather_rotp",         "gather_2page", "gather_load_after_store", "gather_alias",
        "gather_alias_phys63", "shladd_xor"};
    const char *only = getenv("T1H_PROPS"); /* one program, for diagnostics */
    for (unsigned product = 0; product < 2; ++product)
        for (unsigned i = 0; i < sizeof(programs) / sizeof(programs[0]); ++i)
            if (!only || !strcmp(only, programs[i]))
                property_program(programs[i], product);
    printf(
        "regionprops: %zu guest loops on tier1 and tier0+tier1: static checks, allocation report and steady-state "
        "counters hold\n",
        sizeof(programs) / sizeof(programs[0]));
}
