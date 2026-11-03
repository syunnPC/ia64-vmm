/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Both value and NaT must use the same rotated physical home. Cover every
 * backing-frame origin with aliases of either source and independent NaTs. */
static void test_native_stacked_operands(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_ADD,IA64_OP_ANDCM,IA64_OP_SHL,IA64_OP_SHRP_IMM,
        IA64_OP_DEP,IA64_OP_ADDS,IA64_OP_SXT4,IA64_OP_DEPZ,IA64_OP_MOVL};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
                for (unsigned variant = 0; variant < 8; ++variant) {
                    randomized(v);
                    v->s.rse_bof = bof;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                        ((uint64_t)((bof * 7) % 96) << 18);
                    Ia64Instruction i = {.opcode = ops[op], .valid = true, .unit = IA64_UNIT_I,
                        .address = BASE, .slot = 1, .qp = variant == 7 ? 1 : 0,
                        .operands.integer = {.destination = variant & 1 ? 127 : 32,
                            .source1 = variant == 4 ? 0 : 32,
                            .source2 = variant == 5 ? 32 : variant == 6 ? 0 : 127,
                            .immediate = 31}};
                    if (i.opcode == IA64_OP_DEP || i.opcode == IA64_OP_DEPZ)
                        i.operands.integer.immediate = 5 | (16 << 6);
                    if (i.opcode == IA64_OP_ADDS) i.operands.integer.immediate = -7;
                    if (i.opcode == IA64_OP_MOVL) i.operands.integer.immediate = INT64_MIN;
                    v->s.pr &= ~UINT64_C(2);
                    ia64_gr_write(&v->s, 32, variant == 3 ? 64 : random64() & 63, variant & 1);
                    ia64_gr_write(&v->s, 127, random64(), variant & 2);
                    native_single(v, ref, &i, true);
                }
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
