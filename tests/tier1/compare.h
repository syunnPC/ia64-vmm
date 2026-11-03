/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The comparison relation of every Tier1 test:
 * a run under test against an interp_run reference started from the same
 * snapshot under the same injection schedule. compare.c lists what is
 * compared and what is excluded, with the reasons.
 */
#ifndef IA64VMM_TESTS_TIER1_COMPARE_H
#define IA64VMM_TESTS_TIER1_COMPARE_H
#include "ia64/state.h"

/* One transaction of the test MMIO device, in issue order. */
struct compare_device_record {
    uint64_t address, value;
    uint8_t size;
    bool write;
};
/* One completed guest store (verification builds): the merged
 * Tier1 + lower-tier log. Compared tuple, in completion order: (gva, width,
 * value & mask(width)); gva is the effective (pre-increment) guest virtual
 * address -- native records log it, the reference/lower-tier side derives
 * it from the decoded store and the pre-instruction state. */
struct compare_store_record {
    uint64_t gva, value;
    uint8_t width;
};

struct compare_run {
    /* The outermost result of the call under test (t1_run vs interp_run):
     * reason, ip, slot, info compared; detail (diagnostic) excluded. NULL
     * for in-place captures (restore observer); NULL vs non-NULL is one
     * difference. */
    const struct vcpu_exit *exit;
    const struct vcpu *v;
    const uint8_t *ram; /* all bytes of the test RAM */
    uint64_t ram_bytes;
    const struct compare_device_record *device;
    size_t device_count;
    const struct compare_store_record *stores; /* NULL: no store log in this run */
    size_t store_count;
    /* In-place captures: with ram == NULL the test RAM is
     * compared as one hash per 4 KiB page instead (ram_pages entries); a
     * difference names the page ("ram_page"). Both runs must use the same
     * form. */
    const uint64_t *ram_hash;
    size_t ram_pages;
};

/* Called once per difference; what names the field, e.g. "s.gr[5]",
 * "ram[0x1008]", "device[3].value". index is an element index or byte
 * offset where that applies. */
typedef void (*compare_diff_fn)(const char *what, uint64_t index, uint64_t reference,
                                uint64_t actual, void *arg);
/* Returns the number of differences (0 = equal under the comparison relation). A store log
 * present in only one run is one difference. */
unsigned compare_runs(const struct compare_run *reference, const struct compare_run *actual,
                      compare_diff_fn diff, void *arg);
/* Verifies that compare.c's member lists cover every byte of struct
 * ia64_state and struct vcpu except alignment padding; false (with a log
 * line) if a member was added without classifying it. */
bool compare_lists_complete(void);
#endif
