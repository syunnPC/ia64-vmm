/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_native_extensions(struct vcpu *v, struct vcpu *ref)
{
    uint8_t bytes[64];
    struct x86_buffer b = {.data = bytes, .capacity = sizeof(bytes)};
    x86_load_signed(&b, X_RAX, X_R12, 0x100, 1);
    x86_load_signed(&b, X_R9, X_RBP, 0, 2);
    x86_load_signed(&b, X_RAX, X_RDI, -8, 4);
    x86_load_index_signed(&b, X_RAX, X_R12, X_R11, 3, 0x100, 1);
    x86_load_index_signed(&b, X_R10, X_R13, X_R8, 2, 0, 2);
    x86_load_index_signed(&b, X_RAX, X_RDI, X_RSI, 1, -8, 4);
    const uint8_t expected[] = {
        0x49,0x0f,0xbe,0x84,0x24,0,1,0,0, 0x4c,0x0f,0xbf,0x4d,0,
        0x48,0x63,0x47,0xf8, 0x4b,0x0f,0xbe,0x84,0xdc,0,1,0,0,
        0x4f,0x0f,0xbf,0x54,0x85,0, 0x48,0x63,0x44,0x77,0xf8,
    };
    encoded(expected, sizeof(expected), &b);
    const Ia64Opcode ops[] = {IA64_OP_SXT1,IA64_OP_SXT2,IA64_OP_SXT4,
        IA64_OP_ZXT1,IA64_OP_ZXT2,IA64_OP_ZXT4};
    const uint64_t values[] = {0,1,0x7f,0x80,0xff,0x7fff,0x8000,0xffff,
        0x7fffffff,0x80000000,UINT32_MAX,UINT64_C(0xfedcba9876543210),UINT64_MAX};
    const unsigned sources[] = {0,10,49,127};
    for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
        for (unsigned value = 0; value < IA64VMM_ARRAY_SIZE(values); ++value)
            for (unsigned variant = 0; variant < 16; ++variant) {
                randomized(v);
                unsigned source = sources[variant % 4];
                Ia64Instruction i = {.opcode = ops[op], .valid = true, .unit = IA64_UNIT_I,
                    .address = BASE, .slot = 1, .qp = variant & 8 ? 1 : 0,
                    .operands.integer = {.destination = (variant & 4) && source ? source : 8,
                        .source2 = source}};
                v->s.pr &= ~UINT64_C(2);
                ia64_gr_write(&v->s, source, values[value], value & 1);
                native_single(v, ref, &i, true);
            }
}
