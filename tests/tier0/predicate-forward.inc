/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_native_predicate_forward(struct vcpu *v, struct vcpu *ref)
{
    enum relation { REL_EQ,REL_NE,REL_LT,REL_LE,REL_GT,REL_GE,REL_LTU,REL_LEU,REL_GTU,REL_GEU };
    struct compare_desc { bool valid, word, immediate; enum relation relation; Ia64PredicateUpdate update; };
    static const struct compare_desc compares[IA64_OP_COUNT] = {
#include "ia64/decode/compare.inc"
    };
    const uint64_t values[] = {0,UINT64_MAX,UINT64_C(1) << 31,UINT64_C(1) << 32,
        UINT64_C(1) << 63,UINT64_C(0xfedcba9876543210)};
    const Ia64Opcode producers[] = {IA64_OP_ADDS,IA64_OP_SXT4,IA64_OP_MOV_CURRENT_IP};
    for (unsigned opcode = 0; opcode < IA64_OP_COUNT; ++opcode) {
        bool tbit = opcode == IA64_OP_TBIT_Z || opcode == IA64_OP_TBIT_NZ;
        bool tnat = opcode == IA64_OP_TNAT_Z || opcode == IA64_OP_TNAT_NZ;
        if (!compares[opcode].valid && !tbit && !tnat) continue;
        for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
            for (unsigned variant = 0; variant < 16; ++variant) {
                randomized(v); v->s.rse_bof = bof;
                v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                    ((uint64_t)((bof * 7) % 96) << 18) | ((uint64_t)(bof % 48) << 32);
                v->s.psr = variant & 1 ? IA64_PSR_IC : 0;
                unsigned x = bof & 1 ? 32 : 8, y = bof & 1 ? 127 : 9;
                unsigned p1 = bof % 64, p2 = (bof + 1) % 64;
                unsigned qp = variant >= 12 ? (variant & 1 ? p1 : p2) : 0;
                unsigned producer_qp = variant == 10 || variant == 11 ? 1 : 0;
                if (producer_qp) ia64_pr_write(&v->s, producer_qp, variant & 1);
                if (qp) ia64_pr_write(&v->s, qp, variant & 1);
                ia64_gr_write(&v->s, x, values[(bof + 1) % 6], variant & 2);
                ia64_gr_write(&v->s, y, values[(bof + 2) % 6], variant & 4);
                ia64_gr_write(&v->s, 49, values[bof % 6], variant & 2);
                unsigned first = variant % 3 == 1 ? y : x;
                unsigned second = variant % 3 == 0 ? y : x;
                if (!compares[opcode].valid || compares[opcode].immediate) second = x;
                if (variant == 9) first = 0;
                Ia64Instruction insns[3] = {
                    {.opcode = producers[bof % 3], .unit = IA64_UNIT_I, .qp = producer_qp,
                     .operands.integer = {.destination = x, .source2 = 49, .immediate = 1}},
                    {.opcode = opcode, .unit = IA64_UNIT_I, .qp = qp, .compare_unc = (bof & 2) != 0,
                     .operands.integer = {.source1 = first, .source2 = second,
                         .predicate1 = p1, .predicate2 = p2,
                         .immediate = tbit || tnat ? bof : (int64_t)values[bof % 6]}},
                    {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I, .qp = p1,
                     .operands.integer = {.destination = 49, .source2 = x, .immediate = 7}},
                };
                /* In these variants every source NaT is clear; the whole
                 * sequence must remain native in addition to matching state. */
                bool all_clear = !(variant & 6);
                forward_values_region(v, ref, insns, 3, variant % 3, all_clear);
            }
    }
    /* A helper, frame write, not-taken branch, reserved instruction, or
     * successful memory fallback must prevent a stale retained condition. */
    for (unsigned boundary = 0; boundary < 5; ++boundary) {
        randomized(v); v->s.pr &= ~UINT64_C(2); v->s.psr = IA64_PSR_IC;
        v->s.cfm = 96 | (UINT64_C(96) << 7); v->s.rse_dirty = 0;
        ia64_gr_write(&v->s, 127, 15, false);
        Ia64Instruction insns[3] = {
            {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
             .operands.integer = {.destination = 32, .source2 = 127, .immediate = 1}},
            {.opcode = IA64_OP_POPCNT, .unit = IA64_UNIT_I,
             .operands.integer = {.destination = 32, .source2 = 32}},
            {.opcode = IA64_OP_CMP_EQ_IMM, .unit = IA64_UNIT_I,
             .operands.integer = {.source2 = 32, .immediate = 1, .predicate1 = 2, .predicate2 = 3}},
        };
        if (boundary == 1) {
            insns[1].opcode = IA64_OP_ALLOC;
            insns[1].operands.system = (IA64SystemOperands){.destination = 32, .immediate = 96 | 96 << 7};
        } else if (boundary == 2) {
            insns[1].opcode = IA64_OP_BR_COND; insns[1].unit = IA64_UNIT_B; insns[1].qp = 1;
        } else if (boundary == 3) {
            insns[1].opcode = IA64_OP_ADDS; insns[1].reserved_field = true;
        } else if (boundary == 4) {
            insns[1].opcode = IA64_OP_LD8; insns[1].unit = IA64_UNIT_M;
            insns[1].operands.memory = (IA64MemoryOperands){.destination = 32, .base = 10};
            ia64_gr_write(&v->s, 10, DEVICE + 8, false);
            insns[2].operands.integer.immediate = (int64_t)UINT64_C(0xfedcba9876543210);
        }
        forward_values_region(v, ref, insns, 3, boundary == 2 ? 1 : 0, boundary == 1 || boundary == 2);
    }
}
