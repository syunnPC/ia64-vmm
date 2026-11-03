/* SPDX-License-Identifier: GPL-2.0-or-later */
/* In-place restoration comparison, harness side; see observe.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/observe.h"
#include "tests/tier1/reference.h"
#include "engine/tier1/tier1.h"
#include "engine/tier1/region.h" /* struct t1_vcpu: the entry's retired count */
#include "ia64/mmu.h"

void observe_init(struct observer *o, unsigned cap, struct schedule *sched)
{
    *o = (struct observer){.cap = cap, .sched = sched};
    o->c = calloc(cap, sizeof(*o->c));
    T1H_CHECK(o->c != NULL);
}

void observe_free(struct observer *o)
{
    free(o->c);
    *o = (struct observer){0};
}

void observe_reset(struct observer *o)
{
    o->n = 0;
    o->dropped = 0;
}

static void hash_ram(uint64_t *hash)
{
    const uint8_t *ram = t1h_ram();
    for (unsigned i = 0; i < T1H_RAM_PAGES; ++i)
        hash[i] = t1h_page_hash(ram + 4096 * (size_t)i);
}

void observe_capture(struct observer *o, const struct vcpu *v, bool translated, uint64_t gva,
                     bool write, uint32_t region, uint32_t point, uint64_t occurrence)
{
    /* The capture array is sized by the test; running out is a test error,
     * not a reason to skip a comparison. */
    T1H_CHECK(o->n < o->cap);
    struct observe_capture *c = &o->c[o->n++];
    const struct t1h_logs *logs = t1h_logs_of(v);
    T1H_CHECK(logs != NULL);
    c->retired = v->retired;
    c->at_entry = false;
    c->translated = translated;
    c->write = write;
    c->gva = gva;
    c->region = region;
    c->point = point;
    c->occurrence = occurrence;
    c->v = *v;
    hash_ram(c->hash);
    c->device_n = logs->device_n;
    c->store_n = logs->store_n;
    if (o->sched)
        schedule_resolve_pending(o->sched, v->retired);
}

void observe_t1_restore(struct t1_vcpu *tv, const struct vcpu *v, const struct t1_restore_record *r,
                        void *arg)
{
    (void)tv;
    struct observer *o = arg;
    if (!r->materialized) {
        ++o->dropped;
        return;
    }
    T1H_CHECK(r->retired == v->retired);
    observe_capture(o, v, r->translated, r->access_gva, r->access_write, r->region_id, r->point_id,
                    r->occurrence);
    if (tv && v->retired == tv->x.retired_entry)
        observe_mark_entry(o); /* tv: NULL from the plumbing double */
}

void observe_mark_entry(struct observer *o)
{
    T1H_CHECK(o->n > 0);
    o->c[o->n - 1].at_entry = true;
}

unsigned observe_verify(const struct observer *o, const struct snapshot *snap, struct vcpu *ref,
                        struct schedule *sched, const char *label)
{
    static uint64_t hash[T1H_RAM_PAGES];
    unsigned failed = 0;
    if (sched)
        schedule_rearm(sched);
    snapshot_restore(snap, ref);
    const struct t1h_logs *logs = t1h_logs_of(ref);
    T1H_CHECK(logs != NULL);
    for (unsigned i = 0; i < o->n; ++i) {
        const struct observe_capture *c = &o->c[i];
        struct vcpu_exit e = {0};
        char what[160];
        snprintf(what, sizeof(what),
                 "%s capture %u (region %u point %u occurrence %llu, "
                 "retired %llu%s)",
                 label, i, c->region, c->point, (unsigned long long)c->occurrence,
                 (unsigned long long)c->retired, c->translated ? ", after translation" : "");
        if (c->retired < ref->retired || !ref_run_to(ref, c->retired, sched, &e)) {
            if (!t1h_quiet(label))
                fprintf(stderr, "  %s: the reference cannot stop there (at %llu, exit %u)\n", what,
                        (unsigned long long)ref->retired, e.reason);
            ++failed;
            break;
        }
        if (c->at_entry) {
            /* The fetch translation the dispatcher ran before the entry
             * (observe.h): a TLB hit without effect unless CFLE was set or
             * the I-TLB missed, exactly as the reference's next fetch. */
            struct ia64_translation t;
            (void)ia64_translate(ref, ref->s.ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                                 (ref->s.psr >> 32) & 3, false, &t);
        }
        if (c->translated) {
            /* The access check hostwin_resolve ran for this access (it may have walked the VHPT and inserted a D-TC entry,
             * which the reference's own execution of the access then hits. */
            struct ia64_translation t;
            (void)ia64_translate(ref, c->gva, IA64_REF_DATA,
                                 c->write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ,
                                 (ref->s.psr >> 32) & 3, false, &t);
        }
        hash_ram(hash);
        struct compare_run x = {NULL, ref, NULL, 0, NULL, 0, NULL, 0, hash, T1H_RAM_PAGES};
        struct compare_run y = {NULL, &c->v, NULL, 0, NULL, 0, NULL, 0, c->hash, T1H_RAM_PAGES};
        struct t1h_diff_printer printer = {what, 0, 8};
        unsigned n = compare_runs(&x, &y, t1h_print_diff, &printer);
        if (logs->device_n != c->device_n || logs->store_n != c->store_n) {
            if (!t1h_quiet(label))
                fprintf(stderr,
                        "  %s: log lengths device %zu/%zu stores %zu/%zu (reference/actual)\n",
                        what, logs->device_n, c->device_n, logs->store_n, c->store_n);
            ++n;
        }
        if (n)
            ++failed;
    }
    return failed;
}
