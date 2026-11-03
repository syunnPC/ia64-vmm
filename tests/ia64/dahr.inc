/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Data Access Hint Registers are not part of SDM Rev.2.3, and neither
 * Madison nor Montecito implements them. There, the encoding binutils
 * assembles for mov dahr3=imm16 (M: x3=0, x4=1, x2=0, y=1, bits 11:10 = 1)
 * is hint.m, which executes as a nop (Vol.3 Table 4-46, hint); opcode 1
 * x3=0 x6=0x20 (mov r1=dahr[r3]) is Reserved if PR[qp] is 1 (Table 4-45);
 * and x4=2 x2=1 is invala.e r1 whatever the ignored bits 26:13 hold
 * (Table 4-43, format M26). */
static void test_dahr(void)
{
    const uint64_t to_dahr = UINT64_C(0xd800540);       /* gas: mov dahr3=5 */
    const uint64_t from_dahr = UINT64_C(0x2100900200);  /* gas: mov r8=dahr[r9] */
    const uint64_t invala_e = UINT64_C(0x97ffe540);     /* M26 with bits 26:13 set */
    const uint64_t nop_i = UINT64_C(1) << 27;
    CHECK(ia64_decode_insn(IA64_UNIT_M, to_dahr, ENTRY, 0).opcode == IA64_OP_HINT_M);
    CHECK(ia64_decode_insn(IA64_UNIT_M, invala_e, ENTRY, 1).opcode == IA64_OP_INVALAT);
    Ia64Instruction reserved = ia64_decode_insn(IA64_UNIT_M, from_dahr, ENTRY, 0);
    CHECK(reserved.opcode == IA64_OP_ILLEGAL && reserved.encoding_class == IA64_ENCODING_RESERVED_IF_QP);
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        bundle_write(ENTRY, 9, to_dahr, invala_e, nop_i);
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->retired == 3);
        CHECK(v->s.ip == ENTRY + 16 && v->s.slot == 0);
        bundle_write(ENTRY, 9, from_dahr | 1, from_dahr, nop_i);
        ia64_pr_write(&v->s, 1, false);
        v->s.gr[8] = 0x1234; v->s.ip = ENTRY; v->s.slot = 0;
        engine_run(v, 2);
        CHECK(v->retired == 4 && v->fault.kind == IA64_FAULT_ILLEGAL);
        CHECK(v->fault.ip == ENTRY && v->fault.slot == 1 && v->s.gr[8] == 0x1234);
        vcpu_destroy(v);
    }
}
