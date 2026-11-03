/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Random v1-subset programs (see um_random.h). Pointers live in r2-r5 and
 * the register increment in r9; no generated instruction writes them
 * except post-increments by at most 16, so every access stays inside the machine's
 * data area (some misaligned: the lower tier then raises Unaligned Data
 * Reference and the guest spins in the machine's IVT, on both sides).
 */
#include "tests/tier1/um_random.h"

static uint64_t state;
static uint64_t r64(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}
static unsigned rnd(unsigned n)
{
    return n ? (unsigned)(r64() % n) : 0;
}
static bool chance(unsigned percent)
{
    return rnd(100) < percent;
}

struct gen {
    unsigned sof;
    unsigned boundary_rate;
    struct um_random_info *info;
};

static unsigned qp(void)
{
    return chance(50) ? 0 : 1 + rnd(63);
}
static unsigned in_frame(const struct gen *g)
{
    unsigned k = rnd(22 + g->sof);
    return k < 22 ? 10 + k : 32 + (k - 22);
}
static unsigned scratch(const struct gen *g)
{
    if (g->boundary_rate && chance(g->boundary_rate) && 32 + g->sof < 128) {
        ++g->info->boundaries;
        return 32 + g->sof; /* outside the frame: region boundary */
    }
    return in_frame(g);
}
static unsigned source(const struct gen *g)
{
    switch (rnd(8)) {
    case 0:
        return 0;
    case 1:
        return 2 + rnd(4);
    case 2:
        return 9;
    default:
        return in_frame(g);
    }
}
static void preds(unsigned *p1, unsigned *p2)
{
    *p1 = rnd(64);
    do
        *p2 = rnd(64);
    while (*p2 == *p1);
}

static uint64_t a_type(struct gen *g)
{
    unsigned p1, p2, k = rnd(100);
    if (k < 35)
        return ienc_alu(qp(), rnd(8), scratch(g), source(g), source(g));
    if (k < 45)
        return ienc_shladd(qp(), scratch(g), source(g), 1 + rnd(4), source(g));
    if (k < 55) {
        static const enum ienc_alu ops[] = {IENC_SUB, IENC_AND, IENC_ANDCM, IENC_OR, IENC_XOR};
        return ienc_alu_imm8(qp(), ops[rnd(5)], scratch(g), (int)rnd(256) - 128, source(g));
    }
    if (k < 63)
        return ienc_adds(qp(), scratch(g), (int)rnd(1u << 14) - (1 << 13), source(g));
    if (k < 67)
        return ienc_addl(qp(), scratch(g), (int)rnd(1u << 22) - (1 << 21), rnd(4));
    preds(&p1, &p2);
    if (k < 80)
        return ienc_cmp(qp(), rnd(3), rnd(2), rnd(2), p1, p2, source(g), source(g));
    if (k < 90)
        return ienc_cmp_imm(qp(), rnd(3), rnd(2), rnd(2), p1, p2, (int)rnd(256) - 128, source(g));
    ++g->info->helpers;
    return ienc_cmp_parallel(qp(), rnd(3), rnd(2), rnd(2), p1, p2, source(g), source(g));
}

static uint64_t i_type(struct gen *g)
{
    unsigned p1, p2, k = rnd(100), pos = rnd(64), len = 1 + rnd(64);
    if (k < 50)
        return a_type(g);
    if (k < 58)
        return ienc_shl(qp(), scratch(g), source(g), source(g));
    if (k < 66)
        return ienc_shr(qp(), scratch(g), source(g), source(g), rnd(2));
    if (k < 74)
        return ienc_extr(qp(), scratch(g), source(g), pos, len, rnd(2));
    if (k < 80)
        return ienc_depz(qp(), scratch(g), source(g), pos, len);
    if (k < 84)
        return ienc_depz_imm(qp(), scratch(g), (int)rnd(256) - 128, pos, len);
    if (k < 88)
        return ienc_xt(qp(), scratch(g), source(g), 1u << rnd(3), rnd(2));
    if (k < 90)
        return ienc_mov_ip(qp(), scratch(g));
    preds(&p1, &p2);
    ++g->info->helpers;
    if (k < 96)
        return ienc_tbit(qp(), rnd(2), rnd(2), rnd(2), p1, p2, source(g), pos);
    return ienc_tnat(qp(), rnd(2), rnd(2), rnd(2), p1, p2, source(g));
}

static int imm9(unsigned bytes)
{
    int step = chance(80) ? (int)bytes : 1;
    return step * ((int)rnd(5) - 2);
}

static uint64_t m_type(struct gen *g)
{
    unsigned k = rnd(100), bytes = 1u << rnd(4), base = 2 + rnd(4);
    if (k < 30) {
        ++g->info->memory_ops;
        unsigned form = rnd(3), dst = scratch(g);
        bool acq = chance(20);
        if (form == 0)
            return ienc_ld(qp(), bytes, acq, dst, base);
        if (form == 1)
            return ienc_ld_inc_imm(qp(), bytes, acq, dst, base, imm9(bytes));
        return ienc_ld_inc_reg(qp(), bytes, acq, dst, base, 9);
    }
    if (k < 50) {
        ++g->info->memory_ops;
        bool rel = chance(20);
        if (chance(50))
            return ienc_st(qp(), bytes, rel, base, source(g));
        return ienc_st_inc_imm(qp(), bytes, rel, base, source(g), imm9(bytes));
    }
    if (k < 54)
        return ienc_mf(qp());
    if (g->boundary_rate && chance(g->boundary_rate)) {
        ++g->info->boundaries;
        return ienc_break_m(0);
    }
    return a_type(g);
}

void um_random_program(uint64_t seed, struct vcpu **vp, struct vcpu **refp,
                       struct um_random_info *info)
{
    state = seed * UINT64_C(0x9e3779b97f4a7c15) + 1;
    for (unsigned i = 0; i < 4; ++i)
        r64();
    *info = (struct um_random_info){0};
    struct gen g = {.sof = 8 + rnd(89), .boundary_rate = chance(25) ? 2 : 0, .info = info};
    unsigned nb = 3 + rnd(10);
    struct ienc_bundle b[16];
    for (unsigned i = 0; i < nb; ++i) {
        bool last = i == nb - 1;
        unsigned kind = last ? 3 : rnd(5);
        struct ienc_bundle *x = &b[i];
        switch (kind) {
        case 0:
            *x = (struct ienc_bundle){IENC_MII, {m_type(&g), i_type(&g), i_type(&g)}};
            break;
        case 1:
            *x = (struct ienc_bundle){IENC_MMI, {m_type(&g), m_type(&g), i_type(&g)}};
            break;
        case 2: {
            uint64_t l, xs;
            ienc_movl(qp(), scratch(&g), r64(), &l, &xs);
            *x = (struct ienc_bundle){IENC_MLX, {m_type(&g), l, xs}};
            break;
        }
        default: {
            uint64_t br;
            if (last) {
                br = ienc_br_cloop(-(int64_t)(16 * i));
            } else if (chance(50)) {
                /* Forward: inside the body, to the loop's fall-through, or
                 * further (exits). */
                unsigned target = i + 1 + rnd(nb - i + 2);
                br = ienc_br_cond(qp(), (int64_t)(16 * (target - i)));
                ++info->branches;
            } else {
                br = ienc_nop_b();
            }
            *x = chance(50) ? (struct ienc_bundle){IENC_MIB, {m_type(&g), i_type(&g), br}}
                            : (struct ienc_bundle){IENC_MMB, {m_type(&g), m_type(&g), br}};
            break;
        }
        }
        if (chance(30))
            x->tmpl = (uint8_t)IENC_STOP(x->tmpl);
        info->insns += x->tmpl == IENC_MLX || x->tmpl == IENC_STOP(IENC_MLX) ? 2 : 3;
    }
    struct ienc_bundle spin = {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}};
    for (unsigned i = nb; i < nb + 3; ++i)
        b[i] = spin;
    um_emit(UM_RANDOM_CODE, b, nb + 3);
    info->bundles = nb;

    /* Entered through the bit-63 physical alias or a virtual alias in some
     * programs: every IP-derived value is the executing guest IP. */
    unsigned alias = rnd(100);
    uint64_t entry = UM_RANDOM_CODE | (alias < 15   ? UINT64_C(1) << 63
                                       : alias < 30 ? UINT64_C(1) << 61
                                                    : 0);
    struct vcpu *v = um_vcpu(entry);
    if (alias >= 15 && alias < 30)
        um_virtual_code(v);
    unsigned sor = 8 * rnd(g.sof / 8 + 1);
    v->s.cfm = um_cfm(g.sof, rnd(g.sof + 1), sor, sor ? rnd(sor) : 0, rnd(48));
    v->s.rse_bof = rnd(96);
    for (unsigned r = 1; r < 32 + g.sof && r < 128; ++r)
        um_gr(v, r, chance(30) ? rnd(256) : r64(), false);
    for (unsigned r = 2; r <= 5; ++r) {
        uint64_t p = UM_DATA + 0x1000 + (r - 2) * 0x8000 + (rnd(0x1000) & ~7u);
        if (chance(15))
            p += 1 + rnd(7);
        um_gr(v, r, p, false);
    }
    um_gr(v, 9, (uint64_t)(int64_t)(8 * ((int)rnd(5) - 2)), false);
    v->s.pr = r64() | 1;
    v->s.ar[AR_LC] = 1 + rnd(6);
    if (chance(20)) {
        info->nat = true;
        for (unsigned n = 1 + rnd(3); n; --n)
            um_gr(v, in_frame(&g), r64(), true);
        /* The increment register or a pointer: NaT propagation through
         * post-increments and Register NaT Consumption on the base. */
        if (chance(40)) {
            unsigned r = chance(50) ? 9 : 2 + rnd(4);
            v->s.nat[ia64_gr_index(&v->s, r)] = true;
        }
    }
    *vp = v;
    *refp = um_vcpu(entry);
    **refp = *v;
}
