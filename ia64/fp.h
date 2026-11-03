/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_FP_H
#define IA64VMM_IA64_FP_H
#include "ia64/semantics.h"

#define IA64_FP_BIAS 0xffff
#define IA64_FP_INTEGER_EXP 0x1003e
#define IA64_FP_NAT_EXP 0x1fffe
#define IA64_FP_SPECIAL_EXP 0x1ffff
#define IA64_FP_INTEGER_BIT (UINT64_C(1) << 63)
#define IA64_FP_QUIET_BIT (UINT64_C(1) << 62)
#define IA64_PSR_DFL (UINT64_C(1) << 18)
#define IA64_PSR_DFH (UINT64_C(1) << 19)
#define IA64_PSR_MFL (UINT64_C(1) << 4)
#define IA64_PSR_MFH (UINT64_C(1) << 5)

enum ia64_fp_flags { FP_V = 1, FP_D = 2, FP_Z = 4, FP_O = 8, FP_U = 16, FP_I = 32 };
struct ia64_fp_raw {
    bool sign;
    int32_t exp;
    uint64_t mant;
    bool tail, half_greater, half_equal;
};
struct ia64_fr ia64_fr_read(const struct ia64_state *s, unsigned reg);
void ia64_fr_write(struct ia64_state *s, unsigned reg, struct ia64_fr value);
bool ia64_fr_is_nat(struct ia64_fr value);
struct ia64_fr ia64_fp_nat(void);
struct ia64_fr ia64_fp_from_ieee(uint64_t bits, bool single);
uint64_t ia64_fp_to_ieee(struct ia64_fr value, bool single);
struct ia64_result ia64_fp_disabled(const struct ia64_state *s, unsigned a, unsigned b, unsigned c,
                                    unsigned d);
/* FP/FP_DISABLED info carries ISR.code. FP_TRAP includes the FP indication
 * bit and is completing; interruption delivery remains the engine's job. */
struct ia64_result ia64_fp(struct vcpu *v, const Ia64Instruction *insn);
/* Small private FR/FPSR context for scalar arithmetic and packed lanes. */
struct ia64_fp_context {
    struct ia64_fr a, b, c, out;
    uint64_t fpsr;
    bool pr1, pr2;
};
struct ia64_result ia64_fp_compute(struct ia64_fp_context *ctx, const Ia64Instruction *insn);
struct ia64_result ia64_fp_parallel(struct vcpu *v, const Ia64Instruction *insn);
struct ia64_result ia64_fp_memory(struct vcpu *v, const Ia64Instruction *insn);

/* Exact product/addition and one final rounding; no host FP state is used. */
bool ia64_fp_muladd(const struct ia64_fr *addend, const struct ia64_fr *left,
                    const struct ia64_fr *right, unsigned operation, unsigned rc,
                    struct ia64_fp_raw *raw);
__uint128_t ia64_fp_round_right(const struct ia64_fp_raw *raw, uint32_t shift, uint32_t rc,
                                bool *inexact, bool *fpa);
bool ia64_fp_round(const struct ia64_fp_raw *raw, unsigned controls, unsigned masks,
                   unsigned precision, struct ia64_fr *out, unsigned *flags, bool *fpa);
#endif
