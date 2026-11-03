/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Duplicate value/NaT reads may be shared only before an intervening write.
 * Exercise both host-ISA paths, GR0, rotated homes, destination aliases,
 * both NaT states, false qualifiers and full-width variable shift counts. */
static void test_native_same_source(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_ADD,IA64_OP_ADD_ONE,IA64_OP_SUB,IA64_OP_SUB_ONE,
        IA64_OP_AND,IA64_OP_ANDCM,IA64_OP_OR,IA64_OP_XOR,IA64_OP_SHLADD,
        IA64_OP_ADDP4,IA64_OP_SHLADDP4,IA64_OP_SHL,IA64_OP_SHR,IA64_OP_SHRU,
        IA64_OP_DEP,IA64_OP_SHRP_IMM};
    const uint64_t values[] = {0,1,31,32,63,64,65,UINT32_MAX,UINT64_C(1) << 32,
        UINT64_C(1) << 63,UINT64_MAX,UINT64_C(0xfedcba9876543210)};
    const unsigned sources[] = {0,10,32,49,127};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
            for (unsigned source = 0; source < IA64VMM_ARRAY_SIZE(sources); ++source)
                for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(values); ++n)
                    for (unsigned variant = 0; variant < 8; ++variant) {
                        randomized(v);
                        unsigned reg = sources[source];
                        Ia64Instruction i = {.opcode = ops[op], .valid = true, .unit = IA64_UNIT_I,
                            .address = BASE, .slot = 1, .qp = variant & 4 ? 1 : 0,
                            .operands.integer = {.destination = reg && (variant & 2) ? reg : 8,
                                .source1 = reg, .source2 = reg, .immediate = n % 4 + 1}};
                        if (i.opcode == IA64_OP_DEP)
                            i.operands.integer.immediate = (n * 7 % 64) | ((n * 11 % 65) << 6);
                        if (i.opcode == IA64_OP_SHRP_IMM) i.operands.integer.immediate = n * 7;
                        v->s.pr &= ~UINT64_C(2);
                        ia64_gr_write(&v->s, reg, values[n], variant & 1);
                        native_single(v, ref, &i, true);
                    }
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
