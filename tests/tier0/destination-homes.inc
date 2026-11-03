/* SPDX-License-Identifier: GPL-2.0-or-later */
/* An aliasing destination may reuse only the last stacked source's home.
 * Static/GR0 reads preserve that index; a different stacked read replaces it. */
static void test_native_destination_homes(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_ADD,IA64_OP_ANDCM,IA64_OP_SHL,IA64_OP_SHR,
        IA64_OP_SHRP_IMM,IA64_OP_DEP,IA64_OP_ADDS,IA64_OP_SUB_IMM,IA64_OP_AND_IMM,
        IA64_OP_SXT1,IA64_OP_ZXT4,IA64_OP_DEPZ,IA64_OP_EXTRU,IA64_OP_ADDP4,
        IA64_OP_SHLADDP4,IA64_OP_MOVL,IA64_OP_DEPZ_IMM};
    const unsigned regs[] = {0,9,32,49,63,64,96,127};
    const uint64_t values[] = {0,1,31,32,63,64,UINT64_MAX,UINT64_C(1) << 63};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
                for (unsigned variant = 0; variant < 16; ++variant) {
                    randomized(v); v->s.pr &= ~UINT64_C(2);
                    v->s.rse_bof = bof;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                        ((uint64_t)((bof * 5 + variant) % 96) << 18);
                    unsigned a = regs[(bof + variant) % IA64VMM_ARRAY_SIZE(regs)];
                    unsigned b = variant == 7 ? a : regs[(bof / 8 + variant * 3) % IA64VMM_ARRAY_SIZE(regs)];
                    unsigned dest = variant % 4 == 0 ? (a >= 32 ? a : 32) :
                                    variant % 4 == 1 ? (b >= 32 ? b : 127) :
                                    variant % 4 == 2 ? 96 : 0;
                    Ia64Instruction i = {.opcode = ops[op], .valid = true, .unit = IA64_UNIT_I,
                        .address = BASE, .slot = 1, .qp = variant & 8 ? 1 : 0,
                        .operands.integer = {.destination = dest, .source1 = a, .source2 = b,
                            .immediate = 7 | (13 << 6) | (0x55 << 13)}};
                    if (i.opcode == IA64_OP_ADDS || i.opcode == IA64_OP_SUB_IMM) i.operands.integer.immediate = -7;
                    if (i.opcode == IA64_OP_SHLADDP4) i.operands.integer.immediate = 1 + variant % 4;
                    if (i.opcode == IA64_OP_MOVL) i.unit = IA64_UNIT_L;
                    ia64_gr_write(&v->s, a, values[bof % IA64VMM_ARRAY_SIZE(values)], variant & 1);
                    ia64_gr_write(&v->s, b, random64(), variant & 2);
                    native_single(v, ref, &i, dest != 0);
                }
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
