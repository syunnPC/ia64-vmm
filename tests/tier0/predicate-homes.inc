/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_native_predicate_homes(struct vcpu *v, struct vcpu *ref)
{
    enum relation { REL_EQ,REL_NE,REL_LT,REL_LE,REL_GT,REL_GE,REL_LTU,REL_LEU,REL_GTU,REL_GEU };
    struct compare_desc { bool valid, word, immediate; enum relation relation; Ia64PredicateUpdate update; };
    static const struct compare_desc compares[IA64_OP_COUNT] = {
#include "ia64/decode/compare.inc"
    };
    const unsigned sources[] = {0,8,32,49,127};
    const uint64_t values[] = {0,UINT64_MAX,UINT64_C(1) << 31,UINT64_C(1) << 32,
        UINT64_C(1) << 63,UINT64_C(0xfedcba9876543210)};
    for (unsigned opcode = 0; opcode < IA64_OP_COUNT; ++opcode) {
        bool tbit = opcode == IA64_OP_TBIT_Z || opcode == IA64_OP_TBIT_NZ;
        bool tnat = opcode == IA64_OP_TNAT_Z || opcode == IA64_OP_TNAT_NZ;
        if (!compares[opcode].valid && !tbit && !tnat) continue;
        /* Every backing-frame origin, with rotating inputs, independent
         * NaTs, all comparison types and qp aliasing either destination. */
        for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
            for (unsigned variant = 0; variant < 8; ++variant) {
                randomized(v);
                v->s.rse_bof = bof;
                v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                    ((uint64_t)((bof * 7) % 96) << 18) | ((uint64_t)(bof % 48) << 32);
                unsigned a = sources[bof % IA64VMM_ARRAY_SIZE(sources)];
                unsigned b = sources[(bof / IA64VMM_ARRAY_SIZE(sources) + 1) % IA64VMM_ARRAY_SIZE(sources)];
                unsigned p1 = bof % 64, p2 = (bof + 1) % 64;
                Ia64Instruction i = {.opcode = opcode, .valid = true, .unit = IA64_UNIT_I,
                    .address = BASE, .slot = 1, .qp = variant & 4 ? p2 : p1,
                    .compare_unc = (variant & 4) != 0,
                    .operands.integer = {.source1 = a, .source2 = b,
                        .predicate1 = p1, .predicate2 = p2, .immediate = (int64_t)values[bof % 6]}};
                if (tbit || tnat) i.operands.integer.immediate = bof;
                ia64_gr_write(&v->s, a, values[bof % 6], variant & 1);
                ia64_gr_write(&v->s, b, values[(bof + 1) % 6], variant & 2);
                if (i.qp) ia64_pr_write(&v->s, i.qp, !(variant & 4));
                bool nat = !tnat && (ia64_nat_read(&v->s, b) ||
                    (compares[opcode].valid && !compares[opcode].immediate && ia64_nat_read(&v->s, a)));
                bool helper = ia64_pr_read(&v->s, i.qp) && nat;
                uint64_t before = native_helper_calls;
                native_single(v, ref, &i, !helper);
                if (helper) CHECK(native_helper_calls > before);
            }
    }
}
