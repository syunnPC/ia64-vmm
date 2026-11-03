/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Forward value and NaT through all six extension forms, including
 * aliasing destinations, every backing-frame origin and qualifier barriers. */
static void test_native_extension_forward(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_SXT1,IA64_OP_SXT2,IA64_OP_SXT4,
        IA64_OP_ZXT1,IA64_OP_ZXT2,IA64_OP_ZXT4};
    const uint64_t values[] = {0,0x7f,0x80,0xff,0x7fff,0x8000,0xffff,
        0x7fffffff,0x80000000,UINT32_MAX,UINT64_MAX,UINT64_C(1) << 63};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
                for (unsigned variant = 0; variant < 16; ++variant) {
                    randomized(v);
                    v->s.pr = (v->s.pr & ~UINT64_C(2)) | (variant == 12 || variant == 13 ? 2 : 0);
                    v->s.psr = variant & 1 ? IA64_PSR_IC : 0;
                    v->s.rse_bof = bof;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                        ((uint64_t)((bof * 7) % 96) << 18);
                    unsigned x = bof & 1 ? 32 : 8, y = bof & 1 ? 127 : 9, z = bof & 1 ? 49 : 10;
                    unsigned destination = variant & 2 ? x : y;
                    ia64_gr_write(&v->s, z, values[variant % IA64VMM_ARRAY_SIZE(values)], variant & 4);
                    Ia64Instruction insns[3] = {
                        {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                         .qp = variant == 13 || variant == 14 ? 1 : 0,
                         .operands.integer = {.destination = x, .source2 = variant == 11 ? 0 : z}},
                        {.opcode = ops[op], .unit = IA64_UNIT_I,
                         .qp = variant == 12 || variant == 15 ? 1 : 0,
                         .operands.integer = {.destination = destination, .source2 = x}},
                        {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = z, .source2 = destination, .immediate = 17}},
                    };
                    forward_values_region(v, ref, insns, 3, variant % 3, true);
                }
        /* A non-native extension must never consume stale retained inputs. */
        for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op) {
            randomized(v); v->s.psr = IA64_PSR_IC;
            v->s.cfm = 96 | (UINT64_C(96) << 7); v->s.rse_bof = 95;
            Ia64Instruction insns[3] = {
                {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                 .operands.integer = {.destination = 32, .source2 = 127, .immediate = 1}},
                {.opcode = ops[op], .unit = IA64_UNIT_I,
                 .operands.integer = {.destination = 0, .source2 = 32}},
                {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                 .operands.integer = {.destination = 127, .source2 = 32, .immediate = 7}},
            };
            forward_values_region(v, ref, insns, 3, 2, false);
        }
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
