/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Restoration coverage accounting; see ledger.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/ledger.h"
#include "tests/tier1/harness.h"

static const char *const names[LEDGER_ACTIONS] = {
    "TRAP_RETRY",  "TRAP_EXIT", "MISS_EXIT",     "HELPER_EXIT",
    "guest fault", "host fill", "injected exit", "checkpoint",
};

const char *ledger_action_name(enum ledger_action a)
{
    return a < LEDGER_ACTIONS ? names[a] : "?";
}

enum t1_force_action ledger_force_action(enum ledger_action a)
{
    switch (a) {
    case LEDGER_TRAP_RETRY:
        return T1_FORCE_TRAP_RETRY;
    case LEDGER_TRAP_EXIT:
        return T1_FORCE_TRAP_EXIT;
    case LEDGER_MISS_EXIT:
        return T1_FORCE_MISS_EXIT;
    default:
        return T1_FORCE_HELPER_EXIT;
    }
}

void ledger_init(struct ledger *l, unsigned cap)
{
    *l = (struct ledger){.cap = cap, .armed = -1};
    l->c = calloc(cap, sizeof(*l->c));
    T1H_CHECK(l->c != NULL);
}

void ledger_free(struct ledger *l)
{
    free(l->c);
    *l = (struct ledger){0};
}

void ledger_set_program(struct ledger *l, const char *program)
{
    snprintf(l->current, sizeof(l->current), "%s", program);
}

unsigned ledger_expect(struct ledger *l, uint32_t region, uint32_t point, uint64_t occurrence,
                       enum ledger_action a)
{
    T1H_CHECK(l->n < l->cap);
    struct ledger_case *c = &l->c[l->n++];
    *c = (struct ledger_case){
        .region = region, .point = point, .occurrence = occurrence, .action = a};
    snprintf(c->program, sizeof(c->program), "%s", l->current);
    return l->n - 1;
}

void ledger_set_points(struct ledger *l, const struct t1_point_info *points, unsigned n)
{
    T1H_CHECK(n <= T1_MAX_POINTS);
    memset(l->kind, 0xff, sizeof(l->kind));
    for (unsigned i = 0; i < n; ++i)
        if (points[i].point_id < T1_MAX_POINTS)
            l->kind[points[i].point_id] = (uint8_t)points[i].kind;
    l->nkinds = T1_MAX_POINTS;
}

void ledger_arm(struct ledger *l, int entry)
{
    T1H_CHECK(entry < (int)l->n);
    l->armed = entry;
}

unsigned ledger_expect_force(struct ledger *l, uint32_t region, const struct t1_point_info *points,
                             unsigned npoints, unsigned max_occurrence)
{
    unsigned added = 0;
    for (unsigned i = 0; i < npoints; ++i) {
        static const enum ledger_action site[] = {LEDGER_TRAP_RETRY, LEDGER_TRAP_EXIT,
                                                  LEDGER_MISS_EXIT};
        static const enum ledger_action helper[] = {LEDGER_HELPER_EXIT};
        const enum ledger_action *acts = points[i].kind == T1_PT_SITE ? site : helper;
        unsigned nacts = points[i].kind == T1_PT_SITE ? 3 : points[i].kind == T1_PT_HELPER ? 1 : 0;
        for (unsigned a = 0; a < nacts; ++a)
            for (uint64_t o = 1; o <= max_occurrence; ++o, ++added)
                ledger_expect(l, region, points[i].point_id, o, acts[a]);
    }
    return added;
}

static bool kind_is(const struct ledger *l, uint32_t point, enum t1_point_kind kind)
{
    return !l->nkinds || (point < T1_MAX_POINTS && l->kind[point] == kind);
}

/* Does the record have the shape of the action (tier1.h)? */
static bool shape(const struct ledger *l, const struct t1_restore_record *r, enum ledger_action a)
{
    switch (a) {
    case LEDGER_TRAP_RETRY:
        return r->forced && r->path == T1_PATH_TRAP && r->outcome == T1_OUTCOME_RETRY &&
               !r->materialized;
    case LEDGER_TRAP_EXIT:
        return r->forced && r->path == T1_PATH_TRAP && r->outcome == T1_OUTCOME_EXIT &&
               r->materialized;
    case LEDGER_MISS_EXIT:
        return r->forced && r->path == T1_PATH_MISS_STUB && r->outcome == T1_OUTCOME_EXIT &&
               r->materialized;
    case LEDGER_HELPER_EXIT:
        return r->forced && r->path == T1_PATH_HELPER && r->outcome == T1_OUTCOME_EXIT &&
               r->materialized;
    case LEDGER_GUEST_FAULT:
        return !r->forced && r->outcome == T1_OUTCOME_EXIT && r->materialized && r->translated &&
               kind_is(l, r->point_id, T1_PT_SITE);
    case LEDGER_HOST_FILL:
        return !r->forced && r->path == T1_PATH_TRAP && r->outcome == T1_OUTCOME_RETRY &&
               kind_is(l, r->point_id, T1_PT_SITE);
    case LEDGER_INJECTED:
        return !r->forced && r->path == T1_PATH_TRAP && r->outcome == T1_OUTCOME_EXIT &&
               r->materialized && !r->translated && kind_is(l, r->point_id, T1_PT_SITE);
    case LEDGER_CHECKPOINT:
        return !r->forced && r->materialized && r->outcome == T1_OUTCOME_EXIT &&
               kind_is(l, r->point_id, T1_PT_CHECKPOINT);
    default:
        return false;
    }
}

bool ledger_record(struct ledger *l, const struct t1_restore_record *r)
{
    ++l->records;
    if (r->forced) {
        /* The armed case only: same region, point and shape. */
        struct ledger_case *c = l->armed >= 0 ? &l->c[l->armed] : NULL;
        if (!c || strcmp(c->program, l->current) || c->region != r->region_id ||
            c->point != r->point_id || !shape(l, r, c->action)) {
            if (!l->unexpected++)
                snprintf(l->unexpected_first, sizeof(l->unexpected_first),
                         "%s region %u point %u occurrence %llu path %u outcome %u materialized %d "
                         "(armed: %s)",
                         l->current, r->region_id, r->point_id, (unsigned long long)r->occurrence,
                         r->path, r->outcome, r->materialized,
                         c ? ledger_action_name(c->action) : "none");
            return false;
        }
        if (c->occurrence != r->occurrence) {
            if (!l->occurrence_mismatch++)
                snprintf(l->mismatch, sizeof(l->mismatch),
                         "%s region %u point %u %s: record "
                         "occurrence %llu, armed %llu",
                         c->program, c->region, c->point, ledger_action_name(c->action),
                         (unsigned long long)r->occurrence, (unsigned long long)c->occurrence);
            return false;
        }
        ++c->hits;
        return true;
    }
    bool matched = false;
    for (unsigned i = 0; i < l->n; ++i) {
        struct ledger_case *c = &l->c[i];
        if (c->action <= LEDGER_HELPER_EXIT || strcmp(c->program, l->current) ||
            c->region != r->region_id ||
            (c->point != LEDGER_ANY_POINT && c->point != r->point_id) ||
            (c->occurrence && c->occurrence != r->occurrence) || !shape(l, r, c->action))
            continue;
        ++c->hits;
        matched = true;
    }
    return matched;
}

unsigned ledger_report(const struct ledger *l, const char *label, unsigned limit)
{
    unsigned missing = 0, printed = 0;
    bool quiet = t1h_quiet(label);
    for (unsigned i = 0; i < l->n; ++i) {
        const struct ledger_case *c = &l->c[i];
        if (c->hits)
            continue;
        ++missing;
        if (!quiet && printed++ < limit) {
            char occ[24];
            if (c->occurrence)
                snprintf(occ, sizeof(occ), "%llu", (unsigned long long)c->occurrence);
            else
                snprintf(occ, sizeof(occ), "any");
            char point[24];
            if (c->point == LEDGER_ANY_POINT)
                snprintf(point, sizeof(point), "any");
            else
                snprintf(point, sizeof(point), "%u", c->point);
            fprintf(stderr, "  %s: not exercised: %s region %u point %s occurrence %s %s\n", label,
                    c->program, c->region, point, occ, ledger_action_name(c->action));
        }
    }
    if (l->unexpected && !quiet)
        fprintf(stderr, "  %s: %u forced record(s) of no armed case, first: %s\n", label,
                l->unexpected, l->unexpected_first);
    if (l->occurrence_mismatch && !quiet)
        fprintf(stderr,
                "  %s: %u forced record(s) with another occurrence than armed "
                "(occurrence numbering differs from t1_test_force's), first: %s\n",
                label, l->occurrence_mismatch, l->mismatch);
    return missing + l->unexpected + l->occurrence_mismatch;
}
