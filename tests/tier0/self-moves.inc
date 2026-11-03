/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_native_self_moves(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_ADDS,IA64_OP_ADDL,IA64_OP_AND_IMM,IA64_OP_OR_IMM,
        IA64_OP_XOR_IMM,IA64_OP_SHL_IMM,IA64_OP_SHR_IMM,IA64_OP_SHRU_IMM};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
                for (unsigned variant = 0; variant < 8; ++variant) {
                    randomized(v); v->s.psr = variant & 1 ? IA64_PSR_IC : 0;
                    v->s.pr = variant & 2 ? 3 : 1; v->s.rse_bof = bof;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                        ((uint64_t)((bof * 7) % 96) << 18);
                    unsigned x = bof & 1 ? 32 : 8, y = bof & 1 ? 127 : 9;
                    ia64_gr_write(&v->s, x, random64(), variant & 1);
                    ia64_gr_write(&v->s, y, random64(), variant & 2);
                    int64_t imm = ops[op] == IA64_OP_AND_IMM ? -1 : op >= 5 && variant & 4 ? 64 : 0;
                    Ia64Instruction insns[5] = {
                        {.opcode = variant & 4 ? IA64_OP_NOP : IA64_OP_ADDS, .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = x, .source2 = y, .immediate = 7}},
                        {.opcode = ops[op], .unit = IA64_UNIT_I, .qp = variant >= 6 ? 1 : 0,
                         .operands.integer = {.destination = x, .source2 = x, .immediate = imm}},
                        {.opcode = ops[op], .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = x, .source2 = x, .immediate = imm}},
                        {.opcode = IA64_OP_ADD, .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = y, .source1 = x, .source2 = x}},
                        {.opcode = ops[op], .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = y, .source2 = y, .immediate = imm}},
                    };
                    forward_values_region(v, ref, insns, 5, variant % 3, true);
                }
    }
    /* r0 and out-of-frame writes keep their precise helper semantics. */
    for (unsigned reg = 0; reg < 2; ++reg) {
        randomized(v); v->s.cfm = 0;
        Ia64Instruction insns[3] = {
            {.opcode = IA64_OP_NOP, .unit = IA64_UNIT_M},
            {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
             .operands.integer = {.destination = reg ? 32 : 0, .source2 = reg ? 32 : 0}},
            {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
             .operands.integer = {.destination = 8, .source2 = 8, .immediate = 1}},
        };
        forward_values_region(v, ref, insns, 3, 0, false);
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
