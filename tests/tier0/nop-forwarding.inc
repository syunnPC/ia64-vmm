/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_native_nop_forwarding(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode hints[] = {IA64_OP_NOP,IA64_OP_HINT_M,IA64_OP_HINT_I,
        IA64_OP_HINT_F,IA64_OP_HINT_X};
    const IA64SlotUnit units[] = {IA64_UNIT_I,IA64_UNIT_M,IA64_UNIT_I,
        IA64_UNIT_F,IA64_UNIT_X};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
            for (unsigned kind = 0; kind < IA64VMM_ARRAY_SIZE(hints); ++kind)
                for (unsigned variant = 0; variant < 8; ++variant) {
                    randomized(v); v->s.psr = variant & 1 ? IA64_PSR_IC : 0;
                    v->s.rse_bof = bof;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                        ((uint64_t)((bof * 7) % 96) << 18);
                    unsigned x = bof & 1 ? 32 : 8, y = bof & 1 ? 127 : 9, z = bof & 1 ? 49 : 10;
                    ia64_gr_write(&v->s, x, random64(), true);
                    ia64_gr_write(&v->s, y, random64(), variant & 1);
                    ia64_gr_write(&v->s, z, random64(), variant & 4);
                    unsigned gap = kind == 4 ? 1 : (variant & 4 ? 7 : 1);
                    Ia64Instruction insns[10] = {
                        {.opcode = variant & 2 ? IA64_OP_MOV_CURRENT_IP : IA64_OP_ADDS,
                         .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = x, .source2 = y, .immediate = -17}},
                    };
                    for (unsigned at = 1; at <= gap; ++at)
                        insns[at] = (Ia64Instruction){.opcode = hints[kind], .unit = units[kind]};
                    insns[gap + 1] = (Ia64Instruction){.opcode = variant & 4 ? IA64_OP_ADD : IA64_OP_ADDS,
                        .unit = IA64_UNIT_I, .operands.integer = {.destination = variant & 1 ? x : z,
                            .source1 = x, .source2 = x, .immediate = 3}};
                    insns[gap + 2] = (Ia64Instruction){.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                        .operands.integer = {.destination = y, .source2 = variant & 1 ? x : z, .immediate = 5}};
                    forward_values_region(v, ref, insns, gap + 3, 0, true);
                }
    }
    /* Qualifiers consume RAX even when false. Updating hints can overwrite
     * the producer's GR through an exact helper; reserved hints can fault. */
    for (unsigned boundary = 0; boundary < 6; ++boundary) {
        randomized(v); v->s.psr = IA64_PSR_IC; v->s.pr = boundary & 1 ? 3 : 1;
        v->s.cfm = 96 | (UINT64_C(96) << 7);
        Ia64Instruction insns[5] = {
            {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
             .operands.integer = {.destination = 32, .source2 = 127, .immediate = 1}},
            {.opcode = IA64_OP_NOP, .unit = IA64_UNIT_I},
            {.opcode = IA64_OP_NOP, .unit = IA64_UNIT_I, .qp = 1},
            {.opcode = IA64_OP_NOP, .unit = IA64_UNIT_I},
            {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
             .operands.integer = {.destination = 127, .source2 = 32, .immediate = 7}},
        };
        if (boundary >= 2 && boundary <= 4) {
            insns[2] = (Ia64Instruction){.opcode = IA64_OP_HINT_M, .unit = IA64_UNIT_M,
                .qp = boundary == 4 ? 1 : 0, .hint_m_reg_increment = boundary == 2,
                .imm_base_update = boundary != 2,
                .operands.system = {.register_index = 32, .source = 127, .immediate = 19}};
        } else if (boundary == 5) { insns[2].qp = 0; insns[2].reserved_field = true; }
        forward_values_region(v, ref, insns, 5, 0, boundary < 2);
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
