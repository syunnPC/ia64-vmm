/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The guest programs: their images, the vCPU and guest-MMU
 * setup of each variant (PSR, region registers, TRs, VHPT, IVA), the data
 * they work on, and the results each program must produce on its own (the
 * engine comparisons are the suites' business; these checks show that a
 * run really executed the variant as designed: walker vs handler fills,
 * Dirty-Bit faults, aliases, the side exit).
 *
 * Register interface of the images: tests/tier1/programs/gather.inc, shladd_xor.S.
 */
#ifndef IA64VMM_TESTS_TIER1_PROGRAMS_H
#define IA64VMM_TESTS_TIER1_PROGRAMS_H
#include "tests/tier1/harness.h"

/* GUEST_MID_BLOCK and GUEST_WINDOW_TAGS exercise restoration at memory sites.
 * GUEST_RSE_RETURN reaches a hot header with an incomplete caller frame;
 * GUEST_RSE_RECURSION repeats it across frame bases, including wrapped frames
 * (SDM Vol.2 §6.8). GUEST_MMU_LOG_WRITERS covers each MMU-log writer family.
 * GUEST_STITCHED_OUTER stitches an outer loop into an inner-header region;
 * GUEST_LOOP_NEST places both loops in an outer-header region.
 * GUEST_CALL_RETURN covers calls, returns, alloc, chk.s and RSE spills.
 * GUEST_RSE_SPILL covers in-region alloc spills and RNaT collection; GUEST_VIRT
 * purges translated backing-store pages each iteration to force store faults.
 * GUEST_CALLEE_PAGES calls and rewrites other code pages; GUEST_VIRT remaps a
 * callee with smaller ITRs, after the first iteration with GUEST_EARLY_REMAP.
 * Guest sources are under tests/tier1/programs/. */
enum guest_kind {
    GUEST_GATHER,
    GUEST_SHLADD_XOR,
    GUEST_SPEED,
    GUEST_MID_BLOCK,
    GUEST_WINDOW_TAGS,
    GUEST_RSE_RETURN,
    GUEST_MMU_LOG_WRITERS,
    GUEST_STITCHED_OUTER,
    GUEST_LOOP_NEST,
    GUEST_CALL_RETURN,
    GUEST_RSE_SPILL,
    GUEST_CALLEE_PAGES
};
#define GUEST_VIRT (1u << 0)             /* PSR.it/dt/rt = 1: TRs, VHPT, guest fault handlers */
#define GUEST_STRIDE (1u << 1)           /* table stride 4 KiB: one page per S2 occurrence */
#define GUEST_ALIAS (1u << 2)            /* code via ITRs at VRN 0 and 4, output via VRN 3 and 5 */
#define GUEST_PHYS63 (1u << 3)           /* entered at the bit-63 physical alias of the code */
#define GUEST_SINGLE_STEP (1u << 4)      /* PSR.ss = 1: every instruction traps (lower-tier-step) */
#define GUEST_LOAD_AFTER_STORE (1u << 5) /* S4 reads the status word (gather-load-after-store) */
#define GUEST_RSE_RECURSION                                                                        \
    (1u << 6) /* GUEST_RSE_RETURN: rse_recursion.S, passes = recursion depth */
#define GUEST_EARLY_REMAP                                                                          \
    (1u << 7) /* GUEST_CALLEE_PAGES | GUEST_VIRT: the remap after the first iteration */

struct guest_program {
    const char *name;
    const char *image; /* basename, build/tests/tier1 or build/tests/ia64 */
    enum guest_kind kind;
    unsigned flags;
};
/* n: loop iterations per pass (gather) or in total (shladd_xor); passes (gather, >= 1; the
 * sentinel side exit is armed in the last pass only); bench: the output
 * pointer restarts every pass so passes can be many. */
struct guest_params {
    unsigned n, passes;
    bool bench;
    uint64_t seed;
};

extern const struct guest_program guest_programs[];
extern const unsigned guest_program_count;
const struct guest_program *guest_program(const char *name);

/* Small runs for every-k sweeps and the larger completion runs. */
struct guest_params guest_sweep_params(const struct guest_program *p);
struct guest_params guest_completion_params(const struct guest_program *p);

struct vcpu_config guest_vcpu_config(const struct guest_program *p);
/* Loads the image and the data into RAM and resets v to the program's
 * entry state (registers, PSR, IVA, region registers, TRs, PTA, VHPT). */
void guest_setup(const struct guest_program *p, const struct guest_params *q, struct vcpu *v);
/* After a run to completion: true if the program's own results hold;
 * otherwise false with the reason in why. */
bool guest_check(const struct guest_program *p, const struct guest_params *q, const struct vcpu *v,
                 const struct vcpu_exit *exit, const struct t1h_logs *logs, char *why,
                 size_t why_size);
#endif
