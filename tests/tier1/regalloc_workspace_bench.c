/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Record allocation requests and peak carved/pool bytes of the real RA
 * stage. Only the correctness pass is traced; use the paired RA driver
 * for timings so these allocation hooks are absent from those binaries. */
#include <stdio.h>
#include <stdlib.h>
#include "tests/tier1/um_t1.h"
#include "engine/tier1/codegen.h"
#include "engine/tier1/memory.h"

bool t1_regalloc_reference(struct t1_mir *m);
bool __real_t1_regalloc(struct t1_mir *m);
void *__real_t1_ws_alloc(struct t1_workspace *ws, size_t pages, unsigned flags);

static bool tracing;
static unsigned input, nrequests;
static size_t peak_carved;
static struct {
    size_t pages;
    unsigned flags;
    bool success;
} requests[32];

void *__wrap_t1_ws_alloc(struct t1_workspace *ws, size_t pages, unsigned flags);
void *__wrap_t1_ws_alloc(struct t1_workspace *ws, size_t pages, unsigned flags)
{
    void *p = __real_t1_ws_alloc(ws, pages, flags);
    if (tracing) {
        UM_CHECK(nrequests < sizeof(requests) / sizeof(requests[0]));
        requests[nrequests].pages = pages;
        requests[nrequests].flags = flags;
        requests[nrequests++].success = p != NULL;
        if (ws && ws->top > peak_carved)
            peak_carved = ws->top;
    }
    return p;
}

bool __wrap_t1_regalloc(struct t1_mir *m);
bool __wrap_t1_regalloc(struct t1_mir *m)
{
    bool reference = getenv("T1_REGALLOC_REFERENCE") != NULL;
    if (!t1_test_workspace_poison)
        return reference ? t1_regalloc_reference(m) : __real_t1_regalloc(m);
    unsigned old_insns = m->ninsns, cap = m->insn_cap;
    size_t before_top = m->ir->ws ? m->ir->ws->top : 0;
    struct t1_memory_stats before, after;
    t1_memory_get_stats(&before);
    t1_memory_test_reset_peaks();
    nrequests = 0;
    peak_carved = before_top;
    tracing = true;
    bool ok = reference ? t1_regalloc_reference(m) : __real_t1_regalloc(m);
    tracing = false;
    t1_memory_get_stats(&after);
    UM_CHECK(ok && after.workspace_bytes == before.workspace_bytes);
    UM_CHECK(!m->ir->ws || m->ir->ws->top == before_top);
    ++input;
    printf("workspace,%u,%s,%u,%u,%u,%zu,%llu,%llu,%llu\n", input, reference ? "base" : "new",
           old_insns, m->ninsns, cap, (peak_carved - before_top) * 4096,
           (unsigned long long)(after.workspace_peak - before.workspace_bytes),
           (unsigned long long)before.workspace_bytes, (unsigned long long)before.reserve_bytes);
    for (unsigned k = 0; k < nrequests; ++k)
        printf("request,%u,%u,%zu,%u,%u\n", input, k, requests[k].pages * 4096, requests[k].flags,
               requests[k].success);
    return ok;
}
