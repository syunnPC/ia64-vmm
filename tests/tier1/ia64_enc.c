/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only IA-64 encoder (see ia64_enc.h). Formats: SDM Rev 2.3 Vol.3 §4.2
 * (A1-A8), §4.3 (I5, I7, I11-I13, I16, I17, I29), §4.4 (M1-M5, M24, M34, M48),
 * §4.5 (B1, B3, B4, B9), §4.7 (X2).
 */
#include "tests/tier1/ia64_enc.h"

#define F(v, lo, w) ((((uint64_t)(v)) & ((UINT64_C(1) << (w)) - 1)) << (lo))

void ienc_pack(const struct ienc_bundle *b, uint64_t *low, uint64_t *high)
{
    uint64_t s0 = b->slot[0] & ((UINT64_C(1) << 41) - 1);
    uint64_t s1 = b->slot[1] & ((UINT64_C(1) << 41) - 1);
    uint64_t s2 = b->slot[2] & ((UINT64_C(1) << 41) - 1);
    *low = (b->tmpl & 31u) | (s0 << 5) | (s1 << 46);
    *high = (s1 >> 18) | (s2 << 23);
}

static const uint8_t alu_x4[] = {0, 0, 1, 1, 3, 3, 3, 3};
static const uint8_t alu_x2b[] = {0, 1, 1, 0, 0, 1, 2, 3};

uint64_t ienc_alu(unsigned qp, enum ienc_alu op, unsigned r1, unsigned r2, unsigned r3)
{
    return F(8, 37, 4) | F(alu_x4[op], 29, 4) | F(alu_x2b[op], 27, 2) | F(r3, 20, 7) |
           F(r2, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_packed(unsigned qp, bool sub, unsigned bits, unsigned sat, unsigned r1, unsigned r2,
                     unsigned r3)
{
    return F(8, 37, 4) | F(bits == 32, 36, 1) | F(1, 34, 2) | F(bits == 16, 33, 1) | F(sub, 29, 4) |
           F(sat, 27, 2) | F(r3, 20, 7) | F(r2, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_shladd(unsigned qp, unsigned r1, unsigned r2, unsigned count, unsigned r3)
{
    return F(8, 37, 4) | F(4, 29, 4) | F(count - 1, 27, 2) | F(r3, 20, 7) | F(r2, 13, 7) |
           F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_alu_imm8(unsigned qp, enum ienc_alu op, unsigned r1, int imm8, unsigned r3)
{
    unsigned x4 = op == IENC_SUB ? 9 : 0xb, x2b = op == IENC_SUB ? 1 : alu_x2b[op];
    return F(8, 37, 4) | F((unsigned)imm8 >> 7, 36, 1) | F(x4, 29, 4) | F(x2b, 27, 2) |
           F(r3, 20, 7) | F(imm8, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_adds(unsigned qp, unsigned r1, int imm14, unsigned r3)
{
    uint64_t i = (uint64_t)(int64_t)imm14;
    return F(8, 37, 4) | F(i >> 13, 36, 1) | F(2, 34, 2) | F(i >> 7, 27, 6) | F(r3, 20, 7) |
           F(i, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_addl(unsigned qp, unsigned r1, int imm22, unsigned r3)
{
    uint64_t i = (uint64_t)(int64_t)imm22;
    return F(9, 37, 4) | F(i >> 21, 36, 1) | F(i >> 7, 27, 9) | F(i >> 16, 22, 5) | F(r3, 20, 2) |
           F(i, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

static const uint8_t cmp_major[] = {0xc, 0xd, 0xe};

uint64_t ienc_cmp(unsigned qp, enum ienc_rel rel, bool cmp4, bool unc, unsigned p1, unsigned p2,
                  unsigned r2, unsigned r3)
{
    return F(cmp_major[rel], 37, 4) | F(cmp4, 34, 2) | F(p2, 27, 6) | F(r3, 20, 7) | F(r2, 13, 7) |
           F(unc, 12, 1) | F(p1, 6, 6) | F(qp, 0, 6);
}

uint64_t ienc_cmp_imm(unsigned qp, enum ienc_rel rel, bool cmp4, bool unc, unsigned p1, unsigned p2,
                      int imm8, unsigned r3)
{
    return F(cmp_major[rel], 37, 4) | F((unsigned)imm8 >> 7, 36, 1) | F(cmp4 ? 3 : 2, 34, 2) |
           F(p2, 27, 6) | F(r3, 20, 7) | F(imm8, 13, 7) | F(unc, 12, 1) | F(p1, 6, 6) | F(qp, 0, 6);
}

uint64_t ienc_cmp_parallel(unsigned qp, enum ienc_rel major, bool cmp4, bool ne, unsigned p1,
                           unsigned p2, unsigned r2, unsigned r3)
{
    return F(cmp_major[major], 37, 4) | F(cmp4, 34, 2) | F(1, 33, 1) | F(p2, 27, 6) | F(r3, 20, 7) |
           F(r2, 13, 7) | F(ne, 12, 1) | F(p1, 6, 6) | F(qp, 0, 6);
}

uint64_t ienc_shl(unsigned qp, unsigned r1, unsigned value, unsigned count)
{
    return F(7, 37, 4) | F(1, 36, 1) | F(1, 33, 1) | F(8, 27, 6) | F(count, 20, 7) |
           F(value, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_shr(unsigned qp, unsigned r1, unsigned value, unsigned count, bool unsigned_)
{
    return F(7, 37, 4) | F(1, 36, 1) | F(1, 33, 1) | F(unsigned_ ? 0 : 4, 27, 6) | F(value, 20, 7) |
           F(count, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_extr(unsigned qp, unsigned r1, unsigned r3, unsigned pos, unsigned len, bool sign)
{
    return F(5, 37, 4) | F(1, 34, 2) | F(len - 1, 27, 6) | F(r3, 20, 7) | F(pos, 14, 6) |
           F(sign, 13, 1) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_depz(unsigned qp, unsigned r1, unsigned r2, unsigned pos, unsigned len)
{
    return F(5, 37, 4) | F(1, 34, 2) | F(1, 33, 1) | F(len - 1, 27, 6) | F(63 - pos, 20, 6) |
           F(r2, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_depz_imm(unsigned qp, unsigned r1, int imm8, unsigned pos, unsigned len)
{
    return F(5, 37, 4) | F((unsigned)imm8 >> 7, 36, 1) | F(1, 34, 2) | F(1, 33, 1) |
           F(len - 1, 27, 6) | F(1, 26, 1) | F(63 - pos, 20, 6) | F(imm8, 13, 7) | F(r1, 6, 7) |
           F(qp, 0, 6);
}

uint64_t ienc_xt(unsigned qp, unsigned r1, unsigned r3, unsigned bytes, bool sign)
{
    unsigned x6 = (sign ? 0x14 : 0x10) + (bytes == 1 ? 0 : bytes == 2 ? 1 : 2);
    return F(0, 37, 4) | F(x6, 27, 6) | F(r3, 20, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_tbit(unsigned qp, unsigned tb, unsigned ta, unsigned c, unsigned p1, unsigned p2,
                   unsigned r3, unsigned pos)
{
    return F(5, 37, 4) | F(tb, 36, 1) | F(ta, 33, 1) | F(p2, 27, 6) | F(r3, 20, 7) | F(pos, 14, 6) |
           F(c, 12, 1) | F(p1, 6, 6) | F(qp, 0, 6);
}

uint64_t ienc_tnat(unsigned qp, unsigned tb, unsigned ta, unsigned c, unsigned p1, unsigned p2,
                   unsigned r3)
{
    return F(5, 37, 4) | F(tb, 36, 1) | F(ta, 33, 1) | F(p2, 27, 6) | F(r3, 20, 7) | F(1, 13, 1) |
           F(c, 12, 1) | F(p1, 6, 6) | F(qp, 0, 6);
}

uint64_t ienc_mov_ip(unsigned qp, unsigned r1)
{
    return F(0x30, 27, 6) | F(r1, 6, 7) | F(qp, 0, 6);
}
uint64_t ienc_mov_to_ar(unsigned qp, unsigned ar3, unsigned r2)
{
    return F(0x2a, 27, 6) | F(ar3, 20, 7) | F(r2, 13, 7) | F(qp, 0, 6);
}
uint64_t ienc_nop_i(void)
{
    return F(1, 27, 6);
}

static unsigned size_code(unsigned bytes)
{
    return bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3;
}

uint64_t ienc_ld(unsigned qp, unsigned bytes, bool acq, unsigned r1, unsigned r3)
{
    return F(4, 37, 4) | F((acq ? 0x14 : 0) + size_code(bytes), 30, 6) | F(r3, 20, 7) |
           F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_ld_inc_reg(unsigned qp, unsigned bytes, bool acq, unsigned r1, unsigned r3,
                         unsigned r2)
{
    return F(4, 37, 4) | F(1, 36, 1) | F((acq ? 0x14 : 0) + size_code(bytes), 30, 6) |
           F(r3, 20, 7) | F(r2, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_ld_inc_imm(unsigned qp, unsigned bytes, bool acq, unsigned r1, unsigned r3, int imm9)
{
    uint64_t i = (uint64_t)(int64_t)imm9;
    return F(5, 37, 4) | F(i >> 8, 36, 1) | F((acq ? 0x14 : 0) + size_code(bytes), 30, 6) |
           F(i >> 7, 27, 1) | F(r3, 20, 7) | F(i, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_st(unsigned qp, unsigned bytes, bool rel, unsigned r3, unsigned r2)
{
    return F(4, 37, 4) | F((rel ? 0x34 : 0x30) + size_code(bytes), 30, 6) | F(r3, 20, 7) |
           F(r2, 13, 7) | F(qp, 0, 6);
}

uint64_t ienc_st_inc_imm(unsigned qp, unsigned bytes, bool rel, unsigned r3, unsigned r2, int imm9)
{
    uint64_t i = (uint64_t)(int64_t)imm9;
    return F(5, 37, 4) | F(i >> 8, 36, 1) | F((rel ? 0x34 : 0x30) + size_code(bytes), 30, 6) |
           F(i >> 7, 27, 1) | F(r3, 20, 7) | F(r2, 13, 7) | F(i, 6, 7) | F(qp, 0, 6);
}

uint64_t ienc_alloc(unsigned qp, unsigned r1, unsigned sof, unsigned sol, unsigned sor)
{
    return F(1, 37, 4) | F(6, 33, 3) | F(sor, 27, 4) | F(sol, 20, 7) | F(sof, 13, 7) | F(r1, 6, 7) |
           F(qp, 0, 6);
}
uint64_t ienc_mf(unsigned qp)
{
    return F(0x22, 27, 6) | F(qp, 0, 6);
}
uint64_t ienc_nop_m(void)
{
    return F(1, 27, 6);
}
uint64_t ienc_break_m(unsigned imm21)
{
    return F(imm21, 6, 20) | F(imm21 >> 20, 36, 1);
}

uint64_t ienc_br_cond(unsigned qp, int64_t disp)
{
    uint64_t i = (uint64_t)(disp >> 4);
    return F(4, 37, 4) | F(i >> 20, 36, 1) | F(i, 13, 20) | F(qp, 0, 6);
}

uint64_t ienc_br_call(unsigned qp, unsigned b1, int64_t disp)
{
    uint64_t i = (uint64_t)(disp >> 4);
    return F(5, 37, 4) | F(i >> 20, 36, 1) | F(i, 13, 20) | F(b1, 6, 3) | F(qp, 0, 6);
}

uint64_t ienc_br_ret(unsigned qp, unsigned b2)
{
    return F(0x21, 27, 6) | F(b2, 13, 3) | F(4, 6, 3) | F(qp, 0, 6);
}

uint64_t ienc_br_call_indirect(unsigned qp, unsigned b1, unsigned b2)
{
    return F(1, 37, 4) | F(b2, 13, 3) | F(b1, 6, 3) | F(qp, 0, 6);
}

uint64_t ienc_mov_to_br(unsigned qp, unsigned b1, unsigned r2)
{
    return F(7, 33, 3) | F(r2, 13, 7) | F(b1, 6, 3) | F(qp, 0, 6);
}

uint64_t ienc_br_cloop(int64_t disp)
{
    uint64_t i = (uint64_t)(disp >> 4);
    return F(4, 37, 4) | F(i >> 20, 36, 1) | F(i, 13, 20) | F(5, 6, 3);
}

uint64_t ienc_nop_b(void)
{
    return F(2, 37, 4);
}

void ienc_movl(unsigned qp, unsigned r1, uint64_t imm64, uint64_t *l_slot, uint64_t *x_slot)
{
    *l_slot = F(imm64 >> 22, 0, 41);
    *x_slot = F(6, 37, 4) | F(imm64 >> 63, 36, 1) | F(imm64 >> 7, 27, 9) | F(imm64 >> 16, 22, 5) |
              F(imm64 >> 21, 21, 1) | F(imm64, 13, 7) | F(r1, 6, 7) | F(qp, 0, 6);
}
