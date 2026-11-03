/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_cmp_immediate_encoder(void)
{
    uint8_t bytes[128];
    struct x86_buffer b = {.data = bytes, .capacity = sizeof(bytes)};
    x86_cmp_imm(&b, X_RAX, 128, false); x86_cmp_imm(&b, X_RAX, -129, true);
    x86_cmp_imm(&b, X_RDX, -128, false); x86_cmp_imm(&b, X_RDX, 127, true);
    x86_cmp_imm(&b, X_R10, INT32_MIN, false); x86_cmp_imm(&b, X_R10, INT32_MAX, true);
    const uint8_t expected[] = {0x48,0x3d,0x80,0,0,0, 0x3d,0x7f,0xff,0xff,0xff,
        0x48,0x83,0xfa,0x80, 0x83,0xfa,0x7f,
        0x49,0x81,0xfa,0,0,0,0x80, 0x41,0x81,0xfa,0xff,0xff,0xff,0x7f};
    encoded(expected, sizeof(expected), &b);
    void *code = hal_alloc_pages(1, HAL_PAGE_EXEC); CHECK(code != NULL);
    const int32_t immediates[] = {INT32_MIN,-129,-128,-1,0,1,127,128,INT32_MAX};
    const uint64_t values[] = {0,1,127,128,UINT32_MAX,UINT64_C(1) << 31,
        UINT64_C(1) << 32,(UINT64_C(1) << 63) - 1,UINT64_C(1) << 63,UINT64_MAX};
    const unsigned regs[] = {X_RDX, X_R10};
    for (unsigned word = 0; word < 2; ++word)
        for (unsigned reg = 0; reg < IA64VMM_ARRAY_SIZE(regs); ++reg)
            for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(immediates); ++n) {
                for (unsigned generic = 0; generic < 2; ++generic) {
                    b = (struct x86_buffer){.data = (uint8_t *)code + 128 * generic, .capacity = 128};
                    x86_mov(&b, regs[reg], X_RDI);
                    if (generic) {
                        x86_mov_imm(&b, X_RAX, (uint64_t)(int64_t)immediates[n]);
                        x86_alu(&b, 0x39, regs[reg], X_RAX, word);
                    } else x86_cmp_imm(&b, regs[reg], immediates[n], word);
                    x86_mov(&b, X_RAX, regs[reg]); compact_flags(&b); CHECK(!b.failed);
                }
                hal_code_set_executable(code, 1);
                for (unsigned value = 0; value < IA64VMM_ARRAY_SIZE(values); ++value) {
                    uint64_t actual_flags, expected_flags;
                    uint64_t actual = ((uint64_t (*)(uint64_t,uint64_t *))code)(values[value], &actual_flags);
                    uint64_t wanted = ((uint64_t (*)(uint64_t,uint64_t *))((uint8_t *)code + 128))
                        (values[value], &expected_flags);
                    CHECK(actual == values[value] && actual == wanted &&
                        !((actual_flags ^ expected_flags) & UINT64_C(0x8d5)));
                }
                hal_code_set_writable(code, 1);
            }
    hal_free_pages(code, 1);
}

static void test_native_compare_immediate(struct vcpu *v, struct vcpu *ref)
{
    enum relation { REL_EQ,REL_NE,REL_LT,REL_LE,REL_GT,REL_GE,REL_LTU,REL_LEU,REL_GTU,REL_GEU };
    struct compare_desc { bool valid, word, immediate; enum relation relation; Ia64PredicateUpdate update; };
    static const struct compare_desc compares[IA64_OP_COUNT] = {
#include "ia64/decode/compare.inc"
    };
    const int64_t immediates[] = {INT64_MIN,(int64_t)INT32_MIN - 1,INT32_MIN,-129,-128,-1,0,1,
        127,128,INT32_MAX,(int64_t)INT32_MAX + 1,UINT32_MAX,INT64_MAX};
    const unsigned sources[] = {0,8,32,127};
    for (unsigned opcode = 0; opcode < IA64_OP_COUNT; ++opcode) {
        if (!compares[opcode].valid || !compares[opcode].immediate) continue;
        for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(immediates); ++n)
            for (unsigned source = 0; source < IA64VMM_ARRAY_SIZE(sources); ++source)
                for (unsigned variant = 0; variant < 12; ++variant) {
                    randomized(v);
                    unsigned bof = (n * 7 + variant) % IA64_N_STACKED_PHYS;
                    v->s.rse_bof = bof;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                        ((uint64_t)((bof * 7) % 96) << 18) | ((uint64_t)(bof % 48) << 32);
                    v->s.psr = variant & 1 ? IA64_PSR_IC : 0;
                    unsigned p1 = (n * 5 + variant) % 64, p2 = (p1 + 1) % 64;
                    Ia64Instruction i = {.opcode = opcode, .valid = true, .unit = IA64_UNIT_I,
                        .address = BASE, .slot = variant % 3,
                        .qp = variant < 4 ? 0 : variant < 8 ? p1 : p2,
                        .compare_unc = (variant & 2) != 0,
                        .operands.integer = {.source2 = sources[source],
                            .predicate1 = p1, .predicate2 = p2, .immediate = immediates[n]}};
                    /* Unsigned arithmetic deliberately covers wrap boundaries. */
                    uint64_t value = (uint64_t)immediates[n] + variant % 3 - 1;
                    if (variant == 3) value ^= UINT64_C(1) << 63;
                    ia64_gr_write(&v->s, sources[source], value, variant & 1);
                    if (i.qp) ia64_pr_write(&v->s, i.qp, variant % 4 < 2);
                    bool helper = ia64_pr_read(&v->s, i.qp) && ia64_nat_read(&v->s, sources[source]);
                    uint64_t before = native_helper_calls;
                    native_single(v, ref, &i, !helper);
                    if (helper) CHECK(native_helper_calls > before);
                }
    }
}
