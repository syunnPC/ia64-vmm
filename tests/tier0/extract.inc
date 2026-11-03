/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Every encoded position and length, including fields truncated at bit 63,
 * under the baseline host ISA and available BMI1. Keep register rotation,
 * source/destination aliasing, NaTs and false qualifiers in the comparison. */
static void test_native_extract(struct vcpu *v, struct vcpu *ref)
{
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned sign = 0; sign < 2; ++sign)
            for (unsigned pos = 0; pos < 64; ++pos)
                for (unsigned len = 1; len <= 64; ++len)
                    for (unsigned variant = 0; variant < 4; ++variant) {
                        randomized(v);
                        unsigned source = variant == 2 ? 0 : variant ? 49 : 10;
                        Ia64Instruction i = {.opcode = sign ? IA64_OP_EXTR : IA64_OP_EXTRU,
                            .valid = true, .unit = IA64_UNIT_I, .address = BASE, .slot = 1,
                            .qp = variant == 3 ? 1 : 0,
                            .operands.integer = {.destination = variant == 1 ? 49 : 8,
                                .source2 = source, .immediate = pos | (len << 6)}};
                        v->s.pr &= ~UINT64_C(2);
                        unsigned high = pos + len > 64 ? 63 : pos + len - 1;
                        uint64_t value = variant ? UINT64_C(1) << high : random64();
                        ia64_gr_write(&v->s, source, value, (pos + len) & 1);
                        native_single(v, ref, &i, true);
                    }
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
