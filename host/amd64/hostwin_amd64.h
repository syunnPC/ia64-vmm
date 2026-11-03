/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_HOSTWIN_AMD64_H
#define VMM_HOSTWIN_AMD64_H
#include "engine/hostwin/hostwin.h"
/* Stopped executors only: the context of an executor's host CPU, one
 * reservation in its private root; NULL for any other CPU. */
void *amd64_hostwin_context(unsigned cpu);
struct amd64_hostwin_stats {
    uint64_t maps, unmaps, protects, flushes, failures;
    uint64_t page_flushes, all_flushes;
    unsigned shadow_tables;
};
/* One context's statistics, or the totals of all with ctx NULL. */
void amd64_hostwin_get_stats(void *ctx, struct amd64_hostwin_stats *stats);
#endif
