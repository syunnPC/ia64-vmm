/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * engine/engine.c's composition of Tier0 with Tier1 as its upper tier:
 * Tier0 includes no Tier1 header and Tier1 no Tier0 header, so engine.c builds Tier0's
 * struct tier0_upper_ops from Tier1's struct t1_engine_glue.
 */
#ifndef IA64VMM_ENGINE_UPPER_H
#define IA64VMM_ENGINE_UPPER_H
#include "engine/tier0/tier0.h"

/* Tier0's upper-tier operations backed by Tier1 (Tier1 must be
 * initialized: the thresholds and the sequence are Tier1's), or NULL when
 * Tier1 is not linked into the binary. engine=tier1 registers them with
 * tier0_set_upper; the Tier1 harness's `tier0+tier1` engine registers the
 * same operations around each of its runs. */
const struct tier0_upper_ops *engine_tier1_ops(void);
#endif
