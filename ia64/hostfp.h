/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * IA-64 FP arithmetic on host IEEE single/double (SSE, FMA3) when the result
 * register image and every FPSR effect equal the soft-float's (ia64/fp*.c,
 * the reference); the conditions are listed in hostfp.c. Classification
 * is integer-only; hostfp_sse.c performs the host operation and is a stub
 * where the C build has no SIMD (no __SSE2__).
 */
#ifndef IA64VMM_IA64_HOSTFP_H
#define IA64VMM_IA64_HOSTFP_H
#include "ia64/fp.h"

enum ia64_hostfp_format { IA64_HOSTFP_NONE, IA64_HOSTFP_SINGLE, IA64_HOSTFP_DOUBLE };
/* MUL: b*c. MULADD: b*c then + a, two host roundings, the first exact
 * unless fp_approx. FMA: b*c + a with one rounding (host FMA3). */
enum ia64_hostfp_kind { IA64_HOSTFP_MUL, IA64_HOSTFP_MULADD, IA64_HOSTFP_FMA };

/* Outcome of a candidate operation (statistics keep one counter each). */
enum ia64_hostfp_reason {
    IA64_HOSTFP_DOWN,      /* executed on the host */
    IA64_HOSTFP_PRECISION, /* 64-bit precision and an inexact host result; reserved pc */
    IA64_HOSTFP_OPERAND,   /* finite operand outside the host format's precision or range */
    IA64_HOSTFP_SPECIAL,   /* NaN, infinity or unsupported encoding */
    IA64_HOSTFP_UNNORMAL,  /* unnormalized operand: denormal, pseudo-zero, integer form */
    IA64_HOSTFP_NOFMA,     /* fused multiply-add with an inexact product, no host FMA3 */
    IA64_HOSTFP_RESULT,    /* host flag other than inexact, or a denormal result */
    IA64_HOSTFP_TRAP,      /* inexact result with the inexact trap enabled */
    IA64_HOSTFP_HOST,      /* no host execution in this build */
    IA64_HOSTFP_REASONS
};

#define IA64_HOSTFP_OPT_FMA3 1u         /* host fused multiply-add is usable */
#define IA64_HOSTFP_OPT_APPROX 2u       /* fp_approx=1: approximate 24/53-bit arithmetic */
#define IA64_HOSTFP_OPT_EXPERIMENTAL 8u /* explicit opt-in to direct Tier1 FP */
#define IA64_HOSTFP_OPT_SOFT 4u         /* no engine uses host FP: fp_host=0 or no host execution */

/* Host exception flags in MXCSR bits 5:0 order (IE DE ZE OE UE PE), which
 * is the order of the FPSR status field flags v d z o u i. */
#define IA64_HOSTFP_PE 32u

struct ia64_hostfp_plan {
    uint8_t format; /* compute format */
    uint8_t result; /* result format; SINGLE computed in DOUBLE only under fp_approx */
    uint8_t kind;
    bool exact_only;  /* 64-bit precision: only an exact host result is the IA-64 result */
    uint64_t a, b, c; /* host operand bits: addend (fms sign applied), b (fnma sign applied), c */
};

/* Scalar fma, fms, fnma and their pseudo-ops, with the operands after
 * ia64_fp_compute's rewrites: b * c + a, operation 1 negates a, 2 the
 * product, multiply replaces a by a zero of the product's sign. */
enum ia64_hostfp_reason ia64_hostfp_plan(struct ia64_fr a, struct ia64_fr b, struct ia64_fr c,
                                         unsigned operation, bool multiply, unsigned controls,
                                         unsigned precision, unsigned options,
                                         struct ia64_hostfp_plan *plan);
/* The host result with its flags; masks are the effective trap disables. */
enum ia64_hostfp_reason ia64_hostfp_accept(const struct ia64_hostfp_plan *plan, uint64_t bits,
                                           unsigned host_flags, unsigned masks, struct ia64_fr *out,
                                           unsigned *flags);
/* fpma, fpms, fpnma: two singles in bits 31:0 and 63:32 of each operand. */
struct ia64_hostfp_packed {
    uint8_t kind;
    uint64_t a, b, c;
};
enum ia64_hostfp_reason ia64_hostfp_plan_packed(uint64_t a, uint64_t b, uint64_t c,
                                                unsigned operation, bool multiply, unsigned options,
                                                struct ia64_hostfp_packed *plan);
enum ia64_hostfp_reason ia64_hostfp_accept_packed(uint64_t bits, unsigned host_flags,
                                                  unsigned masks, unsigned *flags);

/* Host execution (hostfp_sse.c): rc is FPSR.sfx.rc, equal to MXCSR.RC. */
bool ia64_hostfp_run(const struct ia64_hostfp_plan *plan, unsigned rc, uint64_t *bits,
                     unsigned *host_flags);
bool ia64_hostfp_run_packed(const struct ia64_hostfp_packed *plan, unsigned rc, uint64_t *bits,
                            unsigned *host_flags);
/* Host fused multiply-add: CPUID.1:ECX.FMA with the AVX state enabled. */
bool ia64_hostfp_fma3(void);
/* Host execution in this build: SIMD in C, whose builds own the SIMD state
 * (the IA-32 container swaps it); Tier1 emits host FP only then. */
bool ia64_hostfp_host(void);

/* ia64_execute's FP family entry: host execution where exact, else ia64_fp. */
struct ia64_result ia64_hostfp(struct vcpu *v, const Ia64Instruction *insn);

/* Configuration, before vCPUs run: ENGINE_FP_* (include/ia64vmm/engine.h). */
#define IA64_HOSTFP_CFG_APPROX ENGINE_FP_APPROX
#define IA64_HOSTFP_CFG_SOFT ENGINE_FP_SOFT
#define IA64_HOSTFP_CFG_CHECK ENGINE_FP_CHECK
#define IA64_HOSTFP_CFG_CLASSIFY ENGINE_FP_CLASSIFY
#define IA64_HOSTFP_CFG_EXPERIMENTAL ENGINE_FP_EXPERIMENTAL
#define IA64_HOSTFP_CFG_NOFMA3 32u /* tests: as without host FMA3 */
void ia64_hostfp_configure(unsigned flags);
unsigned ia64_hostfp_options(void);

/* Statistics per vCPU index: candidate scalar and packed operations by
 * outcome, and the other FP arithmetic instructions. */
enum ia64_hostfp_class { IA64_HOSTFP_SCALAR, IA64_HOSTFP_PARALLEL, IA64_HOSTFP_CLASSES };
struct ia64_hostfp_stats {
    uint64_t candidate[IA64_HOSTFP_CLASSES][IA64_HOSTFP_REASONS];
    uint64_t other;
};
void ia64_hostfp_stats(unsigned index, struct ia64_hostfp_stats *out);
void ia64_hostfp_stats_reset(void);
const char *ia64_hostfp_reason_name(enum ia64_hostfp_reason reason);
#endif
