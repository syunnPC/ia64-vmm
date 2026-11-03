/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_native_forwarded_home(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode producers[] = {IA64_OP_ADDS,IA64_OP_SUB,IA64_OP_SXT4,
        IA64_OP_MOVL,IA64_OP_DEPZ_IMM,IA64_OP_MOV_CURRENT_IP};
    const Ia64Opcode consumers[] = {IA64_OP_ADDS,IA64_OP_AND_IMM,IA64_OP_OR_IMM,
        IA64_OP_XOR_IMM,IA64_OP_SHL_IMM,IA64_OP_SHR_IMM,IA64_OP_SHRU_IMM,
        IA64_OP_EXTR,IA64_OP_EXTRU,IA64_OP_DEPZ,IA64_OP_ADD,IA64_OP_SUB,
        IA64_OP_ANDCM,IA64_OP_SHL,IA64_OP_SHRP_IMM,IA64_OP_DEP,IA64_OP_SXT2};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned producer = 0; producer < IA64VMM_ARRAY_SIZE(producers); ++producer)
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
                for (unsigned variant = 0; variant < 8; ++variant) {
                    randomized(v); v->s.rse_bof = bof;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                        ((uint64_t)((bof * 7) % 96) << 18);
                    v->s.psr = variant & 1 ? IA64_PSR_IC : 0;
                    ia64_pr_write(&v->s, 1, variant & 1);
                    unsigned x = bof & 1 ? 32 : 127;
                    /* Static/GR0 inputs preserve R11; another stacked
                     * input must replace the retained destination index. */
                    unsigned other = variant % 3 == 0 ? 9 : variant % 3 == 1 ? 0 : 49;
                    ia64_gr_write(&v->s, x, random64(), variant & 2);
                    ia64_gr_write(&v->s, 49, random64(), variant & 4);
                    ia64_gr_write(&v->s, 9, variant == 3 ? 64 : random64() & 63, variant & 4);
                    Ia64Opcode op = consumers[(producer + bof) % IA64VMM_ARRAY_SIZE(consumers)];
                    enum ia64_nat_rule rule = ia64_opcode_info(op)->nat_rule;
                    unsigned first = variant & 1 ? other : x, second = variant & 1 ? x : other;
                    if (rule == IA64_NAT_SOURCE_R2) first = x;
                    if (rule == IA64_NAT_SOURCE_R3) second = x;
                    if (variant == 4) first = second = x;
                    int64_t imm = op == IA64_OP_EXTR || op == IA64_OP_EXTRU ||
                        op == IA64_OP_DEP || op == IA64_OP_DEPZ ? 7 | (13 << 6) : 1;
                    Ia64Instruction insns[3] = {
                        {.opcode = producers[producer], .unit = IA64_UNIT_I,
                         .qp = variant == 6 ? 1 : 0,
                         .operands.integer = {.destination = x, .source1 = 9,
                             .source2 = 49, .immediate = 7 | (13 << 6) | (0x55 << 13)}},
                        {.opcode = op, .unit = IA64_UNIT_I, .qp = variant == 7 ? 1 : 0,
                         .operands.integer = {.destination = variant == 5 ? 49 : x,
                             .source1 = first, .source2 = second, .immediate = imm}},
                        {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = x, .source2 = x, .immediate = 3}},
                    };
                    bool long_move = producers[producer] == IA64_OP_MOVL;
                    if (long_move) insns[0].unit = IA64_UNIT_L;
                    forward_values_region(v, ref, insns, 3, long_move ? 1 : variant % 3, true);
                }
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
