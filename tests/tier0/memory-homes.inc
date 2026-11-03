/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Read and postincrement homes under every physical register rotation.
 * Compare RAM as well as state, including destination/increment aliases
 * and guards which must fault before an architectural write. */
static void test_native_memory_homes(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t pa = DATA + 0x1000, va = UINT64_C(0x6000000006000000);
    code_cache_flush();
    for (unsigned virt = 0; virt < 2; ++virt)
        for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
            for (unsigned width = 0; width < 4; ++width)
                for (unsigned store = 0; store < 2; ++store)
                    for (unsigned variant = 0; variant < 8; ++variant) {
                        randomized(v); v->s.psr = IA64_PSR_IC | (virt ? IA64_PSR_DT : 0); v->s.pr = 1;
                        v->s.rse_bof = bof;
                        v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                            ((uint64_t)((bof * 7) % 96) << 18);
                        if (virt) { virtual_map(v, 1, 0, va, pa, 12, FETCH_FLAGS); ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0); }
                        unsigned base = bof & 1 ? 32 : 127, source = variant == 6 ? base : 49;
                        unsigned destination = variant == 7 ? base : variant == 5 ? source : 48;
                        bool update = variant == 3 || variant == 5 || variant == 7;
                        Ia64Instruction i = {.opcode = (store ? IA64_OP_ST1 : IA64_OP_LD1) + width,
                            .valid = true, .unit = IA64_UNIT_M, .address = BASE, .slot = 1,
                            .qp = variant == 1 ? 1 : 0, .reg_base_update = update && variant != 7,
                            .imm_base_update = variant == 7, .mem_release = store && (variant & 1),
                            .operands.memory = {.destination = destination, .source = source, .base = base, .immediate = 8}};
                        ia64_gr_write(&v->s, source, variant == 5 ? UINT64_C(-8) : 16, variant == 3);
                        ia64_gr_write(&v->s, base, (virt ? va : pa) + 128 + (variant == 4 ? 1 : 0),
                                      variant == 1 || variant == 2 || variant == 7);
                        CHECK(ia64_physical_region(v, pa, 8) != NULL);
                        uint8_t original[32], expected[32];
                        uint8_t *memory = ram + pa - BASE + 120;
                        for (unsigned byte = 0; byte < sizeof(original); ++byte) original[byte] = memory[byte] = random64();
                        *ref = *v;
                        struct ia64_result wanted = ia64_execute(ref, &i); expect_advance(ref, &i, &wanted);
                        memcpy(expected, memory, sizeof(expected)); memcpy(memory, original, sizeof(original));
                        uint64_t helpers = native_helper_calls;
                        struct ia64_result got = tier0_test_instruction(v, &i);
                        CHECK(got.kind == wanted.kind && got.fault == wanted.fault &&
                              (got.kind == IA64_STEP_COMPLETE || got.info == wanted.info));
                        CHECK(!memcmp(&v->s, &ref->s, sizeof(v->s)) && v->retired == ref->retired);
                        CHECK(!memcmp(memory, expected, sizeof(expected)));
                        if (wanted.kind == IA64_STEP_COMPLETE && variant != 7) CHECK(native_helper_calls == helpers);
                    }
    code_cache_flush();
}
