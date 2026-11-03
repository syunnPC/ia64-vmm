/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_native_system_homes(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t pfs_reserved = (UINT64_C(15) << 58) | (UINT64_C(0x3fff) << 38);
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
            for (unsigned variant = 0; variant < 8; ++variant) {
                randomized(v); v->s.psr = IA64_PSR_IC; v->s.pr = variant & 2 ? 3 : 1;
                v->s.rse_bof = bof;
                v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                    ((uint64_t)((bof * 7) % 96) << 18);
                unsigned x = bof & 1 ? 32 : 127, y = 49;
                uint64_t value = random64() & ~pfs_reserved;
                if (variant & 4) value |= pfs_reserved;
                ia64_gr_write(&v->s, y, value, variant & 1);
                Ia64Instruction insns[3] = {
                    {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                     .operands.integer = {.destination = x, .source2 = y}},
                    {.opcode = kind < 2 ? IA64_OP_MOV_GRAR : kind == 2 ? IA64_OP_MOV_GRBR : IA64_OP_MOV_GRPR,
                     .unit = IA64_UNIT_I, .qp = variant >= 6 ? 1 : 0,
                     .operands.system = {.destination = x, .source = kind == 0 ? AR_LC : kind == 1 ? AR_PFS : 7,
                         .immediate = -2}},
                    {.opcode = IA64_OP_ADD, .unit = IA64_UNIT_I,
                     .operands.integer = {.destination = y, .source1 = x, .source2 = x}},
                };
                bool taken = !insns[1].qp || (v->s.pr & 2);
                bool fault = taken && ((variant & 1) || (kind == 1 && (value & pfs_reserved)));
                forward_values_region(v, ref, insns, 3, 0, !fault);
            }
}
