/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Random programs over the Tier1 v1 subset for compiler's ir_eval vs
 * interp_run test (restricted to what * needs): a counted loop of random bundles -- integer, shift/bitfield,
 * normal/.unc/parallel compares, tbit/tnat, predicated ops, post-increment
 * loads and stores (some misaligned), mf, forward branches inside the
 * body, side exits, the occasional region boundary -- entered with a
 * random CFM (sof/sol/sor, rrb.gr, rrb.pr), rse_bof, predicates and
 * random NaT bits.
 */
#ifndef IA64VMM_TESTS_TIER1_UM_RANDOM_H
#define IA64VMM_TESTS_TIER1_UM_RANDOM_H
#include "tests/tier1/um_machine.h"

#define UM_RANDOM_CODE (UM_CODE + 0x2000)

struct um_random_info {
    unsigned bundles, insns, memory_ops, helpers, branches, boundaries;
    bool nat;
};
/* Writes the program at UM_RANDOM_CODE and returns two vCPUs (test and
 * reference) at its entry with identical random state. */
void um_random_program(uint64_t seed, struct vcpu **v, struct vcpu **ref,
                       struct um_random_info *info);
#endif
