/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only IA-64 encoder for the Tier1 v1 instruction subset (compiler
 * tests: random programs and hand-built regions). Field layouts
 * are SDM Rev 2.3 Vol.3 Ch.4 instruction formats, cross-checked against
 * ia64/decode/decode.c by round-tripping every encoding in unit_asm.c
 * (UM_CHECK) -- the decoder is the oracle, not this file.
 */
#ifndef IA64VMM_TESTS_TIER1_IA64_ENC_H
#define IA64VMM_TESTS_TIER1_IA64_ENC_H
#include "ia64vmm/types.h"

/* Templates used by the tests (SDM Vol.3 Table 3-10). */
enum ienc_template {
    IENC_MII = 0x00,
    IENC_MLX = 0x04,
    IENC_MMI = 0x08,
    IENC_MIB = 0x10,
    IENC_MBB = 0x12,
    IENC_BBB = 0x16,
    IENC_MMB = 0x18,
};
/* Adds the "stop at end" variant (template | 1). */
#define IENC_STOP(t) ((t) | 1)

struct ienc_bundle {
    uint8_t tmpl;
    uint64_t slot[3];
};
void ienc_pack(const struct ienc_bundle *b, uint64_t *low, uint64_t *high);

/* A unit (usable in M or I slots). */
enum ienc_alu { IENC_ADD, IENC_ADD1, IENC_SUB, IENC_SUB1, IENC_AND, IENC_ANDCM, IENC_OR, IENC_XOR };
uint64_t ienc_alu(unsigned qp, enum ienc_alu op, unsigned r1, unsigned r2, unsigned r3);
/* Packed add/subtract, lane bits 8/16/32. sat=0 wraps; 8/16-bit lanes
 * also admit signed (1), unsigned (2), and mixed (3) saturation. */
uint64_t ienc_packed(unsigned qp, bool sub, unsigned bits, unsigned sat, unsigned r1, unsigned r2,
                     unsigned r3);
uint64_t ienc_shladd(unsigned qp, unsigned r1, unsigned r2, unsigned count, unsigned r3);
/* imm8 forms: sub r1 = imm8, r3; and/andcm/or/xor r1 = imm8, r3. */
uint64_t ienc_alu_imm8(unsigned qp, enum ienc_alu op, unsigned r1, int imm8, unsigned r3);
uint64_t ienc_adds(unsigned qp, unsigned r1, int imm14, unsigned r3);
uint64_t ienc_addl(unsigned qp, unsigned r1, int imm22, unsigned r3); /* r3 in r0..r3 */
enum ienc_rel { IENC_LT, IENC_LTU, IENC_EQ };
/* cmp/cmp4 (normal or .unc), register or imm8 (imm != NULL) forms. */
uint64_t ienc_cmp(unsigned qp, enum ienc_rel rel, bool cmp4, bool unc, unsigned p1, unsigned p2,
                  unsigned r2, unsigned r3);
uint64_t ienc_cmp_imm(unsigned qp, enum ienc_rel rel, bool cmp4, bool unc, unsigned p1, unsigned p2,
                      int imm8, unsigned r3);
/* Parallel compares (tb = 0, ta = 1): rel LT -> eq/ne.and, LTU -> eq/ne.or,
 * EQ -> eq/ne.or.andcm; ne selects the c bit. */
uint64_t ienc_cmp_parallel(unsigned qp, enum ienc_rel major, bool cmp4, bool ne, unsigned p1,
                           unsigned p2, unsigned r2, unsigned r3);

/* I unit. */
uint64_t ienc_shl(unsigned qp, unsigned r1, unsigned value, unsigned count);
uint64_t ienc_shr(unsigned qp, unsigned r1, unsigned value, unsigned count, bool unsigned_);
uint64_t ienc_extr(unsigned qp, unsigned r1, unsigned r3, unsigned pos, unsigned len, bool sign);
uint64_t ienc_depz(unsigned qp, unsigned r1, unsigned r2, unsigned pos, unsigned len);
uint64_t ienc_depz_imm(unsigned qp, unsigned r1, int imm8, unsigned pos, unsigned len);
/* bytes 1/2/4; sign selects sxt over zxt. */
uint64_t ienc_xt(unsigned qp, unsigned r1, unsigned r3, unsigned bytes, bool sign);
/* tbit/tnat with the raw tb, ta, c bits (SDM Vol.3 Table 4-23): 000 .z,
 * 001 .z.unc, then the parallel and/or/or.andcm forms. */
uint64_t ienc_tbit(unsigned qp, unsigned tb, unsigned ta, unsigned c, unsigned p1, unsigned p2,
                   unsigned r3, unsigned pos);
uint64_t ienc_tnat(unsigned qp, unsigned tb, unsigned ta, unsigned c, unsigned p1, unsigned p2,
                   unsigned r3);
uint64_t ienc_mov_ip(unsigned qp, unsigned r1);                  /* mov r1 = ip (I25) */
uint64_t ienc_mov_to_ar(unsigned qp, unsigned ar3, unsigned r2); /* mov ar3 = r2 (I26) */
uint64_t ienc_mov_to_br(unsigned qp, unsigned b1, unsigned r2);  /* mov b1 = r2 (I21) */
uint64_t ienc_nop_i(void);

/* M unit. bytes 1/2/4/8. */
uint64_t ienc_ld(unsigned qp, unsigned bytes, bool acq, unsigned r1, unsigned r3);
uint64_t ienc_ld_inc_reg(unsigned qp, unsigned bytes, bool acq, unsigned r1, unsigned r3,
                         unsigned r2);
uint64_t ienc_ld_inc_imm(unsigned qp, unsigned bytes, bool acq, unsigned r1, unsigned r3, int imm9);
uint64_t ienc_st(unsigned qp, unsigned bytes, bool rel, unsigned r3, unsigned r2);
uint64_t ienc_st_inc_imm(unsigned qp, unsigned bytes, bool rel, unsigned r3, unsigned r2, int imm9);
uint64_t ienc_mf(unsigned qp);
/* alloc r1 = ar.pfs with the raw sof, sol and sor (= rotating size / 8)
 * fields (M34); qp is encodable, alloc requires p0. */
uint64_t ienc_alloc(unsigned qp, unsigned r1, unsigned sof, unsigned sol, unsigned sor);
uint64_t ienc_nop_m(void);
/* break.m imm21 (not in the v1 subset: a region boundary). */
uint64_t ienc_break_m(unsigned imm21);

/* B unit. disp is the byte displacement to the target bundle (multiple
 * of 16). */
uint64_t ienc_br_cond(unsigned qp, int64_t disp);
uint64_t ienc_br_cloop(int64_t disp);
uint64_t ienc_br_call(unsigned qp, unsigned b1, int64_t disp); /* br.call b1 = target (B3) */
uint64_t ienc_br_call_indirect(unsigned qp, unsigned b1, unsigned b2); /* br.call b1 = b2 (B5) */
uint64_t ienc_br_ret(unsigned qp, unsigned b2);                        /* br.ret b2 (B4) */
uint64_t ienc_nop_b(void);

/* X: movl r1 = imm64 occupies slots 1 (L) and 2 (X) of an MLX bundle. */
void ienc_movl(unsigned qp, unsigned r1, uint64_t imm64, uint64_t *l_slot, uint64_t *x_slot);
#endif
