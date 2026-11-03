/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FW_FPSWA_MODEL_H
#define FW_FPSWA_MODEL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ia64/decode/decode.h"

/* Compile the audited sources unchanged against a firmware-local execution
 * context. The VMM state (TLBs, RSE, MSRs, devices) would exhaust an OS kernel
 * interruption stack. This header defines no cross-module VMM contract. */
#define IA64VMM_IA64_SEMANTICS_H
struct ia64_fr {
    uint64_t significand;
    uint32_t exponent;
    bool sign;
};
struct ia64_state {
    struct ia64_fr fr[128];
    uint64_t pr, psr, cfm, ar[41];
};
struct vcpu {
    struct ia64_state s;
};
enum { AR_FPSR = 40 };
enum { IA64_STEP_COMPLETE, IA64_STEP_BRANCH, IA64_STEP_FAULT, IA64_STEP_EXIT };
enum {
    IA64_FAULT_NONE,
    IA64_FAULT_ILLEGAL,
    IA64_FAULT_RESERVED,
    IA64_FAULT_FP_DISABLED,
    IA64_FAULT_FP,
    IA64_FAULT_FP_TRAP
};
struct ia64_result {
    unsigned kind, fault;
    uint64_t info;
};
static inline struct ia64_result ia64_complete(void)
{
    return (struct ia64_result){0};
}
static inline struct ia64_result ia64_fault(unsigned fault, uint64_t info)
{
    return (struct ia64_result){IA64_STEP_FAULT, fault, info};
}
static inline struct ia64_result ia64_unimplemented(const Ia64Instruction *i)
{
    return (struct ia64_result){IA64_STEP_EXIT, 0, i->raw};
}
static inline bool ia64_opcode_supported(Ia64Opcode op)
{
    switch (op) {
#define IA64_SUPPORTED(name)                                                                       \
    case IA64_OP_##name:                                                                           \
        return true;
#include "ia64/decode/supported.inc"
#undef IA64_SUPPORTED
    default:
        return false;
    }
}
static inline unsigned ia64_fr_index(const struct ia64_state *s, unsigned r)
{
    return r < 32 ? r : 32 + (r - 32 + ((s->cfm >> 25) & 127)) % 96;
}
static inline unsigned ia64_pr_index(const struct ia64_state *s, unsigned r)
{
    return r < 16 ? r : 16 + (r - 16 + ((s->cfm >> 32) & 63)) % 48;
}
static inline bool ia64_pr_read(const struct ia64_state *s, unsigned r)
{
    return !r || ((s->pr >> ia64_pr_index(s, r)) & 1);
}
static inline void ia64_pr_write(struct ia64_state *s, unsigned r, bool value)
{
    if (r) {
        uint64_t bit = UINT64_C(1) << ia64_pr_index(s, r);
        s->pr = value ? s->pr | bit : s->pr & ~bit;
    }
}
/* The adapter admits only arithmetic instructions. A model getf/setf path
 * would indicate a programming error, never a successful emulation. */
static inline uint64_t ia64_gr_read(const struct ia64_state *s, unsigned r)
{
    (void)s;
    (void)r;
    __builtin_trap();
}
static inline bool ia64_nat_read(const struct ia64_state *s, unsigned r)
{
    (void)s;
    (void)r;
    __builtin_trap();
}
static inline void ia64_gr_write(struct ia64_state *s, unsigned r, uint64_t v, bool n)
{
    (void)s;
    (void)r;
    (void)v;
    (void)n;
    __builtin_trap();
}
#include "ia64/fp.h"
#endif
