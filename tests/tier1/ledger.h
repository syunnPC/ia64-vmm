/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Restoration case generation and coverage accounting: a
 * restoration case counts only if the fault or exit was raised inside
 * generated code and the verification build recorded it. Every expected
 * (program, region, point, occurrence, action) gets a ledger entry; restore
 * records mark entries exercised when they match (forced or natural kind,
 * point, occurrence, path, outcome, materialization). An entry without a
 * matching record -- e.g. a point that was reached in the lower tier, which
 * produces no record -- fails as "not exercised". A forced record no entry
 * expects is a failure too (the run forced something nobody armed).
 */
#ifndef IA64VMM_TESTS_TIER1_LEDGER_H
#define IA64VMM_TESTS_TIER1_LEDGER_H
#include "engine/tier1/tier1.h"

enum ledger_action {
    LEDGER_TRAP_RETRY,  /* forced; path TRAP, outcome RETRY, not materialized */
    LEDGER_TRAP_EXIT,   /* forced; path TRAP, outcome EXIT, materialized */
    LEDGER_MISS_EXIT,   /* forced; path MISS_STUB, outcome EXIT, materialized */
    LEDGER_HELPER_EXIT, /* forced; path HELPER, outcome EXIT, materialized */
    LEDGER_GUEST_FAULT, /* natural (guest-fault-restore); materialized, translated */
    LEDGER_HOST_FILL,   /* natural (host-fill); path TRAP, outcome RETRY */
    LEDGER_INJECTED, /* natural (injected-resolve); path TRAP, outcome EXIT, materialized, not translated */
    LEDGER_CHECKPOINT, /* natural (checkpoint-exit); materialized, at a checkpoint point */
    LEDGER_ACTIONS
};
const char *ledger_action_name(enum ledger_action a);
/* The forced-exit action of a ledger action (LEDGER_TRAP_RETRY..HELPER_EXIT). */
enum t1_force_action ledger_force_action(enum ledger_action a);

/* occurrence 0: any occurrence; point LEDGER_ANY_POINT: any point (natural
 * classes, e.g. the n-th window fill of injected-resolve). */
#define LEDGER_ANY_POINT UINT32_MAX
struct ledger_case {
    char program[24];
    uint32_t region, point;
    uint64_t occurrence;
    enum ledger_action action;
    unsigned hits;
};

struct ledger {
    struct ledger_case *c;
    unsigned n, cap;
    unsigned unexpected;          /* forced records without an armed entry */
    unsigned occurrence_mismatch; /* forced records of the armed entry, other occurrence */
    char mismatch[160];           /* the first one, for the report */
    char unexpected_first[200];   /* the first unexpected forced record */
    unsigned records;
    char current[24]; /* program of the records being fed */
    int armed;        /* the entry the current run forces, or -1 */
    /* Point kinds of the current region (natural classes match by kind). */
    uint8_t kind[T1_MAX_POINTS];
    unsigned nkinds;
};

void ledger_init(struct ledger *l, unsigned cap);
void ledger_free(struct ledger *l);
void ledger_set_program(struct ledger *l, const char *program);
/* Returns the entry's index. */
unsigned ledger_expect(struct ledger *l, uint32_t region, uint32_t point, uint64_t occurrence,
                       enum ledger_action a);
/* The point kinds of the region whose records follow. */
void ledger_set_points(struct ledger *l, const struct t1_point_info *points, unsigned n);
/* The forced entry of the next run (-1: none). Every run arms at most one
 * forced case, so a forced record belongs to it: same region and
 * point and the action's shape. Its occurrence must equal the armed one;
 * a different value is reported as an occurrence mismatch (an interface
 * disagreement about how occurrences are counted), not as a coverage
 * hole. Assumption: Tier1 numbers a forced record's occurrence as
 * t1_test_force did (the n-th execution after arming). */
void ledger_arm(struct ledger *l, int entry);
/* forced-exit cases of one region: every SITE point x {TRAP_RETRY,
 * TRAP_EXIT, MISS_EXIT} and every HELPER point x HELPER_EXIT, each for
 * occurrences 1..max_occurrence. Returns the number added. */
unsigned ledger_expect_force(struct ledger *l, uint32_t region, const struct t1_point_info *points,
                             unsigned npoints, unsigned max_occurrence);
/* Feeds one restore record of the current program. True if it matched an
 * entry. LEDGER_CHECKPOINT ignores the path; natural classes require the
 * record's point to be of the kind they occur at (CHECKPOINT, or SITE). */
bool ledger_record(struct ledger *l, const struct t1_restore_record *r);
/* Entries not exercised plus unexpected forced records, each printed under
 * label (up to `limit` lines). 0 = complete coverage. */
unsigned ledger_report(const struct ledger *l, const char *label, unsigned limit);
#endif
