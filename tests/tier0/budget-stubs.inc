/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_budget_add_encoder(void)
{
    uint8_t *pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pages != MAP_FAILED && mprotect(pages + 4096, 4096, PROT_NONE) == 0);
    uint64_t *word = (uint64_t *)(pages + 4088);
    void *code = hal_alloc_pages(1, HAL_PAGE_EXEC); CHECK(code != NULL);
    struct x86_buffer b = {.data = code, .capacity = 4096};
    x86_push(&b, X_R12); x86_push(&b, X_R13);
    x86_mov(&b, X_R12, X_RDI); x86_mov(&b, X_R13, X_RSI);
    x86_add_store(&b, X_R12, 0, X_R13);
    x86_pop(&b, X_R13); x86_pop(&b, X_R12); x86_u8(&b, 0xc3); CHECK(!b.failed);
    hal_code_set_executable(code, 1);
    void (*add_to)(uint64_t *, uint64_t) = code;
    const uint64_t values[] = {0,1,UINT32_MAX,UINT64_C(0x100000000),UINT64_C(0x8000000000000000),UINT64_MAX};
    for (unsigned a = 0; a < IA64VMM_ARRAY_SIZE(values); ++a)
        for (unsigned d = 0; d < IA64VMM_ARRAY_SIZE(values); ++d) {
            *word = values[a]; add_to(word, values[d]); CHECK(*word == values[a] + values[d]);
        }
    hal_free_pages(code, 1); CHECK(munmap(pages, 8192) == 0);
    uint8_t bytes[16], truncated[16];
    b = (struct x86_buffer){.data = bytes, .capacity = sizeof(bytes)};
    x86_add_store(&b, X_R12, 0x12345678, X_R13); CHECK(!b.failed);
    for (size_t capacity = 0; capacity <= b.size; ++capacity) {
        memset(truncated, 0xa5, sizeof(truncated));
        struct x86_buffer part = {.data = truncated, .capacity = capacity};
        x86_add_store(&part, X_R12, 0x12345678, X_R13);
        CHECK(part.size == capacity && part.failed == (capacity < b.size));
        CHECK(!memcmp(truncated, bytes, capacity));
        for (size_t n = capacity; n < sizeof(truncated); ++n) CHECK(truncated[n] == 0xa5);
        part = (struct x86_buffer){.capacity = capacity};
        x86_add_store(&part, X_R12, 0x12345678, X_R13);
        CHECK(part.size == capacity && part.failed == (capacity < b.size));
    }
}

static void test_native_budget_stubs(struct vcpu *v, struct vcpu *ref)
{
    test_budget_add_encoder();
    for (unsigned start = 0; start < 3; ++start)
        for (unsigned boundary = 0; boundary < 7; ++boundary)
            for (unsigned variant = 0; variant < 16; ++variant) {
                randomized(v); v->s.psr = variant & 1 ? IA64_PSR_IC : 0; v->s.pr = 1;
                v->s.cfm = 96 | (UINT64_C(96) << 7); v->s.rse_dirty = 0;
                v->retired = UINT64_MAX - variant % 9;
                Ia64Instruction insns[9];
                for (unsigned at = 0; at < 9; ++at)
                    insns[at] = (Ia64Instruction){.opcode = at % 3 == 1 ? IA64_OP_NOP : IA64_OP_ADDS,
                        .unit = IA64_UNIT_I, .qp = variant & 2 && at % 2 ? 1 : 0,
                        .operands.integer = {.destination = at % 2 ? 32 : 127,
                            .source2 = at % 2 ? 127 : 32, .immediate = at + 1}};
                bool native = true;
                if (boundary == 1) {
                    insns[4].opcode = IA64_OP_POPCNT; insns[4].qp = 0; native = false;
                } else if (boundary == 2) {
                    insns[4].opcode = IA64_OP_LD8; insns[4].unit = IA64_UNIT_M; insns[4].qp = 0;
                    insns[4].operands.memory = (IA64MemoryOperands){.destination = 32, .base = 10};
                    ia64_gr_write(&v->s, 10, DEVICE + 8, false); native = false;
                } else if (boundary == 3) {
                    insns[4].opcode = IA64_OP_ALLOC; insns[4].qp = 0;
                    insns[4].operands.system = (IA64SystemOperands){.destination = 32, .immediate = 96 | 96 << 7};
                } else if (boundary == 4) {
                    insns[4].opcode = IA64_OP_BR_COND; insns[4].unit = IA64_UNIT_B; insns[4].qp = 1;
                } else if (boundary == 5) { insns[4].reserved_field = true; native = false; }
                else if (boundary == 6) {
                    unsigned at = (4 - start) % 3;
                    insns[at].opcode = IA64_OP_MOVL; insns[at].unit = IA64_UNIT_L; insns[at].qp = 0;
                }
                forward_values_region(v, ref, insns, 9, start, native);
            }
}
