/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Identity operations still transfer source NaT, write their destination,
 * and retire at every prefix, including when they consume/produce the
 * retained RAX/R8 pair. Nearby non-identities must not be elided. */
static void test_native_neutral_immediates(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_ADDS,IA64_OP_ADDL,IA64_OP_AND_IMM,
        IA64_OP_OR_IMM,IA64_OP_XOR_IMM};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
                for (unsigned variant = 0; variant < 8; ++variant) {
                    randomized(v); v->s.psr = variant & 1 ? IA64_PSR_IC : 0;
                    v->s.pr = variant & 2 ? 3 : 1;
                    v->s.rse_bof = bof;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                        ((uint64_t)((bof * 7) % 96) << 18);
                    unsigned x = bof & 1 ? 32 : 8, y = bof & 1 ? 127 : 9, z = bof & 1 ? 49 : 10;
                    ia64_gr_write(&v->s, y, random64(), variant & 1);
                    unsigned destination = variant & 4 ? x : z;
                    int64_t identity = ops[op] == IA64_OP_AND_IMM ? -1 : 0;
                    Ia64Instruction insns[4] = {
                        {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = x, .source2 = y, .immediate = -19}},
                        {.opcode = ops[op], .unit = IA64_UNIT_I, .qp = variant >= 6 ? 1 : 0,
                         .operands.integer = {.destination = destination, .source2 = x, .immediate = identity}},
                        {.opcode = IA64_OP_ADD, .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = y, .source1 = destination, .source2 = destination}},
                        {.opcode = ops[op], .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = z, .source2 = y,
                             .immediate = identity ? -2 : 1}},
                    };
                    forward_values_region(v, ref, insns, 4, 0, true);
                }
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
