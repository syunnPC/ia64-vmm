/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Same-source constants and identities still carry the source NaT. Cover
 * both a retained producer and a consumer of the simplified result, with
 * every budget prefix and a requested exit at the first checkpoint. */
static void test_native_same_value_folds(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_AND, IA64_OP_OR, IA64_OP_SUB,
        IA64_OP_SUB_ONE, IA64_OP_XOR, IA64_OP_ANDCM};
    const unsigned sources[] = {0, 8, 32, 127};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
                for (unsigned variant = 0; variant < 16; ++variant) {
                    randomized(v); v->s.pr &= ~UINT64_C(2);
                    v->s.rse_bof = bof; v->s.psr = variant & 1 ? IA64_PSR_IC : 0;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                        ((uint64_t)((bof * 7) % 96) << 18);
                    unsigned source = sources[(variant + bof) % IA64VMM_ARRAY_SIZE(sources)];
                    unsigned dest = source && (variant & 4) ? source : source < 32 ? 9 : 48;
                    ia64_gr_write(&v->s, source, variant & 4 ? UINT64_MAX : random64(), variant & 2);
                    ia64_gr_write(&v->s, 10, random64(), variant & 4);
                    Ia64Instruction insns[3] = {
                        {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I, .qp = variant & 2 ? 1 : 0,
                         .operands.integer = {.destination = source ? source : 11,
                             .source2 = 10, .immediate = -17}},
                        {.opcode = ops[op], .unit = IA64_UNIT_I, .qp = variant & 8 ? 1 : 0,
                         .operands.integer = {.destination = dest, .source1 = source, .source2 = source}},
                        {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = 49, .source2 = dest, .immediate = 7}},
                    };
                    forward_values_region(v, ref, insns, 3, bof % 3, true);
                }
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}

/* x & x and x | x copy the value and NaT, just like adds r=x,0.
 * They need the same code for static/stacked homes and retained values. */
static void test_native_same_value_identities(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_ADDS, IA64_OP_AND, IA64_OP_OR};
    const unsigned sources[] = {9, 49};
    for (unsigned source = 0; source < IA64VMM_ARRAY_SIZE(sources); ++source)
        for (unsigned alias = 0; alias < 2; ++alias)
            for (unsigned forwarded = 0; forwarded < 2; ++forwarded) {
                size_t bytes[IA64VMM_ARRAY_SIZE(ops)];
                for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op) {
                    randomized(v);
                    unsigned reg = sources[source], dest = alias ? reg : reg + 1;
                    uint64_t value = UINT64_C(0xa55ac33c96696996);
                    ia64_gr_write(&v->s, reg, value, true);
                    Ia64Instruction insns[3] = {
                        {.opcode = IA64_OP_ADDS, .valid = true, .unit = IA64_UNIT_I,
                         .address = BASE, .slot = 0,
                         .operands.integer = {.destination = reg, .source2 = reg}},
                        {.opcode = ops[op], .valid = true, .unit = IA64_UNIT_I,
                         .address = BASE, .slot = 1,
                         .operands.integer = {.destination = dest, .source1 = reg, .source2 = reg}},
                        {.opcode = IA64_OP_ADDS, .valid = true, .unit = IA64_UNIT_I,
                         .address = BASE, .slot = 2,
                         .operands.integer = {.destination = 11, .source2 = dest}},
                    };
                    struct tier0_region r = {.insns = insns + !forwarded, .count = forwarded ? 3 : 1};
                    memset(r.slots, 1, sizeof(r.slots));
                    struct x86_buffer b = {.capacity = SIZE_MAX};
                    tier0_emit(&b, &r, &v->s);
                    CHECK(!b.failed); bytes[op] = b.size;
                    native_single(v, ref, &insns[1], true);
                    CHECK(ia64_gr_read(&v->s, dest) == value && ia64_nat_read(&v->s, dest));
                }
                if (bytes[0] != bytes[1] || bytes[0] != bytes[2])
                    LOGE("same-value identity bytes: adds=%zu and=%zu or=%zu source=%u alias=%u forwarded=%u",
                        bytes[0], bytes[1], bytes[2], sources[source], alias, forwarded);
                CHECK(bytes[0] == bytes[1] && bytes[0] == bytes[2]);
            }
}
