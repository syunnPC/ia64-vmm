/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit test of the test-only encoder (ia64_enc.c): every form round-trips
 * through the project's decoder (ia64_decode_insn / ia64_decode_bundle)
 * with the expected opcode and operands, for random operands.
 */
#include <stdio.h>
#include <stdlib.h>
#include "tests/tier1/um_machine.h"
#include "engine/cache/cache.h"

static uint64_t rng = 0x243f6a8885a308d3u;
static unsigned rnd(unsigned n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng % n);
}

static Ia64Instruction dec(IA64SlotUnit unit, uint64_t raw)
{
    Ia64Instruction in = ia64_decode_insn(unit, raw, 0x1000, 0);
    UM_CHECK(in.valid && in.encoding_class == IA64_ENCODING_DEFINED && !in.reserved_field);
    return in;
}

#define OPS(in) (&(in).operands.common)

void unit_asm_test(void);
void unit_asm_test(void)
{
    unsigned checked = 0;
    for (unsigned iter = 0; iter < 2000; ++iter) {
        unsigned qp = rnd(64), r1 = 1 + rnd(127), r2 = rnd(128), r3 = rnd(128), p1 = rnd(64),
                 p2 = rnd(64);
        static const Ia64Opcode alu[] = {IA64_OP_ADD, IA64_OP_ADD_ONE, IA64_OP_SUB, IA64_OP_SUB_ONE,
                                         IA64_OP_AND, IA64_OP_ANDCM,   IA64_OP_OR,  IA64_OP_XOR};
        unsigned op = rnd(8);
        Ia64Instruction in = dec(rnd(2) ? IA64_UNIT_I : IA64_UNIT_M, ienc_alu(qp, op, r1, r2, r3));
        UM_CHECK(in.opcode == alu[op] && in.qp == qp && OPS(in)->destination == r1 &&
                 OPS(in)->source1 == r2 && OPS(in)->source2 == r3);
        static const Ia64Opcode packed[2][3] = {{IA64_OP_PADD1, IA64_OP_PADD2, IA64_OP_PADD4},
                                                {IA64_OP_PSUB1, IA64_OP_PSUB2, IA64_OP_PSUB4}};
        for (unsigned sub = 0; sub < 2; ++sub)
            for (unsigned width = 0; width < 3; ++width)
                for (unsigned sat = 0; sat < (width == 2 ? 1u : 4u); ++sat) {
                    in = dec((iter + sub + width + sat) & 1 ? IA64_UNIT_I : IA64_UNIT_M,
                             ienc_packed(qp, sub, 8u << width, sat, r1, r2, r3));
                    UM_CHECK(in.opcode == packed[sub][width] && in.qp == qp &&
                             OPS(in)->destination == r1 && OPS(in)->source1 == r2 &&
                             OPS(in)->source2 == r3 && OPS(in)->immediate == sat);
                    ++checked;
                }
        unsigned ct = 1 + rnd(4);
        in = dec(IA64_UNIT_I, ienc_shladd(qp, r1, r2, ct, r3));
        UM_CHECK(in.opcode == IA64_OP_SHLADD && OPS(in)->immediate == ct &&
                 OPS(in)->source1 == r2 && OPS(in)->source2 == r3 && OPS(in)->destination == r1);
        int imm8 = (int)rnd(256) - 128;
        static const enum ienc_alu immops[] = {IENC_SUB, IENC_AND, IENC_ANDCM, IENC_OR, IENC_XOR};
        static const Ia64Opcode immcodes[] = {IA64_OP_SUB_IMM, IA64_OP_AND_IMM, IA64_OP_ANDCM_IMM,
                                              IA64_OP_OR_IMM, IA64_OP_XOR_IMM};
        unsigned k = rnd(5);
        in = dec(IA64_UNIT_M, ienc_alu_imm8(qp, immops[k], r1, imm8, r3));
        UM_CHECK(in.opcode == immcodes[k] && OPS(in)->immediate == imm8 && OPS(in)->source2 == r3 &&
                 OPS(in)->destination == r1);
        int imm14 = (int)rnd(1u << 14) - (1 << 13);
        in = dec(IA64_UNIT_I, ienc_adds(qp, r1, imm14, r3));
        UM_CHECK(in.opcode == IA64_OP_ADDS && OPS(in)->immediate == imm14 &&
                 OPS(in)->source2 == r3);
        int imm22 = (int)rnd(1u << 22) - (1 << 21);
        in = dec(IA64_UNIT_M, ienc_addl(qp, r1, imm22, r3 & 3));
        UM_CHECK(in.opcode == IA64_OP_ADDL && OPS(in)->immediate == imm22 &&
                 OPS(in)->source2 == (r3 & 3));

        static const Ia64Opcode cmps[2][3] = {{IA64_OP_CMP_LT, IA64_OP_CMP_LTU, IA64_OP_CMP_EQ},
                                              {IA64_OP_CMP4_LT, IA64_OP_CMP4_LTU, IA64_OP_CMP4_EQ}};
        static const Ia64Opcode cmpi[2][3] = {
            {IA64_OP_CMP_LT_IMM, IA64_OP_CMP_LTU_IMM, IA64_OP_CMP_EQ_IMM},
            {IA64_OP_CMP4_LT_IMM, IA64_OP_CMP4_LTU_IMM, IA64_OP_CMP4_EQ_IMM}};
        unsigned rel = rnd(3), c4 = rnd(2), unc = rnd(2);
        in = dec(IA64_UNIT_I, ienc_cmp(qp, rel, c4, unc, p1, p2, r2, r3));
        UM_CHECK(in.opcode == cmps[c4][rel] && in.compare_unc == unc && OPS(in)->auxiliary1 == p1 &&
                 OPS(in)->auxiliary2 == p2 && OPS(in)->source1 == r2 && OPS(in)->source2 == r3);
        in = dec(IA64_UNIT_M, ienc_cmp_imm(qp, rel, c4, unc, p1, p2, imm8, r3));
        UM_CHECK(in.opcode == cmpi[c4][rel] && in.compare_unc == unc &&
                 OPS(in)->immediate == imm8 && OPS(in)->source2 == r3 && OPS(in)->auxiliary1 == p1);
        static const Ia64Opcode par[2][3][2] = {
            {{IA64_OP_CMP_EQ_AND, IA64_OP_CMP_NE_AND},
             {IA64_OP_CMP_EQ_OR, IA64_OP_CMP_NE_OR},
             {IA64_OP_CMP_EQ_OR_ANDCM, IA64_OP_CMP_NE_OR_ANDCM}},
            {{IA64_OP_CMP4_EQ_AND, IA64_OP_CMP4_NE_AND},
             {IA64_OP_CMP4_EQ_OR, IA64_OP_CMP4_NE_OR},
             {IA64_OP_CMP4_EQ_OR_ANDCM, IA64_OP_CMP4_NE_OR_ANDCM}}};
        unsigned ne = rnd(2);
        in = dec(IA64_UNIT_I, ienc_cmp_parallel(qp, rel, c4, ne, p1, p2, r2, r3));
        UM_CHECK(in.opcode == par[c4][rel][ne] && OPS(in)->source1 == r2 &&
                 OPS(in)->source2 == r3 && OPS(in)->auxiliary1 == p1 && OPS(in)->auxiliary2 == p2);

        in = dec(IA64_UNIT_I, ienc_shl(qp, r1, r2, r3));
        UM_CHECK(in.opcode == IA64_OP_SHL && OPS(in)->source1 == r3 && OPS(in)->source2 == r2);
        bool u = rnd(2);
        in = dec(IA64_UNIT_I, ienc_shr(qp, r1, r2, r3, u));
        UM_CHECK(in.opcode == (u ? IA64_OP_SHRU : IA64_OP_SHR) && OPS(in)->source1 == r3 &&
                 OPS(in)->source2 == r2);
        unsigned pos = rnd(64), len = 1 + rnd(64);
        bool sg = rnd(2);
        in = dec(IA64_UNIT_I, ienc_extr(qp, r1, r3, pos, len, sg));
        UM_CHECK(in.opcode == (sg ? IA64_OP_EXTR : IA64_OP_EXTRU) && OPS(in)->source2 == r3 &&
                 (OPS(in)->immediate & 63) == pos && (OPS(in)->immediate >> 6) == len);
        in = dec(IA64_UNIT_I, ienc_depz(qp, r1, r2, pos, len));
        UM_CHECK(in.opcode == IA64_OP_DEPZ && OPS(in)->source1 == r2 &&
                 (OPS(in)->immediate & 63) == pos && ((OPS(in)->immediate >> 6) & 127) == len);
        in = dec(IA64_UNIT_I, ienc_depz_imm(qp, r1, imm8, pos, len));
        UM_CHECK(in.opcode == IA64_OP_DEPZ_IMM && (OPS(in)->immediate & 63) == pos &&
                 ((OPS(in)->immediate >> 6) & 127) == len &&
                 ia64_sign_extend((uint64_t)OPS(in)->immediate >> 13, 8) == imm8);
        static const unsigned widths[] = {1, 2, 4};
        static const Ia64Opcode xts[2][3] = {{IA64_OP_ZXT1, IA64_OP_ZXT2, IA64_OP_ZXT4},
                                             {IA64_OP_SXT1, IA64_OP_SXT2, IA64_OP_SXT4}};
        unsigned w = rnd(3);
        in = dec(IA64_UNIT_I, ienc_xt(qp, r1, r3, widths[w], sg));
        UM_CHECK(in.opcode == xts[sg][w] && OPS(in)->source2 == r3 && OPS(in)->destination == r1);
        unsigned tb = rnd(2), ta = rnd(2), cc = rnd(2);
        in = dec(IA64_UNIT_I, ienc_tbit(qp, tb, ta, cc, p1, p2, r3, pos));
        UM_CHECK((in.opcode == IA64_OP_TBIT_Z || in.opcode == IA64_OP_TBIT_NZ) &&
                 OPS(in)->source2 == r3 && OPS(in)->immediate == pos &&
                 in.compare_unc == (!tb && !ta && cc));
        in = dec(IA64_UNIT_I, ienc_tnat(qp, tb, ta, cc, p1, p2, r3));
        UM_CHECK((in.opcode == IA64_OP_TNAT_Z || in.opcode == IA64_OP_TNAT_NZ) &&
                 OPS(in)->source2 == r3);

        in = dec(IA64_UNIT_I, ienc_mov_ip(qp, r1));
        UM_CHECK(in.opcode == IA64_OP_MOV_CURRENT_IP && OPS(in)->destination == r1 && in.qp == qp);
        unsigned ar3 = 64 + rnd(3); /* PFS, LC, EC: I-unit ARs */
        in = dec(IA64_UNIT_I, ienc_mov_to_ar(qp, ar3, r2));
        UM_CHECK(in.opcode == IA64_OP_MOV_GRAR && OPS(in)->source1 == ar3 &&
                 OPS(in)->destination == r2 && in.qp == qp);
        static const unsigned bytes[] = {1, 2, 4, 8};
        static const Ia64Opcode lds[] = {IA64_OP_LD1, IA64_OP_LD2, IA64_OP_LD4, IA64_OP_LD8};
        static const Ia64Opcode sts[2][4] = {
            {IA64_OP_ST1, IA64_OP_ST2, IA64_OP_ST4, IA64_OP_ST8},
            {IA64_OP_ST1REL, IA64_OP_ST2REL, IA64_OP_ST4REL, IA64_OP_ST8REL}};
        unsigned b = rnd(4);
        bool acq = rnd(2);
        int imm9 = (int)rnd(512) - 256;
        const IA64MemoryOperands *m;
        in = dec(IA64_UNIT_M, ienc_ld(qp, bytes[b], acq, r1, r3));
        m = &in.operands.memory;
        UM_CHECK(in.opcode == lds[b] && in.mem_acquire == acq && m->destination == r1 &&
                 m->base == r3 && !in.reg_base_update && !in.imm_base_update);
        in = dec(IA64_UNIT_M, ienc_ld_inc_reg(qp, bytes[b], acq, r1, r3, r2));
        UM_CHECK(in.opcode == lds[b] && in.reg_base_update && m->source == r2 && m->base == r3);
        in = dec(IA64_UNIT_M, ienc_ld_inc_imm(qp, bytes[b], acq, r1, r3, imm9));
        UM_CHECK(in.opcode == lds[b] && in.imm_base_update && m->immediate == imm9 &&
                 m->destination == r1);
        in = dec(IA64_UNIT_M, ienc_st(qp, bytes[b], acq, r3, r2));
        UM_CHECK(in.opcode == sts[acq][b] && m->base == r3 && m->source == r2 &&
                 !in.imm_base_update);
        in = dec(IA64_UNIT_M, ienc_st_inc_imm(qp, bytes[b], acq, r3, r2, imm9));
        UM_CHECK(in.opcode == sts[acq][b] && in.imm_base_update && m->immediate == imm9 &&
                 m->source == r2);
        in = dec(IA64_UNIT_M, ienc_mf(qp));
        UM_CHECK(in.opcode == IA64_OP_MF && in.qp == qp);
        unsigned sof = rnd(128), sol = rnd(128), sor = rnd(16);
        in = dec(IA64_UNIT_M, ienc_alloc(qp, r1, sof, sol, sor));
        UM_CHECK(in.opcode == IA64_OP_ALLOC && in.qp == qp && OPS(in)->destination == r1 &&
                 in.operands.system.immediate == (int64_t)(sof | sol << 7 | sor << 14));

        int64_t disp = ((int64_t)rnd(1u << 21) - (1 << 20)) * 16;
        in = dec(IA64_UNIT_B, ienc_br_cond(qp, disp));
        UM_CHECK(in.opcode == IA64_OP_BR_COND && in.operands.branch.displacement == disp &&
                 in.qp == qp);
        in = dec(IA64_UNIT_B, ienc_br_cloop(disp));
        UM_CHECK(in.opcode == IA64_OP_BR_CLOOP && in.operands.branch.displacement == disp);
        unsigned breg = rnd(8);
        in = dec(IA64_UNIT_B, ienc_br_call(qp, breg, disp));
        UM_CHECK(in.opcode == IA64_OP_BR_CALL && in.operands.branch.displacement == disp &&
                 in.operands.branch.link == breg && in.qp == qp);
        unsigned target = rnd(8);
        in = dec(IA64_UNIT_B, ienc_br_call_indirect(qp, breg, target));
        UM_CHECK(in.opcode == IA64_OP_BR_CALL_INDIRECT && in.operands.branch.target == target &&
                 in.operands.branch.link == breg && in.qp == qp);
        in = dec(IA64_UNIT_I, ienc_mov_to_br(qp, breg, r2));
        UM_CHECK(in.opcode == IA64_OP_MOV_GRBR && in.operands.system.source == breg &&
                 in.operands.common.destination == r2 && in.qp == qp);
        in = dec(IA64_UNIT_B, ienc_br_ret(qp, breg));
        UM_CHECK(in.opcode == IA64_OP_BR_RET && in.operands.branch.target == breg && in.qp == qp);

        uint64_t imm64 = ((uint64_t)rnd(1u << 31) << 33) ^ ((uint64_t)rnd(1u << 31) << 2) ^ rnd(4);
        struct ienc_bundle mlx = {IENC_MLX, {ienc_nop_m(), 0, 0}};
        ienc_movl(qp, r1, imm64, &mlx.slot[1], &mlx.slot[2]);
        uint64_t lo, hi;
        ienc_pack(&mlx, &lo, &hi);
        struct ia64_bundle bd;
        ia64_decode_bundle(lo, hi, 0x1000, &bd);
        UM_CHECK(bd.long_instruction && bd.insn[1].opcode == IA64_OP_MOVL && bd.insn[1].qp == qp &&
                 (uint64_t)bd.insn[1].operands.common.immediate == imm64 &&
                 bd.insn[1].operands.common.destination == r1 && bd.insn[0].opcode == IA64_OP_NOP);
        checked += 32;
    }
    UM_CHECK(dec(IA64_UNIT_I, ienc_nop_i()).opcode == IA64_OP_NOP);
    UM_CHECK(dec(IA64_UNIT_M, ienc_nop_m()).opcode == IA64_OP_NOP);
    UM_CHECK(dec(IA64_UNIT_B, ienc_nop_b()).opcode == IA64_OP_NOP);
    printf("unit_asm: %u encodings round-tripped through the decoder\n", checked);
}
