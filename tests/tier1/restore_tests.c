/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Restoration tests require a generated-code record for every expected
 * (program, region, point, occurrence, action), tracked by ledger.h.
 * Materialized state compares in place with the reference at the same retired
 * count (observe.h); results.h compares final state and the store log.
 *
 * Drivers cover forced site/helper exits, checkpoints and kicks, guest faults,
 * store-once behavior, wrapped frame bases, recursive returns and chained
 * entries. The wrong-old-value case must fail in place even when the final
 * state matches. Window and native-code cases require the Tier1 provider.
 * The plumbing double validates drivers only; it supplies no Tier1 restoration
 * evidence. T1H_RESTORE_TRACE=1 prints region points and forced records.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/ledger.h"
#include "tests/tier1/observe.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/provider.h"
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/storelog.h"
#include "ia64vmm/guest_mem.h"
#include "engine/tier1/x86enc.h"
#include "tests/tier1/bof.h"
#include "tests/tier1/engines.h"
#include "tests/tier1/engine_tier1.h"
#include "engine/tier1/region.h"

#define BIG_BUDGET UINT64_C(50000000)
#define MAX_OCCURRENCE 3u
#define CAPTURES 512u
#define LEDGER_CAP 4096u

struct env {
    const struct t1h_provider *pv;
    const struct guest_program *p;
    struct guest_params q;
    struct vcpu *ref, *v;
    struct t1h_logs ref_logs, logs;
    struct snapshot snap; /* at the loop header */
    struct run_result reference, actual;
    void *ctx;
    struct t1_compile_report report;
    uint32_t region;
    struct t1_point_info points[T1_MAX_POINTS];
    unsigned npoints;
    struct observer obs;
    struct schedule sched;
    struct ledger *ledger;
    uint64_t header;
    unsigned records, materialized, captures;
    void *hook_arg;
};

static struct env E;

static const char *evidence(const struct t1h_provider *pv)
{
    if (!pv->evidence)
        return "plumbing double";
    return strcmp(pv->name, "tier1") ? "Tier1 on tier0+tier1" : "Tier1";
}

static void on_restore(struct t1_vcpu *tv, const struct vcpu *v, const struct t1_restore_record *r,
                       void *arg)
{
    struct env *e = arg;
    if (getenv("T1H_RESTORE_TRACE") && r->forced)
        fprintf(
            stderr,
            "trace: %s armed %d: region %u point %u occ %llu path %u outcome %u mat %d retired %llu\n",
            e->p->name, e->ledger ? e->ledger->armed : -2, r->region_id, r->point_id,
            (unsigned long long)r->occurrence, r->path, r->outcome, r->materialized,
            (unsigned long long)r->retired);
    ++e->records;
    if (r->materialized)
        ++e->materialized;
    if (e->ledger)
        ledger_record(e->ledger, r);
    observe_t1_restore(tv, v, r, &e->obs);
}

/* The machine of one program: vCPUs bound to their logs. */
static void env_machine(const struct t1h_provider *pv, const char *program,
                        const struct guest_params *q, struct ledger *ledger)
{
    struct env *e = &E;
    e->pv = pv;
    e->p = guest_program(program);
    T1H_CHECK(e->p != NULL);
    e->q = q ? *q : guest_sweep_params(e->p);
    e->ledger = ledger;
    struct vcpu_config cfg = guest_vcpu_config(e->p);
    e->ref = vcpu_create(&cfg);
    e->v = vcpu_create(&cfg);
    T1H_CHECK(e->ref && e->v);
    t1h_bind(e->ref, &e->ref_logs);
    t1h_bind(e->v, &e->logs);
    e->ref_logs.stores_on = false;
}

/* The program set up and stepped to its loop header, snapshot there.
 * bof != ENV_RESET_BOF (frame-bases): the reset state's frame
 * base replaced before the program's first instruction; with nothing dirty
 * the program's alloc then places its frame there and spills nothing. */
#define ENV_RESET_BOF UINT32_MAX
static void env_header(unsigned bof)
{
    struct env *e = &E;
    guest_setup(e->p, &e->q, e->ref);
    if (bof != ENV_RESET_BOF) {
        T1H_CHECK(!e->ref->s.rse_dirty && !e->ref->s.rse_missing && !e->ref->s.rse_cfle);
        e->ref->s.rse_bof = bof;
    }
    T1H_CHECK(t1h_run_to_loop_header(e->ref, 100000, &e->header));
    snapshot_take(&e->snap, e->ref);
}

/* The region at the header of E.snap, compiled once; its points, the
 * observer and the ledger's view of them. */
static void env_compile(const struct t1_compile_options *opt)
{
    struct env *e = &E;
    const char *program = e->p->name;
    const struct t1h_provider *pv = e->pv;
    snapshot_restore(&e->snap, e->v);
    e->ctx = pv->create(e->v);
    struct t1_compile_options verify = {.flags = T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG};
    enum t1_compile_status st = pv->compile(e->ctx, opt ? opt : &verify, &e->report);
    if (st != T1_COMPILED)
        fprintf(stderr, "restore: %s/%s: compile at %#llx: status %u (%s)\n", program, pv->name,
                (unsigned long long)e->header, st,
                e->report.reject_reason ? e->report.reject_reason : "-");
    T1H_CHECK(st == T1_COMPILED);
    e->region = e->report.region_id;
    e->npoints = pv->point_count(e->ctx, e->region);
    T1H_CHECK(e->npoints > 0 && e->npoints <= T1_MAX_POINTS);
    for (unsigned i = 0; i < e->npoints; ++i)
        T1H_CHECK(pv->point_get(e->ctx, e->region, i, &e->points[i]));
    if (getenv("T1H_RESTORE_TRACE"))
        for (unsigned i = 0; i < e->npoints; ++i)
            fprintf(
                stderr,
                "trace: %s/%s region %u point %u kind %u ip %#llx slot %u insn %u size %u write %d\n",
                program, pv->name, e->region, e->points[i].point_id, e->points[i].kind,
                (unsigned long long)e->points[i].ip, e->points[i].slot, e->points[i].insn_index,
                e->points[i].access_size, e->points[i].access_write);
    observe_init(&e->obs, CAPTURES, &e->sched);
    if (e->ledger) {
        ledger_set_program(e->ledger, program);
        ledger_set_points(e->ledger, e->points, e->npoints);
        ledger_arm(e->ledger, -1);
    }
    e->records = e->materialized = e->captures = 0;
}

static void env_open(const struct t1h_provider *pv, const char *program,
                     const struct guest_params *q, const struct t1_compile_options *opt,
                     struct ledger *ledger)
{
    env_machine(pv, program, q, ledger);
    env_header(ENV_RESET_BOF);
    env_compile(opt);
}

static void env_close(void)
{
    struct env *e = &E;
    observe_free(&e->obs);
    e->pv->destroy(e->ctx);
    storelog_detach(e->ref);
    storelog_detach(e->v);
    t1h_unbind(e->ref);
    t1h_unbind(e->v);
    vcpu_destroy(e->ref);
    vcpu_destroy(e->v);
    e->ref = e->v = NULL;
}

/* A SITE point by its instruction: a load into `dest`, or (dest 0) a store. */
static int find_site(bool load, unsigned dest)
{
    for (unsigned i = 0; i < E.npoints; ++i) {
        const struct t1_point_info *p = &E.points[i];
        if (p->kind != T1_PT_SITE || p->access_write == load)
            continue;
        Ia64Instruction in;
        T1H_CHECK(storelog_decode(&E.snap.v, p->ip, p->slot, &in));
        if (!load || in.operands.memory.destination == dest)
            return (int)i;
    }
    return -1;
}

typedef void (*arm_fn)(struct env *e, void *arg);
struct run_options {
    int entry;          /* the forced ledger entry this run arms + 1 (0: none) */
    uint64_t budget;    /* 0: to the end (BIG_BUDGET) */
    bool kick;          /* t1_kick before the run */
    bool step_to_entry; /* keep Tier0 from translating across the forced head */
    t1_entry_hook_fn hook;
    arm_fn arm;
    void *arg;
    t1_restore_fn restore; /* the restore observer (NULL: on_restore) */
};

/* One case: the run under test from the snapshot, then the reference under
 * the same (resolved) schedule. Returns final differences; *inplace gets
 * the in-place comparisons that failed. */
static unsigned run_case(const char *label, const struct run_options *o, unsigned *inplace)
{
    struct env *e = &E;
    schedule_clear(&e->sched);
    observe_reset(&e->obs);
    e->logs.stores_on = true;
    snapshot_restore(&e->snap, e->v);
    e->pv->restored(e->ctx);
    e->pv->set_restore_observer(e->ctx, o->restore ? o->restore : on_restore, e);
    e->pv->set_store_observer(e->ctx, storelog_t1_store, &e->logs);
    e->pv->set_entry_hook(e->ctx, o->hook, e);
    if (e->ledger)
        ledger_arm(e->ledger, o->entry - 1);
    if (o->arm)
        o->arm(e, o->arg);
    if (o->kick)
        e->pv->kick(e->ctx);
    uint64_t budget = o->budget ? o->budget : BIG_BUDGET;
    struct vcpu_exit x;
    uint64_t start = e->v->retired;
    do {
        uint64_t left = budget - (e->v->retired - start), slice = left;
        if (o->step_to_entry) {
            slice = e->v->s.ip == e->snap.v.s.ip && e->v->s.slot == e->snap.v.s.slot
                        ? e->report.insns
                        : 1;
            if (slice > left)
                slice = left;
        }
        x = e->pv->run(e->ctx, slice);
    } while (o->step_to_entry && x.reason == VCPU_EXIT_BUDGET && e->v->retired - start < budget);
    e->pv->set_restore_observer(e->ctx, NULL, NULL);
    e->pv->set_store_observer(e->ctx, NULL, NULL);
    e->pv->set_entry_hook(e->ctx, NULL, NULL);
    if (e->ledger)
        ledger_arm(e->ledger, -1);
    T1H_CHECK(t1h_logs_ok(&e->logs));
    result_take(&e->actual, &x, e->v);
    schedule_rearm(&e->sched);
    e->ref_logs.stores_on = true;
    snapshot_restore(&e->snap, e->ref);
    struct vcpu_exit rx = ref_run(e->ref, budget, &e->sched);
    T1H_CHECK(schedule_complete(&e->sched));
    result_take(&e->reference, &rx, e->ref);
    unsigned diff = result_compare(&e->reference, &e->actual, true, label);
    *inplace = observe_verify(&e->obs, &e->snap, e->ref, &e->sched, label);
    e->captures += e->obs.n;
    return diff;
}

static void must_pass(const char *label, const struct run_options *o)
{
    unsigned inplace;
    unsigned diff = run_case(label, o, &inplace);
    if (diff || inplace) {
        fprintf(stderr, "restore: %s: %u final and %u in-place differences\n", label, diff,
                inplace);
        T1H_CHECK(!"restoration case differs from the reference");
    }
}

/* ---- forced-exit (and rotating-pred-frames) ---------------------------------------------- */

static void arm_force(struct env *e, void *arg)
{
    const struct ledger_case *c = arg;
    T1H_CHECK(e->pv->force(e->ctx, e->region, c->point, c->occurrence,
                           ledger_force_action(c->action), NULL, NULL));
}

/* Predicated calls leave the main region before its RSE return tails.
 * Enter each promised operation at a reference-reached boundary instead. */
static void forced_rse_exit(const struct t1h_provider *pv, struct ledger *L, const char *program)
{
    struct t1_point_info targets[T1_MAX_POINTS];
    unsigned ntargets = 0, first = L->n, records = 0, captures = 0;
    env_open(pv, program, NULL, NULL, L);
    for (unsigned i = 0; i < E.npoints; ++i)
        if (E.points[i].kind == T1_PT_SITE || E.points[i].kind == T1_PT_HELPER)
            targets[ntargets++] = E.points[i];
    env_close();
    T1H_CHECK(ntargets == 7); /* six helpers and one load */
    for (unsigned i = 0; i < ntargets; ++i) {
        env_machine(pv, program, NULL, L);
        guest_setup(E.p, &E.q, E.ref);
        uint64_t steps = 0;
        while (E.ref->s.ip != targets[i].ip || E.ref->s.slot != targets[i].slot) {
            T1H_CHECK(++steps < BIG_BUDGET);
            T1H_CHECK(ref_run(E.ref, 1, NULL).reason == VCPU_EXIT_BUDGET);
        }
        E.header = E.ref->s.ip;
        snapshot_take(&E.snap, E.ref);
        env_compile(NULL);
        unsigned point = 0;
        while (point < E.npoints &&
               (E.points[point].ip != targets[i].ip || E.points[point].slot != targets[i].slot ||
                E.points[point].kind != targets[i].kind))
            ++point;
        T1H_CHECK(point < E.npoints);
        unsigned start = L->n;
        ledger_expect_force(L, E.region, &E.points[point], 1, MAX_OCCURRENCE);
        for (unsigned k = start; k < L->n; ++k) {
            const struct ledger_case *c = &L->c[k];
            char label[160];
            snprintf(label, sizeof(label), "forced-rse %s/%s point %u occurrence %llu %s", program,
                     pv->name, c->point, (unsigned long long)c->occurrence,
                     ledger_action_name(c->action));
            must_pass(label, &(struct run_options){.entry = (int)k + 1,
                                                   .step_to_entry = true,
                                                   .arm = arm_force,
                                                   .arg = &L->c[k]});
        }
        records += E.records;
        captures += E.captures;
        env_close();
    }
    T1H_CHECK(L->n - first == 27);
    printf(
        "restore: forced-rse %-16s (%s): %u points, %u cases, %u records, %u in-place captures equal\n",
        program, evidence(pv), ntargets, L->n - first, records, captures);
}

static void forced_exit(const struct t1h_provider *pv, struct ledger *L, const char *program)
{
    if (pv->evidence && guest_program(program)->kind == GUEST_RSE_SPILL) {
        forced_rse_exit(pv, L, program);
        return;
    }
    env_open(pv, program, NULL, NULL, L);
    unsigned first = L->n, sites = 0, helpers = 0;
    ledger_expect_force(L, E.region, E.points, E.npoints, MAX_OCCURRENCE);
    for (unsigned i = 0; i < E.npoints; ++i) {
        sites += E.points[i].kind == T1_PT_SITE;
        helpers += E.points[i].kind == T1_PT_HELPER;
    }
    T1H_CHECK(L->n > first);
    for (unsigned i = first; i < L->n; ++i) {
        const struct ledger_case *c = &L->c[i];
        char label[160];
        snprintf(label, sizeof(label), "forced-exit %s/%s point %u occurrence %llu %s", program,
                 pv->name, c->point, (unsigned long long)c->occurrence,
                 ledger_action_name(c->action));
        must_pass(label,
                  &(struct run_options){.entry = (int)i + 1, .arm = arm_force, .arg = &L->c[i]});
    }
    printf("restore: forced-exit %-15s (%s): %u sites, %u helpers, %u cases, %u records, "
           "%u in-place captures equal%s\n",
           program, evidence(pv), sites, helpers, L->n - first, E.records, E.captures,
           !strcmp(program, "gather_rotp") ? " (rotating-pred-frames)" : "");
    env_close();
}

/* ---- wrong-old-value ------------------------------------------------------- */

struct wrong_old_value {
    uint32_t point;
    bool corrupt;
};
static void arm_wrong_old_value(struct env *e, void *arg)
{
    const struct wrong_old_value *w = arg;
    T1H_CHECK(e->pv->force(e->ctx, e->region, w->point, 2, T1_FORCE_TRAP_EXIT, NULL, NULL));
    /* r18 is a static register: physical index 18. */
    if (w->corrupt)
        T1H_CHECK(e->pv->corrupt(e->ctx, e->region, w->point, 2, T1_ELEMENT_GR(18), 1));
}

static void t_wrong_old_value(const struct t1h_provider *pv, struct ledger *L)
{
    env_open(pv, "gather_phys", NULL, NULL, L);
    int s2 = find_site(true, 18);
    T1H_CHECK(s2 >= 0);
    struct wrong_old_value w = {E.points[s2].point_id, false};
    int entry = (int)ledger_expect(L, E.region, w.point, 2, LEDGER_TRAP_EXIT) + 1;
    must_pass("wrong-old-value control gather_phys",
              &(struct run_options){.entry = entry, .arm = arm_wrong_old_value, .arg = &w});
    w.corrupt = true;
    unsigned inplace,
        diff = run_case(
            T1H_EXPECTED_DIFFERENCE " wrong-old-value",
            &(struct run_options){.entry = entry, .arm = arm_wrong_old_value, .arg = &w}, &inplace);
    if (diff || !inplace) {
        fprintf(stderr,
                "restore: wrong-old-value (%s): final differences %u (want 0), in-place failures "
                "%u (want > 0)\n",
                pv->name, diff, inplace);
        T1H_CHECK(!"wrong-old-value did not fail in place only");
    }
    printf(
        "restore: wrong-old-value gather_phys     (%s): a wrong old r18 passes the final comparison and "
        "fails in place (%u capture)\n",
        evidence(pv), inplace);
    env_close();
}

/* ---- checkpoint-exit -------------------------------------------------------------- */

static void r_ck(const struct t1h_provider *pv, struct ledger *L)
{
    env_open(pv, "gather_phys", NULL, NULL, L);
    /* Some checkpoint of the region (its kind is checked by the ledger). */
    ledger_expect(L, E.region, LEDGER_ANY_POINT, 0, LEDGER_CHECKPOINT);
    /* The whole run's length from the header, then budgets across it. */
    unsigned inplace;
    must_pass("checkpoint-exit whole gather_phys", &(struct run_options){0});
    uint64_t total = E.reference.v.retired - E.snap.v.retired;
    unsigned runs = 0;
    for (uint64_t k = 1; k < total; k += 1 + total / 40, ++runs) {
        char label[96];
        snprintf(label, sizeof(label), "checkpoint-exit budget %llu gather_phys/%s",
                 (unsigned long long)k, pv->name);
        unsigned diff = run_case(label, &(struct run_options){.budget = k}, &inplace);
        if (diff || inplace) {
            fprintf(stderr, "restore: %s: %u final and %u in-place differences\n", label, diff,
                    inplace);
            T1H_CHECK(!"checkpoint-exit budget case differs");
        }
    }
    must_pass("checkpoint-exit kick gather_phys", &(struct run_options){.kick = true});
    printf(
        "restore: checkpoint-exit      gather_phys     (%s): %u budgets and a kick, %u records, %u in-place "
        "captures equal\n",
        evidence(pv), runs, E.records, E.captures);
    env_close();
}

/* ---- guest-fault-restore -------------------------------------------------------------- */

/* At region entry: remove the guest translation of the table page S2 reads
 * next (gather: r32 indexes the index array, S2 reads r33 + 8 * index). The
 * hook runs before hostwin_enter, with v's homes exact at entry. */
static unsigned guest_fault_injections, guest_fault_chained;
static void guest_fault_hook(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)region;
    struct env *e = arg;
    uint64_t r32 = ia64_gr_read(&e->v->s, 32), index;
    gpa_t pa = r32 & ia64_low_mask(61); /* index array: VA payload = GPA */
    if (!gm_read(pa, &index, 8) || !index)
        return;
    uint64_t gva = ia64_gr_read(&e->v->s, 33) + 8 * index;
    schedule_apply_now(&e->sched, e->v, (struct injection){.kind = INJ_UNMAP, .address = gva});
    ++guest_fault_injections;
    guest_fault_chained += tv && tv->chained;
}

static void r_gf(const struct t1h_provider *pv, struct ledger *L)
{
    struct guest_params q = guest_sweep_params(guest_program("gather_virt_stride"));
    q.passes = 2; /* pass 2: pages warm from pass 1 */
    env_open(pv, "gather_virt_stride", &q, NULL, L);
    int s2 = find_site(true, 18);
    T1H_CHECK(s2 >= 0);
    if (pv->evidence)
        ledger_expect(L, E.region, E.points[s2].point_id, 0, LEDGER_GUEST_FAULT);
    guest_fault_injections = 0;
    must_pass("guest-fault-restore gather_virt_stride",
              &(struct run_options){.hook = guest_fault_hook});
    T1H_CHECK(guest_fault_injections > 0);
    printf(
        "restore: guest-fault-restore      gather_virt_stride (%s): %u UNMAP injections at region entry, %u records, "
        "%u in-place captures equal%s\n",
        evidence(pv), guest_fault_injections, E.records, E.captures,
        pv->evidence ? "" : " (guest faults inside regions not modelled by the double)");
    env_close();
}

/* ---- store-once -------------------------------------------------------------- */

struct store_once_case {
    uint64_t occurrence, gva, stored;
};

/* Occurrences of S4 that follow a store S3 completed in the same iteration,
 * found on the stepping reference, with that store's address and value. */
static unsigned store_once_candidates(uint32_t s4, struct store_once_case *out, unsigned max)
{
    struct env *e = &E;
    const struct t1_point_info *p = &e->points[s4];
    e->ref_logs.stores_on = true;
    snapshot_restore(&e->snap, e->ref);
    size_t at_header = 0;
    uint64_t occurrence = 0;
    unsigned n = 0;
    for (uint64_t step = 0; step < 100000 && n < max; ++step) {
        if (e->ref->s.ip == e->header && e->ref->s.slot == 0)
            at_header = e->ref_logs.store_n;
        if (e->ref->s.ip == p->ip && e->ref->s.slot == p->slot) {
            ++occurrence;
            if (e->ref_logs.store_n > at_header) {
                const struct compare_store_record *s = &e->ref_logs.stores[e->ref_logs.store_n - 1];
                out[n++] = (struct store_once_case){occurrence, s->gva, s->value};
            }
        }
        if (ref_run(e->ref, 1, NULL).reason != VCPU_EXIT_BUDGET)
            break;
    }
    return n;
}

struct store_once_arm {
    uint32_t point;
    struct store_once_case c;
    struct schedule_hook hook;
};
static void arm_store_once(struct env *e, void *arg)
{
    struct store_once_arm *a = arg;
    a->hook = (struct schedule_hook){&e->sched,
                                     {.at = T1H_AT_PENDING,
                                      .kind = INJ_DMA,
                                      .address = a->c.gva,
                                      .value = ~a->c.stored,
                                      .width = 8}};
    T1H_CHECK(e->pv->force(e->ctx, e->region, a->point, a->c.occurrence, T1_FORCE_TRAP_EXIT,
                           schedule_before_resolve, &a->hook));
}

static void r_st(const struct t1h_provider *pv, struct ledger *L)
{
    env_open(pv, "gather_load_after_store", NULL, NULL, L);
    int s4 = find_site(true, 19);
    T1H_CHECK(s4 >= 0 && find_site(false, 0) >= 0);
    struct store_once_case cases[3];
    unsigned n = store_once_candidates((uint32_t)s4, cases, 3);
    T1H_CHECK(n == 3);
    for (unsigned i = 0; i < n; ++i) {
        struct store_once_arm a = {E.points[s4].point_id, cases[i], {0}};
        int entry =
            (int)ledger_expect(L, E.region, a.point, cases[i].occurrence, LEDGER_TRAP_EXIT) + 1;
        char label[96];
        snprintf(label, sizeof(label), "store-once gather_load_after_store/%s S4 occurrence %llu",
                 pv->name, (unsigned long long)cases[i].occurrence);
        must_pass(label, &(struct run_options){.entry = entry, .arm = arm_store_once, .arg = &a});
        /* S3 never stores to that address again: the DMA survives. */
        uint64_t got;
        memcpy(&got, E.actual.ram + (cases[i].gva - T1H_RAM_BASE), 8);
        T1H_CHECK(got == ~cases[i].stored);
    }
    printf(
        "restore: store-once      gather_load_after_store       (%s): %u DMA writes from forced exits at S4 survive, "
        "every store logged once (%zu stores per run)\n",
        evidence(pv), n, E.reference.logs.store_n);
    env_close();
}

/* ---- chained-entry ------------------------------------------------------------ */

/* Whether the first forced record of a run came from an entry made at a
 * chain point. */
static struct {
    bool forced, chained;
} chained_force;
static void chained_force_restore(struct t1_vcpu *tv, const struct vcpu *v,
                                  const struct t1_restore_record *r, void *arg)
{
    if (r->forced && !chained_force.forced) {
        chained_force.forced = true;
        chained_force.chained = tv->chained;
    }
    on_restore(tv, v, r, arg);
}

struct chained_force_arm {
    uint32_t point;
    uint64_t occurrence;
    enum t1_force_action action;
};
static void arm_chained_force(struct env *e, void *arg)
{
    const struct chained_force_arm *a = arg;
    T1H_CHECK(e->pv->force(e->ctx, e->region, a->point, a->occurrence, a->action, NULL, NULL));
}

#define CHAINED_FORCE_OCCURRENCES 64u
static void r_chain(const struct t1h_provider *pv)
{
    struct guest_params q = guest_sweep_params(guest_program("gather_phys"));
    q.passes = 6;
    env_open(pv, "gather_phys", &q, NULL, NULL);
    /* The other regions compiled first; every run learns its successors
     * again (the restore empties the cache). */
    must_pass("chained-entry warm-up gather_phys", &(struct run_options){0});
    struct t1_stats s0;
    t1_get_stats(&s0);
    unsigned cases = 0, runs = 0;
    for (unsigned i = 0; i < E.npoints; ++i) {
        const struct t1_point_info *p = &E.points[i];
        static const enum t1_force_action site[] = {T1_FORCE_TRAP_RETRY, T1_FORCE_TRAP_EXIT,
                                                    T1_FORCE_MISS_EXIT};
        static const enum t1_force_action helper[] = {T1_FORCE_HELPER_EXIT};
        const enum t1_force_action *acts = p->kind == T1_PT_SITE ? site : helper;
        unsigned nacts = p->kind == T1_PT_SITE ? 3 : p->kind == T1_PT_HELPER ? 1 : 0;
        for (unsigned a = 0; a < nacts; ++a) {
            bool chained = false;
            for (uint64_t occ = 1; occ <= CHAINED_FORCE_OCCURRENCES && !chained; ++occ, ++runs) {
                struct chained_force_arm arm = {p->point_id, occ, acts[a]};
                chained_force.forced = chained_force.chained = false;
                char label[128];
                snprintf(label, sizeof(label),
                         "chained-entry gather_phys/%s point %u occurrence %llu action %u",
                         pv->name, p->point_id, (unsigned long long)occ, acts[a]);
                must_pass(label, &(struct run_options){.arm = arm_chained_force,
                                                       .arg = &arm,
                                                       .restore = chained_force_restore});
                chained = chained_force.forced && chained_force.chained;
            }
            T1H_CHECK(chained && "no forced exit in an entry made at a chain point");
            ++cases;
        }
    }
    struct t1_stats s1;
    t1_get_stats(&s1);
    T1H_CHECK(cases > 0 && s1.chain_links > s0.chain_links);
    printf(
        "restore: chained-entry   gather_phys     (%s): %u forced cases, each at the first occurrence in an entry made "
        "at a chain point (%u runs, %llu chain links), %u in-place captures equal\n",
        evidence(pv), cases, runs, (unsigned long long)(s1.chain_links - s0.chain_links),
        E.captures);
    env_close();
    /* guest-fault-restore with injections at entries made at chain points. */
    q = guest_sweep_params(guest_program("gather_virt_stride"));
    q.passes = 12;
    env_open(pv, "gather_virt_stride", &q, NULL, NULL);
    guest_fault_injections = guest_fault_chained = 0;
    must_pass("chained-entry guest-fault-restore gather_virt_stride",
              &(struct run_options){.hook = guest_fault_hook});
    T1H_CHECK(guest_fault_chained > 0);
    printf(
        "restore: chained-entry   gather_virt_stride (%s): %u UNMAP injections at region entry, %u of them at entries "
        "made at chain points, %u in-place captures equal\n",
        evidence(pv), guest_fault_injections, guest_fault_chained, E.captures);
    env_close();
}

/* ---- Tier1-only drivers ------------------------------------------------ */

/* host-fill: the first runs after compile fill every site's page through the
 * trap path; then the entry hook drops the page S1 reads next. */
static void host_fill_hook(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)tv;
    (void)region;
    struct env *e = arg;
    hostwin_test_drop(e->pv->hostwin(e->ctx), ia64_gr_read(&e->v->s, 32));
}

static void r_host(const struct t1h_provider *pv, struct ledger *L)
{
    env_open(pv, "gather_phys", NULL, NULL, L);
    for (unsigned i = 0; i < E.npoints; ++i)
        if (E.points[i].kind == T1_PT_SITE)
            ledger_expect(L, E.region, E.points[i].point_id, 0, LEDGER_HOST_FILL);
    must_pass("host-fill cold gather_phys", &(struct run_options){0});
    must_pass("host-fill drop gather_phys", &(struct run_options){.hook = host_fill_hook});
    printf(
        "restore: host-fill    gather_phys     (%s): cold fills and dropped shadows refilled, %u records\n",
        evidence(pv), E.records);
    env_close();
}

/* injected-resolve: the n-th hostwin_resolve exits (HW_EXIT_INJECTED). */
static void arm_inj(struct env *e, void *arg)
{
    struct hostwin_vcpu *hw = e->pv->hostwin(e->ctx);
    hostwin_invalidate(hw); /* every access needs a fill */
    hostwin_test_inject(hw, *(uint64_t *)arg);
}

static void r_inj(const struct t1h_provider *pv, struct ledger *L)
{
    env_open(pv, "gather_phys", NULL, NULL, L);
    for (uint64_t n = 1; n <= 3; ++n) {
        ledger_expect(L, E.region, LEDGER_ANY_POINT, 0, LEDGER_INJECTED);
        char label[64];
        snprintf(label, sizeof(label), "injected-resolve gather_phys n=%llu",
                 (unsigned long long)n);
        must_pass(label, &(struct run_options){.arm = arm_inj, .arg = &n});
    }
    printf(
        "restore: injected-resolve     gather_phys     (%s): injected resolve exits 1-3, %u records\n",
        evidence(pv), E.records);
    env_close();
}

/* pinned-rax: r14 (the loop-carried sum) pinned to RAX survives a kick exit, a
 * budget checkpoint and a requested exit. */
static unsigned rax_entries;
static void rax_hook(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)tv;
    (void)region;
    struct env *e = arg;
    if (++rax_entries == 2)
        vcpu_request_exit(e->v, VCPU_EXIT_HALT);
}

static void t_rax(const struct t1h_provider *pv, struct ledger *L)
{
    struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG,
                                     .pin_element = T1_ELEMENT_GR(14),
                                     .pin_reg = T1_RAX};
    env_open(pv, "gather_phys", NULL, &opt, L);
    bool pinned = false;
    for (unsigned i = 0; i < E.report.n_loop_carried && i < 16; ++i)
        pinned |=
            E.report.loop_carried[i] == T1_ELEMENT_GR(14) && E.report.loop_carried_reg[i] == T1_RAX;
    T1H_CHECK(pinned);
    must_pass("pinned-rax kick gather_phys", &(struct run_options){.kick = true});
    must_pass("pinned-rax budget gather_phys", &(struct run_options){.budget = 37});
    /* Requested exit at the second region entry: t1_run returns it at the
     * next event checkpoint. The reference receives the same request at the
     * same retired count, so both consume it (interp_run clears it when it
     * returns the exit) and must agree on exit and state; then both finish. */
    rax_entries = 0;
    E.logs.stores_on = E.ref_logs.stores_on = true;
    snapshot_restore(&E.snap, E.v);
    pv->restored(E.ctx);
    pv->set_entry_hook(E.ctx, rax_hook, &E);
    pv->set_store_observer(E.ctx, storelog_t1_store, &E.logs);
    struct vcpu_exit x = pv->run(E.ctx, BIG_BUDGET);
    pv->set_entry_hook(E.ctx, NULL, NULL);
    T1H_CHECK(x.reason == VCPU_EXIT_HALT);
    result_take(&E.actual, &x, E.v);
    snapshot_restore(&E.snap, E.ref);
    struct vcpu_exit rx;
    T1H_CHECK(ref_run_to(E.ref, E.v->retired, NULL, &rx));
    vcpu_request_exit(E.ref, VCPU_EXIT_HALT);
    rx = ref_run(E.ref, BIG_BUDGET, NULL);
    result_take(&E.reference, &rx, E.ref);
    T1H_CHECK(result_compare(&E.reference, &E.actual, true, "pinned-rax requested exit") == 0);
    x = pv->run(E.ctx, BIG_BUDGET);
    pv->set_store_observer(E.ctx, NULL, NULL);
    result_take(&E.actual, &x, E.v);
    rx = ref_run(E.ref, BIG_BUDGET, NULL);
    result_take(&E.reference, &rx, E.ref);
    T1H_CHECK(
        result_compare(&E.reference, &E.actual, true, "pinned-rax after the requested exit") == 0);
    printf(
        "restore: pinned-rax     gather_phys     (%s): r14 in RAX across kick, budget and requested exits\n",
        evidence(pv));
    env_close();
}

/* mid-block-exit: a forced exit at a load after two completed pure instructions
 * of its block; the same t1_run(budget) continues in the lower tier and
 * retires exactly the budget. */
static void arm_mid(struct env *e, void *arg)
{
    T1H_CHECK(e->pv->force(e->ctx, e->region, *(uint32_t *)arg, 2, T1_FORCE_TRAP_EXIT, NULL, NULL));
}

static void t_midblock(const struct t1h_provider *pv, struct ledger *L)
{
    env_open(pv, "mid_block", NULL, NULL, L);
    int site = find_site(true, 19);
    T1H_CHECK(site >= 0);
    uint32_t point = E.points[site].point_id;
    int entry = (int)ledger_expect(L, E.region, point, 2, LEDGER_TRAP_EXIT) + 1;
    /* The budget ends a little after the load's second execution, but no
     * earlier than two loop passes later: the back-edge checkpoint leaves
     * the region when less than max_path remains, and the forced
     * occurrence must be reached in generated code, not in the lower
     * tier (where it would not be exercised). */
    snapshot_restore(&E.snap, E.ref);
    uint64_t seen = 0, budget = 0, pass = 0;
    for (uint64_t k = 0; k < 10000 && !budget; ++k) {
        if (E.ref->s.ip == E.header && E.ref->s.slot == 0 && k && !pass)
            pass = k;
        if (E.ref->s.ip == E.points[site].ip && E.ref->s.slot == E.points[site].slot && ++seen == 2)
            budget = E.ref->retired - E.snap.v.retired + 2 * pass + 5;
        T1H_CHECK(ref_run(E.ref, 1, NULL).reason == VCPU_EXIT_BUDGET);
    }
    T1H_CHECK(budget);
    must_pass(
        "mid-block-exit mid_block",
        &(struct run_options){.entry = entry, .budget = budget, .arm = arm_mid, .arg = &point});
    T1H_CHECK(E.actual.exit.reason == VCPU_EXIT_BUDGET &&
              E.actual.v.retired == E.snap.v.retired + budget);
    printf(
        "restore: mid-block-exit mid_block      (%s): exit at a load after 2 pure instructions, then exactly "
        "%llu retired in one t1_run (%u records)\n",
        evidence(pv), (unsigned long long)budget, E.records);
    env_close();
}

/* window-tags: twelve tags in one region execution through 8 slots. */
static void t_window_tags(const struct t1h_provider *pv, struct ledger *L)
{
    env_open(pv, "window_tags", NULL, NULL, L);
    must_pass("window-tags window_tags", &(struct run_options){0});
    struct hostwin_stats hs;
    hostwin_get_stats(pv->hostwin(E.ctx), &hs);
    struct t1_stats ts;
    pv->stats(E.ctx, &ts);
    T1H_CHECK(hs.binds >= 9 && hs.rebinds > 0 && hs.exits[HW_EXIT_STALE_SLOT] == 0);
    printf(
        "restore: window-tags    window_tags      (%s): %llu binds, %llu rebinds, %llu window misses, "
        "no stale slot\n",
        evidence(pv), (unsigned long long)hs.binds, (unsigned long long)hs.rebinds,
        (unsigned long long)ts.window_misses);
    env_close();
}

/* ---- frame-bases (frame-relative homes) ------------------
 *
 * Test BOFs 0, 44, 90 and 95; the sof=8 gather frame wraps the 96-register
 * ring at 90 and 95. Registers outside the frame are poisoned with distinct
 * values and NaT on every third. Comparing physical gr[]/nat[] at each
 * materialization and at the end detects homes using the wrong frame base.
 * One region compiled at BOF 44 must serve every BOF: the ledger checks
 * region ids and the compile observer forbids further header compiles.
 * Each BOF exercises all forced-exit points/actions at occurrences 1-3.
 * Gather cases corrupt r32's materialization to verify its physical home and
 * the in-place comparison; gather-phys and gather-rot also test budgets/a kick.
 */
static const unsigned frame_bases_bof[] = {0, 44, 90, 95};
#define FRAME_BASES_N 4u
#define FRAME_BASES_COMPILED_AT 1u

static struct snapshot frame_bases_snap[FRAME_BASES_N];
static uint64_t frame_bases_header;
static unsigned frame_bases_compiles;
static void frame_bases_on_compile(const struct vcpu *v, enum t1_compile_status st,
                                   const struct t1_compile_report *rep, void *arg)
{
    (void)st;
    (void)rep;
    (void)arg;
    if (v->s.ip == frame_bases_header && v->s.slot == 0)
        ++frame_bases_compiles;
}

static void frame_bases_poison(struct ia64_state *s, unsigned bof)
{
    unsigned sof = (unsigned)(s->cfm & 127);
    for (unsigned o = sof; o < IA64_N_STACKED_PHYS; ++o) {
        unsigned at = t1t_phys(bof, 32 + o);
        s->gr[at] = UINT64_C(0xb0f0000000000000) | (uint64_t)o << 8 | bof;
        s->nat[at] = o % 3 == 0;
    }
}

static void frame_bases_select(unsigned b)
{
    E.snap.v = frame_bases_snap[b].v;
    memcpy(E.snap.ram, frame_bases_snap[b].ram, T1H_RAM_SIZE);
}

/* The capture of (point, occurrence) in the last run, or NULL. */
static const struct observe_capture *capture_at(uint32_t point, uint64_t occurrence)
{
    for (unsigned i = 0; i < E.obs.n; ++i)
        if (E.obs.c[i].point == point && E.obs.c[i].occurrence == occurrence)
            return &E.obs.c[i];
    return NULL;
}

struct wrong_old_value_stacked {
    uint32_t point;
    uint16_t element;
    uint64_t mask;
    bool corrupt;
};
static void arm_wrong_old_value_stacked(struct env *e, void *arg)
{
    const struct wrong_old_value_stacked *w = arg;
    T1H_CHECK(e->pv->force(e->ctx, e->region, w->point, 2, T1_FORCE_TRAP_EXIT, NULL, NULL));
    if (w->corrupt)
        T1H_CHECK(e->pv->corrupt(e->ctx, e->region, w->point, 2, w->element, w->mask));
}

/* r32's element (frame-relative, after rotation) corrupted at S2's second
 * occurrence: the capture differs from the uncorrupted one in exactly r32's
 * physical home at this BOF, by the mask. */
static void frame_bases_wrong_old_value(struct ledger *L, unsigned bof, const char *tag)
{
    int s2 = find_site(true, 18);
    T1H_CHECK(s2 >= 0);
    const struct ia64_state *s = &E.snap.v.s;
    unsigned reg = t1t_element_reg(s, 32), at = t1t_phys(bof, reg);
    T1H_CHECK(at == ia64_gr_index(s, 32)); /* bof.h agrees with the CPU model */
    struct wrong_old_value_stacked w = {E.points[s2].point_id, T1_ELEMENT_GR(reg), 8, false};
    int entry = (int)ledger_expect(L, E.region, w.point, 2, LEDGER_TRAP_EXIT) + 1;
    char label[96];
    snprintf(label, sizeof(label), "frame-bases wrong-old-value control %s", tag);
    must_pass(label,
              &(struct run_options){.entry = entry, .arm = arm_wrong_old_value_stacked, .arg = &w});
    const struct observe_capture *c = capture_at(w.point, 2);
    T1H_CHECK(c != NULL);
    static struct vcpu control;
    control = c->v;
    w.corrupt = true;
    unsigned inplace;
    snprintf(label, sizeof(label), T1H_EXPECTED_DIFFERENCE " frame-bases wrong-old-value %s", tag);
    run_case(label,
             &(struct run_options){.entry = entry, .arm = arm_wrong_old_value_stacked, .arg = &w},
             &inplace);
    c = capture_at(w.point, 2);
    T1H_CHECK(inplace > 0 && c != NULL);
    for (unsigned i = 0; i < 128; ++i) {
        if (c->v.s.gr[i] != (control.s.gr[i] ^ (i == at ? w.mask : 0)) ||
            c->v.s.nat[i] != control.s.nat[i]) {
            fprintf(
                stderr,
                "restore: frame-bases wrong-old-value %s: gr/nat[%u] %#llx/%d, control %#llx/%d (corrupted home %u)\n",
                tag, i, (unsigned long long)c->v.s.gr[i], c->v.s.nat[i],
                (unsigned long long)control.s.gr[i], control.s.nat[i], at);
            T1H_CHECK(!"t1_test_corrupt reached another physical register");
        }
    }
}

static void frame_bases_ck(struct ledger *L, const char *tag)
{
    ledger_expect(L, E.region, LEDGER_ANY_POINT, 0, LEDGER_CHECKPOINT);
    char label[96];
    snprintf(label, sizeof(label), "frame-bases checkpoint-exit whole %s", tag);
    must_pass(label, &(struct run_options){0});
    uint64_t total = E.reference.v.retired - E.snap.v.retired;
    for (uint64_t k = 1; k < total; k += 1 + total / 12) {
        snprintf(label, sizeof(label), "frame-bases checkpoint-exit budget %llu %s",
                 (unsigned long long)k, tag);
        must_pass(label, &(struct run_options){.budget = k});
    }
    snprintf(label, sizeof(label), "frame-bases checkpoint-exit kick %s", tag);
    must_pass(label, &(struct run_options){.kick = true});
}

static void r_bof(const struct t1h_provider *pv, struct ledger *L, const char *program)
{
    env_machine(pv, program, NULL, L);
    bool gather = E.p->kind == GUEST_GATHER;
    for (unsigned b = 0; b < FRAME_BASES_N; ++b) {
        env_header(frame_bases_bof[b]);
        struct ia64_state *s = &E.ref->s;
        unsigned sof = (unsigned)(s->cfm & 127);
        T1H_CHECK(s->rse_bof == frame_bases_bof[b] && !s->rse_dirty && (!gather || sof == 8));
        /* 90 and 95: the frame's last register is physically below its first. */
        bool wraps = frame_bases_bof[b] + sof > IA64_N_STACKED_PHYS;
        T1H_CHECK(wraps ==
                  (t1t_phys(frame_bases_bof[b], 32 + sof - 1) < t1t_phys(frame_bases_bof[b], 32)));
        T1H_CHECK(wraps == (frame_bases_bof[b] >= 90));
        frame_bases_poison(s, frame_bases_bof[b]);
        snapshot_take(&frame_bases_snap[b], E.ref);
    }
    frame_bases_header = E.header;
    frame_bases_select(FRAME_BASES_COMPILED_AT);
    env_compile(NULL);
    frame_bases_compiles = 0;
    if (pv->evidence)
        t1_test_set_compile_observer(frame_bases_on_compile, NULL);
    unsigned first = L->n, sites = 0, helpers = 0;
    for (unsigned i = 0; i < E.npoints; ++i) {
        sites += E.points[i].kind == T1_PT_SITE;
        helpers += E.points[i].kind == T1_PT_HELPER;
    }
    for (unsigned b = 0; b < FRAME_BASES_N; ++b) {
        char tag[32];
        snprintf(tag, sizeof(tag), "%s@bof%u", program, frame_bases_bof[b]);
        frame_bases_select(b);
        ledger_set_program(L, tag);
        /* The region compiled at BOF 44 runs here: a whole run leaves it
         * (Tier1; the double's exit log covers forced exits only). */
        char whole[96];
        snprintf(whole, sizeof(whole), "frame-bases whole %s", tag);
        must_pass(whole, &(struct run_options){0});
        unsigned mine = 0;
        struct t1_exit_info xi;
        for (unsigned k = 0; k < pv->exit_count(E.ctx); ++k)
            if (pv->exit_get(E.ctx, k, &xi) && xi.region_id == E.region)
                ++mine;
        if (pv->evidence && !mine)
            fprintf(stderr, "restore: %s/%s: region %u not entered\n", whole, pv->name, E.region);
        T1H_CHECK(mine > 0 || !pv->evidence);
        unsigned from = L->n;
        ledger_expect_force(L, E.region, E.points, E.npoints, MAX_OCCURRENCE);
        T1H_CHECK(L->n > from);
        for (unsigned i = from; i < L->n; ++i) {
            const struct ledger_case *c = &L->c[i];
            char label[160];
            snprintf(label, sizeof(label),
                     "frame-bases forced-exit %s/%s point %u occurrence %llu %s", tag, pv->name,
                     c->point, (unsigned long long)c->occurrence, ledger_action_name(c->action));
            must_pass(label, &(struct run_options){
                                 .entry = (int)i + 1, .arm = arm_force, .arg = &L->c[i]});
        }
        if (gather)
            frame_bases_wrong_old_value(L, frame_bases_bof[b], tag);
        if (!strcmp(program, "gather_phys") || !strcmp(program, "gather_rot"))
            frame_bases_ck(L, tag);
    }
    t1_test_set_compile_observer(NULL, NULL);
    if (pv->evidence)
        T1H_CHECK(frame_bases_compiles == 0);
    printf(
        "restore: frame-bases     %-15s (%s): one region compiled at BOF %u, entered at BOF 0/44/90/95 "
        "(sof %u, 90 and 95 wrap): %u sites, %u helpers, %u cases, %u records, %u in-place captures "
        "equal%s, %u compiles at the header after the first\n",
        program, evidence(pv), frame_bases_bof[FRAME_BASES_COMPILED_AT],
        (unsigned)(E.snap.v.s.cfm & 127), sites, helpers, L->n - first, E.records, E.captures,
        gather ? ", stacked wrong-old-value lands on r32's physical home" : "",
        frame_bases_compiles);
    env_close();
}

/* ---- rse-recursion-restore (frame-relative homes) --------------------
 *
 * programs/rse_recursion.S returns to its header at 14 BOFs, 9k mod 96,
 * with rse_missing=9 and CFLE set; level 10 wraps at BOF 90. Tier1's hotness
 * policy must compile one verification region serving every BOF, entered
 * only after the lower tier's mandatory RSE loads (SDM Vol.2 §6.8).
 * An external interrupt at each header arrival returns there with CFLE set.
 * Every run must match the reference.
 */
#define RSE_RECURSION_INT_VECTOR 0x40u
static uint64_t rse_recursion_head;
static unsigned rse_recursion_entries, rse_recursion_bad, rse_recursion_compiles;
static bool rse_recursion_bof_seen[IA64_N_STACKED_PHYS];
static void rse_recursion_hook(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)tv;
    (void)region;
    const struct vcpu *v = arg;
    if (v->s.ip != rse_recursion_head || v->s.slot)
        return;
    ++rse_recursion_entries;
    if (v->s.rse_cfle || v->s.rse_missing)
        ++rse_recursion_bad;
    rse_recursion_bof_seen[v->s.rse_bof % IA64_N_STACKED_PHYS] = true;
}
static void rse_recursion_on_compile(const struct vcpu *v, enum t1_compile_status st,
                                     const struct t1_compile_report *rep, void *arg)
{
    (void)rep;
    (void)arg;
    if (v->s.ip == rse_recursion_head && v->s.slot == 0 && st == T1_COMPILED)
        ++rse_recursion_compiles;
}

static void r_rse_bof(void)
{
    const struct guest_program *p = guest_program("rse_recursion");
    T1H_CHECK(p != NULL);
    struct guest_params q = guest_completion_params(p);
    struct env *e = &E;
    env_machine(t1h_provider_real(), p->name, &q, NULL);
    guest_setup(p, &q, e->ref);
    snapshot_take(&e->snap, e->ref);
    /* The reference: every header arrival with its frame state. */
    snapshot_restore(&e->snap, e->ref);
    T1H_CHECK(t1h_run_to_loop_header(e->ref, 100000, &rse_recursion_head));
    snapshot_restore(&e->snap, e->ref);
    static uint64_t arrival[1024];
    unsigned narrival = 0, unloaded = 0, nbof = 0;
    bool bof_ref[IA64_N_STACKED_PHYS] = {false}, wrapped = false;
    for (uint64_t step = 0; step < 1000000; ++step) {
        const struct ia64_state *s = &e->ref->s;
        if (s->ip == rse_recursion_head && s->slot == 0) {
            T1H_CHECK(narrival < 1024);
            arrival[narrival++] = e->ref->retired;
            if (s->rse_missing == 9 && s->rse_cfle) {
                ++unloaded;
                unsigned bof = s->rse_bof % IA64_N_STACKED_PHYS;
                nbof += !bof_ref[bof];
                bof_ref[bof] = true;
                wrapped |= bof + (unsigned)(s->cfm & 127) > IA64_N_STACKED_PHYS;
            }
        }
        if (ref_run(e->ref, 1, NULL).reason != VCPU_EXIT_BUDGET)
            break;
    }
    T1H_CHECK(unloaded == q.passes && narrival == q.passes * q.n && nbof == q.passes && wrapped);
    e->ref_logs.stores_on = true;
    snapshot_restore(&e->snap, e->ref);
    struct vcpu_exit rx = ref_run(e->ref, BIG_BUDGET, NULL);
    result_take(&e->reference, &rx, e->ref);
    char why[200];
    T1H_CHECK(guest_check(p, &q, e->ref, &rx, &e->ref_logs, why, sizeof(why)));

    /* Tier1, whole run: the header region at every level's BOF. */
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier1(), e->v);
    t1h_tier1_set_flags(T1_COMPILE_VERIFY);
    memset(rse_recursion_bof_seen, 0, sizeof(rse_recursion_bof_seen));
    rse_recursion_entries = rse_recursion_bad = rse_recursion_compiles = 0;
    t1_test_set_entry_hook(rse_recursion_hook, e->v);
    t1_test_set_compile_observer(rse_recursion_on_compile, NULL);
    e->logs.stores_on = true;
    snapshot_restore(&e->snap, e->v);
    t1h_engine_restored(&ev);
    struct vcpu_exit x = t1h_engine_run(&ev, BIG_BUDGET);
    t1_test_set_compile_observer(NULL, NULL);
    result_take(&e->actual, &x, e->v);
    T1H_CHECK(result_compare(&e->reference, &e->actual, true, "rse-recursion-restore tier1") == 0);
    unsigned entered_bofs = 0, entered_wrap = 0;
    for (unsigned b = 0; b < IA64_N_STACKED_PHYS; ++b) {
        entered_bofs += rse_recursion_bof_seen[b];
        entered_wrap += rse_recursion_bof_seen[b] && b + 9 > IA64_N_STACKED_PHYS;
    }
    if (rse_recursion_bad || rse_recursion_compiles != 1 || entered_bofs != q.passes ||
        !entered_wrap)
        fprintf(
            stderr,
            "rse-recursion-restore: %u header entries (%u with an incomplete frame) at %u BOFs (%u wrapping), "
            "%u compiles at the header\n",
            rse_recursion_entries, rse_recursion_bad, entered_bofs, entered_wrap,
            rse_recursion_compiles);
    T1H_CHECK(rse_recursion_bad == 0 && rse_recursion_compiles == 1 && entered_bofs == q.passes &&
              entered_wrap);

    /* An external interrupt at every header arrival. */
    static struct schedule sched;
    unsigned compared = 0;
    for (unsigned a = 0; a < narrival; a += 1 + narrival / 64) {
        uint64_t k = arrival[a];
        schedule_clear(&sched);
        schedule_add(&sched, (struct injection){
                                 .at = k, .kind = INJ_RAISE, .address = RSE_RECURSION_INT_VECTOR});
        snapshot_restore(&e->snap, e->ref);
        rx = ref_run(e->ref, BIG_BUDGET, &sched);
        T1H_CHECK(schedule_complete(&sched) && t1h_bank0(e->ref, 24) == 1);
        result_take(&e->reference, &rx, e->ref);
        schedule_rearm(&sched);
        snapshot_restore(&e->snap, e->v);
        t1h_engine_restored(&ev);
        x = t1h_engine_run_sched(&ev, BIG_BUDGET, &sched);
        result_take(&e->actual, &x, e->v);
        T1H_CHECK(result_compare(&e->reference, &e->actual, true,
                                 "rse-recursion-restore interrupt") == 0);
        ++compared;
    }
    t1_test_set_entry_hook(NULL, NULL);
    T1H_CHECK(rse_recursion_bad == 0);
    storelog_detach(e->v);
    t1h_engine_release(&ev);
    printf(
        "restore: rse-recursion-restore rse_recursion (Tier1): %u returns onto the header with 9 registers unloaded at %u "
        "BOFs (one wrapping); the header region (1 compile) entered %u times at %u BOFs, never with an "
        "incomplete frame; %u runs with an interrupt at a header arrival equal\n",
        unloaded, nbof, rse_recursion_entries, entered_bofs, compared);
    storelog_detach(e->ref);
    t1h_unbind(e->ref);
    t1h_unbind(e->v);
    vcpu_destroy(e->ref);
    vcpu_destroy(e->v);
    e->ref = e->v = NULL;
}

/* ---- the ledger itself ------------------------------------------------- */

static void ledger_tests(void)
{
    const struct t1_point_info pts[] = {
        {.point_id = 0, .kind = T1_PT_CHECKPOINT},
        {.point_id = 1, .kind = T1_PT_SITE},
        {.point_id = 2, .kind = T1_PT_SITE, .access_write = true},
        {.point_id = 3, .kind = T1_PT_HELPER},
    };
    static const enum t1_restore_path paths[] = {T1_PATH_TRAP, T1_PATH_TRAP, T1_PATH_MISS_STUB,
                                                 T1_PATH_HELPER};
    struct ledger l;
    ledger_init(&l, 64);
    ledger_set_program(&l, "synthetic");
    T1H_CHECK(ledger_expect_force(&l, 7, pts, 4, 2) == 2 * 3 * 2 + 2);
    ledger_expect(&l, 7, LEDGER_ANY_POINT, 0, LEDGER_INJECTED);
    /* A record per case, shaped as its action requires. */
    ledger_set_points(&l, pts, 4);
    for (unsigned i = 0; i + 1 < l.n; ++i) {
        const struct ledger_case *c = &l.c[i];
        ledger_arm(&l, c->action <= LEDGER_HELPER_EXIT ? (int)i : -1);
        struct t1_restore_record r = {.region_id = 7,
                                      .point_id = c->point,
                                      .occurrence = c->occurrence,
                                      .path = paths[c->action],
                                      .forced = true,
                                      .outcome = c->action == LEDGER_TRAP_RETRY ? T1_OUTCOME_RETRY
                                                                                : T1_OUTCOME_EXIT,
                                      .materialized = c->action != LEDGER_TRAP_RETRY};
        T1H_CHECK(ledger_record(&l, &r));
    }
    struct t1_restore_record inj = {.region_id = 7,
                                    .point_id = 2,
                                    .path = T1_PATH_TRAP,
                                    .outcome = T1_OUTCOME_EXIT,
                                    .materialized = true};
    T1H_CHECK(ledger_record(&l, &inj));
    T1H_CHECK(ledger_report(&l, "ledger complete", 4) == 0);
    /* A case whose point was reached in the lower tier has no record. */
    ledger_set_program(&l, "missing");
    ledger_arm(&l, (int)ledger_expect(&l, 7, 1, 3, LEDGER_TRAP_EXIT));
    T1H_CHECK(ledger_report(&l, T1H_EXPECTED_DIFFERENCE " ledger", 4) == 1);
    /* A record of the wrong shape (a retry for an exit case) does not count. */
    struct t1_restore_record wrong = {.region_id = 7,
                                      .point_id = 1,
                                      .occurrence = 3,
                                      .path = T1_PATH_TRAP,
                                      .forced = true,
                                      .outcome = T1_OUTCOME_RETRY};
    T1H_CHECK(!ledger_record(&l, &wrong));
    T1H_CHECK(ledger_report(&l, T1H_EXPECTED_DIFFERENCE " ledger", 4) == 2); /* + unexpected */
    /* Natural records never match forced cases and vice versa. */
    struct t1_restore_record natural = {.region_id = 7,
                                        .point_id = 1,
                                        .occurrence = 3,
                                        .path = T1_PATH_TRAP,
                                        .outcome = T1_OUTCOME_EXIT,
                                        .materialized = true,
                                        .translated = true};
    T1H_CHECK(!ledger_record(&l, &natural));
    /* A forced record of the armed case at another occurrence: an
     * occurrence-numbering disagreement, reported as such. */
    struct t1_restore_record later = wrong;
    later.outcome = T1_OUTCOME_EXIT;
    later.materialized = true;
    later.occurrence = 18;
    T1H_CHECK(!ledger_record(&l, &later) && l.occurrence_mismatch == 1);
    T1H_CHECK(ledger_report(&l, T1H_EXPECTED_DIFFERENCE " ledger", 4) == 3);
    ledger_free(&l);
    printf("restore: ledger: complete coverage passes; a missing case, a wrong-shaped record, an "
           "unarmed forced record and an occurrence mismatch fail\n");
}

/* ---- suite ------------------------------------------------------------- */

static void run_provider(const struct t1h_provider *pv)
{
    /* mid_block: the in-region helper (tbit) of the HELPER_EXIT class; call_return:
     * sites and helpers inside the frames of callees the region calls;
     * rse_spill: allocs that spill, at helper points after calls. */
    static const char *const force_programs[] = {
        "gather_phys", "gather_virt",  "gather_virt_stride",      "gather_rot",
        "gather_rotp", "gather_2page", "gather_load_after_store", "mid_block",
        "call_return", "rse_spill",    "rse_spill_virt"};
    struct ledger L;
    ledger_init(&L, LEDGER_CAP);
    struct t1_stats st0;
    t1_get_stats(&st0);
    for (unsigned i = 0; i < sizeof(force_programs) / sizeof(force_programs[0]); ++i)
        forced_exit(pv, &L, force_programs[i]);
    t_wrong_old_value(pv, &L);
    r_ck(pv, &L);
    r_gf(pv, &L);
    r_st(pv, &L);
    /* frame-bases: gather and gather-rot, and the variants with rotating predicate
     * frames, guest faults inside regions (gather-virt), a helper (mid_block) and
     * callee frames above the entry's (call_return). */
    static const char *const bof_programs[] = {"gather_phys", "gather_rot", "gather_rotp",
                                               "gather_virt", "mid_block",  "call_return"};
    for (unsigned i = 0; i < sizeof(bof_programs) / sizeof(bof_programs[0]); ++i)
        r_bof(pv, &L, bof_programs[i]);
    if (pv->evidence)
        r_rse_bof();
    /* The product path chains; off with T1H_CHAIN=off. */
    const char *chain = getenv("T1H_CHAIN");
    if (!strcmp(pv->name, "tier0+tier1") && !(chain && !strcmp(chain, "off")))
        r_chain(pv);
    if (pv->evidence) {
        r_host(pv, &L);
        r_inj(pv, &L);
        t_rax(pv, &L);
        t_midblock(pv, &L);
        t_window_tags(pv, &L);
    } else {
        printf(
            "restore: host-fill, injected-resolve, pinned-rax, mid-block-exit, window-tags: not run on the plumbing double "
            "(they need Tier1's windows and code)\n");
    }
    char label[64];
    snprintf(label, sizeof(label), "coverage (%s)", pv->name);
    unsigned missing = ledger_report(&L, label, 32);
    if (missing)
        T1H_CHECK(!"restoration coverage incomplete: not exercised = fail");
    printf(
        "restore: coverage (%s): %u cases, every one exercised by a matching record (%u records)\n",
        evidence(pv), L.n, L.records);
    if (pv->evidence) {
        /* Every case of an evidence provider ran Tier1 code; for the
         * product path also through Tier0's links. */
        struct t1_stats st;
        t1_get_stats(&st);
        uint64_t linked =
            st.upper_ran + st.upper_ran_pending - st0.upper_ran - st0.upper_ran_pending;
        T1H_CHECK(st.entries > st0.entries && (!strcmp(pv->name, "tier1") ? !linked : linked > 0));
        printf("restore: %s: %llu region entries, %llu of them from Tier0's lookups\n",
               evidence(pv), (unsigned long long)(st.entries - st0.entries),
               (unsigned long long)linked);
    }
    ledger_free(&L);
}

void restore_tests(void);
void restore_tests(void)
{
    snapshot_init(&E.snap);
    for (unsigned b = 0; b < FRAME_BASES_N; ++b)
        snapshot_init(&frame_bases_snap[b]);
    result_init(&E.reference);
    result_init(&E.actual);
    ledger_tests();
    /* The double first: a plumbing test of the drivers themselves (a
     * failure there is a harness bug, not a Tier1 one); then Tier1, the
     * evidence. */
    run_provider(t1h_provider_double());
    run_provider(t1h_provider_real());
    /* The product path: the same drivers and ledger with
     * every region entered from Tier0's lookups. */
    run_provider(t1h_provider_tier0t1());
    result_free(&E.reference);
    result_free(&E.actual);
    for (unsigned b = 0; b < FRAME_BASES_N; ++b)
        snapshot_free(&frame_bases_snap[b]);
    snapshot_free(&E.snap);
}
