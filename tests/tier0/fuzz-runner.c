/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/state.h"
#ifndef TIER0_FUZZ_ENGINE
#define TIER0_FUZZ_ENGINE ENGINE_TIER0
#endif
static struct vcpu *fuzz_create(const struct vcpu_config *config)
{
    struct vcpu_config selected = *config;
    selected.engine = TIER0_FUZZ_ENGINE;
    return vcpu_create(&selected);
}
/* Keep the existing complete-state observer and terminal hook identical. */
#define vcpu_create fuzz_create
#include "tests/fuzz/runner.c"
#undef vcpu_create
