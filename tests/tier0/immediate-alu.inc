/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sign-extension boundaries exercise both native imm8/imm32 encodings
 * and full-width diagnostic operands. Aliased and rotated homes retain
 * the original NaT and false-predicate behavior. */
static void test_native_immediate_alu(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_ADDS,IA64_OP_ADDL,IA64_OP_AND_IMM,
        IA64_OP_OR_IMM,IA64_OP_XOR_IMM};
    const int64_t immediates[] = {INT64_MIN,INT64_C(-2147483649),INT32_MIN,
        -129,-128,-1,0,1,127,128,INT32_MAX,INT64_C(2147483648),UINT32_MAX,INT64_MAX};
    const uint64_t values[] = {0,1,UINT64_C(0x7fffffffffffffff),
        UINT64_C(0x8000000000000000),UINT64_MAX};
    const unsigned sources[] = {0,10,49,127};
    for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
        for (unsigned imm = 0; imm < IA64VMM_ARRAY_SIZE(immediates); ++imm)
            for (unsigned variant = 0; variant < 16; ++variant) {
                randomized(v);
                unsigned source = sources[variant % 4];
                Ia64Instruction i = {.opcode = ops[op], .valid = true, .unit = IA64_UNIT_I,
                    .address = BASE, .slot = 1, .qp = variant & 8 ? 1 : 0,
                    .operands.integer = {.destination = (variant & 4) && source ? source : 8,
                        .source2 = source, .immediate = immediates[imm]}};
                v->s.pr &= ~UINT64_C(2);
                ia64_gr_write(&v->s, source, values[imm % IA64VMM_ARRAY_SIZE(values)], imm & 1);
                native_single(v, ref, &i, true);
            }
}
