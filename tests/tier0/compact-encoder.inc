/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The shorter encodings retain operands and arithmetic flags. Compare
 * executable code against the generic imm32 / imm8 encodings as well as
 * fixed bytes, including registers that require a bare REX prefix. */
static void compact_flags(struct x86_buffer *b)
{
    x86_u8(b, 0x9c); /* pushfq */
    x86_pop(b, X_RDX); x86_store(b, X_RSI, 0, X_RDX, 8); x86_u8(b, 0xc3);
}
static void test_compact_encoder(void)
{
    uint8_t bytes[128];
    struct x86_buffer b = {.data = bytes, .capacity = sizeof(bytes)};
    x86_imm(&b, 0, X_RAX, 128); x86_imm(&b, 4, X_RAX, -129);
    x86_imm(&b, 7, X_RAX, 127); x86_imm(&b, 6, X_R10, 128);
    x86_shift(&b, 4, X_RAX, 1, false); x86_shift(&b, 5, X_R12, 1, false);
    x86_shift(&b, 7, X_RDI, 0, false); x86_shift(&b, 4, X_RAX, 1, true);
    x86_setcc(&b, X_LE, X_RCX); x86_setcc(&b, X_E, X_RSP); x86_setcc(&b, X_NE, X_R8);
    x86_store(&b, X_RDI, 0, X_RAX, 1); x86_store(&b, X_RDI, 0, X_RSP, 1);
    x86_load_index(&b, X_RCX, X_RAX, X_RDX, 2, 0, 4);
    x86_store_index(&b, X_RAX, X_RDX, 0, 0, X_RAX, 1);
    x86_store_index(&b, X_RAX, X_RDX, 0, 0, X_RSP, 1);
    const uint8_t expected[] = {
        0x48,0x05,0x80,0,0,0, 0x48,0x25,0x7f,0xff,0xff,0xff,
        0x48,0x83,0xf8,0x7f, 0x49,0x81,0xf2,0x80,0,0,0,
        0x48,0xd1,0xe0, 0x49,0xd1,0xec, 0x48,0xc1,0xff,0, 0x48,0xd3,0xe0,
        0x0f,0x9e,0xc1, 0x40,0x0f,0x94,0xc4, 0x41,0x0f,0x95,0xc0,
        0x88,0x07, 0x40,0x88,0x27, 0x8b,0x0c,0x90,
        0x88,0x04,0x10, 0x40,0x88,0x24,0x10,
    };
    encoded(expected, sizeof(expected), &b);
    void *code = hal_alloc_pages(1, HAL_PAGE_EXEC); CHECK(code != NULL);
    const int32_t immediates[] = {INT32_MIN,-129,-128,-1,0,1,127,128,INT32_MAX};
    const uint64_t values[] = {0,1,127,128,UINT32_MAX,UINT64_C(0x7fffffffffffffff),
        UINT64_C(0x8000000000000000),UINT64_MAX};
    for (unsigned shift = 0; shift < 2; ++shift)
        for (unsigned group = 0; group < 8; ++group) {
            if (shift && group == 6) continue;
            for (unsigned n = 0; n < (shift ? 1 : IA64VMM_ARRAY_SIZE(immediates)); ++n)
                for (unsigned carry = 0; carry < 2; ++carry) {
                    for (unsigned generic = 0; generic < 2; ++generic) {
                        b = (struct x86_buffer){.data = (uint8_t *)code + 128 * generic, .capacity = 128};
                        x86_mov(&b, X_RAX, X_RDI);
                        x86_alu(&b, 0x85, X_RAX, X_RAX, false);
                        x86_u8(&b, carry ? 0xf9 : 0xf8); /* stc / clc */
                        if (generic) {
                            x86_u8(&b, 0x48); x86_u8(&b, shift ? 0xc1 : 0x81);
                            x86_u8(&b, 0xc0 | (group << 3));
                            if (shift) x86_u8(&b, 1); else x86_u32(&b, immediates[n]);
                        } else if (shift) x86_shift(&b, group, X_RAX, 1, false);
                        else x86_imm(&b, group, X_RAX, immediates[n]);
                        compact_flags(&b); CHECK(!b.failed);
                    }
                    hal_code_set_executable(code, 1);
                    for (unsigned value = 0; value < IA64VMM_ARRAY_SIZE(values); ++value) {
                        uint64_t actual_flags, expected_flags;
                        uint64_t actual = ((uint64_t (*)(uint64_t, uint64_t *))code)(values[value], &actual_flags);
                        uint64_t wanted = ((uint64_t (*)(uint64_t, uint64_t *))((uint8_t *)code + 128))
                            (values[value], &expected_flags);
                        /* AF is undefined for logical operations and shifts. */
                        uint64_t flags = shift || group == 1 || group == 4 || group == 6 ? 0x8c5 : 0x8d5;
                        CHECK(actual == wanted && !((actual_flags ^ expected_flags) & flags));
                    }
                    hal_code_set_writable(code, 1);
                }
        }
    hal_free_pages(code, 1);
}
