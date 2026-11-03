/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The one definition of consumed/remaining budget: with budget_entry the raw R13 passed to the region entry,
 *   consumed(P)  = budget_entry - raw_R13 + P.retired_offset
 *   remaining(P) = budget_entry - consumed(P)
 * exact on every path by the block-start invariant (each block
 * subtracts its charge at its end; synthetic blocks charge the predicated
 * instruction on both paths). The materializer and the dispatcher both use
 * these; a fill/retry or miss/retry leaves raw R13 unchanged.
 */
#include "engine/tier1/exec.h"

uint64_t t1_point_consumed(const struct t1_exec *x, const struct t1_point *p)
{
    return x->budget_entry - x->raw_r13 + p->retired_offset;
}

uint64_t t1_point_remaining(const struct t1_exec *x, const struct t1_point *p)
{
    return x->budget_entry - t1_point_consumed(x, p);
}
