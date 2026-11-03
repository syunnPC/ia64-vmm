/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_TIER1_WINDOW_LOOP_H
#define IA64VMM_TIER1_WINDOW_LOOP_H
#include "engine/tier1/codegen.h"

/* A single counted loop. The entry guard runs after its edge copies.
 * Every access remains in order; only admitted tag/alignment tests disappear. */
struct t1_window_loop {
    bool member[T1_MIR_MAX_BLOCKS];
    uint16_t header, entry;
    unsigned insns;
    struct {
        uint32_t point, span;
        uint16_t site;
        uint8_t reg, bytes;
    } access[2];
};
bool t1_window_loop_plan(const struct t1_mir *m, struct t1_window_loop *p);
#endif
