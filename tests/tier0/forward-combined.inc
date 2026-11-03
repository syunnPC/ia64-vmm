/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Forward RAX/R8 through an immediate shift between same-source
 * producer/consumer instructions, crossing a bundle boundary. */
static void test_native_forward_combined(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode shifts[] = {IA64_OP_SHL_IMM, IA64_OP_SHR_IMM, IA64_OP_SHRU_IMM};
    const unsigned counts[] = {0, 1, 31, 63, 64, 127};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(shifts); ++op)
            for (unsigned count = 0; count < IA64VMM_ARRAY_SIZE(counts); ++count)
                for (unsigned stacked = 0; stacked < 2; ++stacked)
                    for (unsigned nat = 0; nat < 2; ++nat)
                        for (unsigned alias = 0; alias < 2; ++alias) {
                            randomized(v);
                            v->s.psr = IA64_PSR_IC;
                            v->s.rse_bof = 95;
                            v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                                (UINT64_C(95) << 18);
                            unsigned x = stacked ? 32 : 8, y = stacked ? 127 : 9, z = stacked ? 49 : 10;
                            unsigned shifted = alias ? x : y;
                            ia64_gr_write(&v->s, z, UINT64_C(0xf123456789abcdef), nat != 0);
                            Ia64Instruction insns[3] = {
                                {.opcode = IA64_OP_OR, .unit = IA64_UNIT_I,
                                 .operands.integer = {.destination = x, .source1 = z, .source2 = z}},
                                {.opcode = shifts[op], .unit = IA64_UNIT_I,
                                 .operands.integer = {.destination = shifted, .source2 = x, .immediate = counts[count]}},
                                {.opcode = IA64_OP_ADD, .unit = IA64_UNIT_I,
                                 .operands.integer = {.destination = z, .source1 = shifted, .source2 = shifted}},
                            };
                            forward_values_region(v, ref, insns, 3, 2, true);
                        }
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
