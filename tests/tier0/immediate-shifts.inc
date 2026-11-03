/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_native_immediate_shifts(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_SHL_IMM,IA64_OP_SHR_IMM,IA64_OP_SHRU_IMM};
    const uint64_t values[] = {0,1,UINT64_C(0x7fffffffffffffff),UINT64_C(1) << 63,
        UINT64_MAX,UINT64_C(0xfedcba9876543210)};
    const uint64_t extra[] = {64,128,UINT64_MAX,UINT64_C(1) << 63};
    const unsigned sources[] = {0,9,32,127};
    for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
        for (unsigned count = 0; count < 64 + IA64VMM_ARRAY_SIZE(extra); ++count)
            for (unsigned value = 0; value < IA64VMM_ARRAY_SIZE(values); ++value)
                for (unsigned variant = 0; variant < 16; ++variant) {
                    randomized(v);
                    unsigned source = sources[variant % 4];
                    Ia64Instruction i = {.opcode = ops[op], .valid = true, .unit = IA64_UNIT_I,
                        .address = BASE, .slot = 1, .qp = variant & 8 ? 1 : 0,
                        .operands.integer = {.destination = source && (variant & 4) ? source : 8,
                            .source2 = source, .immediate = (int64_t)(count < 64 ? count : extra[count - 64])}};
                    v->s.pr &= ~UINT64_C(2);
                    ia64_gr_write(&v->s, source, values[value], value & 1);
                    native_single(v, ref, &i, true);
                }
}
