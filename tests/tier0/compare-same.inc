/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_native_compare_same(struct vcpu *v, struct vcpu *ref)
{
    enum relation { REL_EQ,REL_NE,REL_LT,REL_LE,REL_GT,REL_GE,REL_LTU,REL_LEU,REL_GTU,REL_GEU };
    struct compare_desc { bool valid, word, immediate; enum relation relation; Ia64PredicateUpdate update; };
    static const struct compare_desc compares[IA64_OP_COUNT] = {
#include "ia64/decode/compare.inc"
    };
    const unsigned sources[] = {0,8,32,127};
    const uint64_t values[] = {0,UINT64_MAX,UINT64_C(1) << 31,UINT64_C(1) << 32,
        UINT64_C(1) << 63,UINT64_C(0xfedcba9876543210)};
    for (unsigned opcode = 0; opcode < IA64_OP_COUNT; ++opcode) {
        if (!compares[opcode].valid || compares[opcode].immediate) continue;
        for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
            for (unsigned variant = 0; variant < 16; ++variant) {
                randomized(v); v->s.rse_bof = bof;
                v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                    ((uint64_t)((bof * 7) % 96) << 18) | ((uint64_t)(bof % 48) << 32);
                v->s.psr = variant & 1 ? IA64_PSR_IC : 0;
                unsigned source = sources[variant % 4], producer = source ? source : 8;
                unsigned p1 = bof % 64, p2 = (bof + 1) % 64;
                unsigned qp = variant & 8 ? (bof & 4 ? p1 : p2) : 0;
                if (qp) ia64_pr_write(&v->s, qp, !(bof & 1));
                ia64_gr_write(&v->s, 49, values[bof % IA64VMM_ARRAY_SIZE(values)], variant & 4);
                Ia64Instruction insns[3] = {
                    {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                     .operands.integer = {.destination = producer, .source2 = 49, .immediate = 1}},
                    {.opcode = opcode, .unit = IA64_UNIT_I, .qp = qp, .compare_unc = (bof & 2) != 0,
                     .operands.integer = {.source1 = source, .source2 = source,
                         .predicate1 = p1, .predicate2 = p2}},
                    {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I, .qp = p1,
                     .operands.integer = {.destination = 49, .source2 = producer, .immediate = 7}},
                };
                bool helper = source && (variant & 4) && ia64_pr_read(&v->s, qp);
                uint64_t before = native_helper_calls;
                forward_values_region(v, ref, insns, 3, variant % 3, !helper);
                if (helper) CHECK(native_helper_calls > before);
            }
        /* Equal predicate targets remain ineligible and use exact helpers,
         * including p0 and a qualifier aliasing that same target. */
        for (unsigned variant = 0; variant < 8; ++variant) {
            randomized(v); v->s.cfm = 96 | (UINT64_C(96) << 7); v->s.rse_bof = 95;
            v->s.slot = variant % 3;
            unsigned p = variant & 2 ? 0 : 17;
            Ia64Instruction i = {.opcode = opcode, .valid = true, .unit = IA64_UNIT_I,
                .address = BASE, .slot = variant % 3, .qp = variant & 4 ? p : 0,
                .compare_unc = (variant & 1) != 0,
                .operands.integer = {.source1 = 127, .source2 = 127, .predicate1 = p, .predicate2 = p}};
            ia64_gr_write(&v->s, 127, random64(), variant & 1);
            if (i.qp) ia64_pr_write(&v->s, i.qp, !(variant & 1));
            uint64_t before = native_helper_calls;
            native_single(v, ref, &i, false); CHECK(native_helper_calls > before);
        }
    }
}
