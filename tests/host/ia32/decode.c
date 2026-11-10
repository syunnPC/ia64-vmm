/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/ia32/decode.h"

/* SDM Rev.2.3 Vol.2 Tables 9-1/2 and 10-5; Vol.4 instruction encodings.
 * Expected lengths, offsets and intercept records are explicit constants. */
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static struct ia32_native_state n;
static struct ia32_decoded d;
static void init(void)
{
    memset(&n, 0, sizeof(n));
    n.cs.attrib = 0x400;
    n.gpr[IA32_EAX] = 0x12345678;
    n.gpr[IA32_ECX] = 0x10;
    n.gpr[IA32_EDX] = 0x20;
    n.gpr[IA32_EBX] = 0xfffffff0;
    n.gpr[IA32_ESP] = 0x1000;
    n.gpr[IA32_EBP] = 0x2000;
    n.gpr[IA32_ESI] = 0x3000;
    n.gpr[IA32_EDI] = 0x4000;
}
static void good(const uint8_t *b, size_t bytes, enum ia32_instruction_kind kind)
{
    CHECK(ia32_decode(b, bytes, &n, &d) == IA32_DECODE_OK);
    CHECK(d.kind == kind && d.length == bytes);
    for (size_t i = 0; i < bytes; ++i)
        CHECK(ia32_decode(b, i, &n, &d) == IA32_DECODE_INCOMPLETE);
    CHECK(ia32_decode(b, bytes, &n, &d) == IA32_DECODE_OK);
}
#define GOOD(kind, ...)                                                                            \
    do {                                                                                           \
        const uint8_t b[] = {__VA_ARGS__};                                                         \
        good(b, sizeof(b), kind);                                                                  \
    } while (0)
static void jmpe(void)
{
    GOOD(IA32_INSN_JMPE, 0x0f, 0xb8, 0x78, 0x56, 0x34, 0x12);
    CHECK(d.operand == 0x12345678 && !d.operand_memory && d.intercept_code == 6);
    CHECK(d.iim == UINT64_C(0x12345678b80f));
    GOOD(IA32_INSN_JMPE, 0x66, 0x0f, 0xb8, 0x34, 0x12);
    CHECK(d.operand == 0x1234 && d.operand_bits == 16 && d.intercept_code == 0x1004);
    GOOD(IA32_INSN_JMPE, 0x0f, 0x00, 0xf0);
    CHECK(d.operand == 0x12345678 && !d.operand_memory);
    GOOD(IA32_INSN_JMPE, 0x66, 0x0f, 0x00, 0xf0);
    CHECK(d.operand == 0x5678 && !d.operand_memory);
    GOOD(IA32_INSN_JMPE, 0x0f, 0x00, 0x74, 0x8d, 0xf0);
    CHECK(d.operand_memory && d.offset == 0x2030 && d.segment == IA32_SEG_SS);
    GOOD(IA32_INSN_JMPE, 0x64, 0x0f, 0x00, 0x74, 0x8d, 0xf0);
    CHECK(d.offset == 0x2030 && d.segment == IA32_SEG_FS && d.intercept_code == 0x1246);
    GOOD(IA32_INSN_JMPE, 0x0f, 0x00, 0x34, 0x25, 0xf0, 0xff, 0xff, 0xff);
    CHECK(d.offset == 0xfffffff0 && d.segment == IA32_SEG_DS);
    GOOD(IA32_INSN_JMPE, 0x0f, 0x00, 0x34, 0x85, 0xf0, 0xff, 0xff, 0xff);
    CHECK(d.offset == 0x48d159d0 && d.segment == IA32_SEG_DS);
    GOOD(IA32_INSN_JMPE, 0x0f, 0x00, 0x73, 0x20);
    CHECK(d.offset == 0x10 && d.segment == IA32_SEG_DS);
    GOOD(IA32_INSN_JMPE, 0x67, 0x0f, 0x00, 0x32);
    CHECK(d.offset == 0x5000 && d.segment == IA32_SEG_SS && d.address_bits == 16);
    GOOD(IA32_INSN_JMPE, 0x67, 0x0f, 0x00, 0x30);
    CHECK(d.offset == 0x2ff0 && d.segment == IA32_SEG_DS);
    GOOD(IA32_INSN_JMPE, 0x67, 0x0f, 0x00, 0x36, 0xfe, 0xff);
    CHECK(d.offset == 0xfffe && d.segment == IA32_SEG_DS);
    GOOD(IA32_INSN_JMPE, 0x67, 0x0f, 0x00, 0x76, 0xfe);
    CHECK(d.offset == 0x1ffe && d.segment == IA32_SEG_SS);
    n.cs.attrib = 0;
    GOOD(IA32_INSN_JMPE, 0x0f, 0xb8, 0xff, 0xff);
    CHECK(d.operand == 65535 && d.intercept_code == 0);
    GOOD(IA32_INSN_JMPE, 0x66, 0x66, 0x67, 0x0f, 0xb8, 1, 2, 3, 4);
    CHECK(d.operand == 0x04030201 && d.intercept_code == 0x3006);
    init();
}
static void intercepts(void)
{
    const uint8_t ops[] = {0x06, 0x08, 0x09, 0x0b, 0x30, 0x32, 0x34, 0x35, 0xaa};
    for (size_t i = 0; i < sizeof(ops); ++i) {
        uint8_t b[] = {0x0f, ops[i]};
        good(b, sizeof(b), IA32_INSN_INTERCEPT);
    }
    GOOD(IA32_INSN_INTERCEPT, 0xf4);
    GOOD(IA32_INSN_INTERCEPT, 0xcf);
    GOOD(IA32_INSN_INTERCEPT, 0xf1);
    GOOD(IA32_INSN_INTERCEPT, 0x0f, 0x22, 0xc0);
    GOOD(IA32_INSN_INTERCEPT, 0x0f, 0x21, 0xc0);
    GOOD(IA32_INSN_INTERCEPT, 0x0f, 0x23, 0xc0);
    for (unsigned reg = 0; reg < 4; ++reg) {
        uint8_t b[] = {0x0f, 0x00, (uint8_t)(0xc0 | reg << 3)};
        good(b, sizeof(b), IA32_INSN_INTERCEPT);
    }
    const uint8_t groups[] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x30, 0x38};
    for (size_t i = 0; i < sizeof(groups); ++i) {
        uint8_t b[] = {0x0f, 0x01, groups[i]};
        good(b, sizeof(b), IA32_INSN_INTERCEPT);
    }
    GOOD(IA32_INSN_INTERCEPT, 0x64, 0x66, 0x67, 0x0f, 0x01, 0x16, 0x78, 0x56);
    CHECK(d.intercept_code == 0x3240 && d.iim == UINT64_C(0x567816010f));
    GOOD(IA32_INSN_INT, 0xcd, 0x80);
    CHECK(d.vector == 0x80);
    GOOD(IA32_INSN_INT, 0xcd, 3);
    CHECK(d.vector == 3);
    GOOD(IA32_INSN_INT3, 0xcc);
    CHECK(d.vector == 3);
    GOOD(IA32_INSN_INTO, 0xce);
    CHECK(d.vector == 4);
}
static void policy(void)
{
    GOOD(IA32_INSN_SYSTEM_FLAG, 0xfa);
    GOOD(IA32_INSN_SYSTEM_FLAG, 0xfb);
    GOOD(IA32_INSN_SYSTEM_FLAG, 0x9d);
    GOOD(IA32_INSN_SYSTEM_FLAG, 0x17);
    GOOD(IA32_INSN_SYSTEM_FLAG, 0x8e, 0xd0);
    GOOD(IA32_INSN_SEGMENT, 0x8e, 0xd8);
    GOOD(IA32_INSN_SEGMENT, 0x0f, 0xb2, 0x00);
    GOOD(IA32_INSN_CONTROL_READ, 0x0f, 0x20, 0xc0);
    GOOD(IA32_INSN_CPUID, 0x0f, 0xa2);
    GOOD(IA32_INSN_CLOCK, 0x0f, 0x31);
    GOOD(IA32_INSN_CLOCK, 0x0f, 0x33);
    GOOD(IA32_INSN_IO, 0xe4, 0x80);
    GOOD(IA32_INSN_IO, 0x6f);
    CHECK((d.flags & (IA32_INSN_MEMORY | IA32_INSN_STRING)) ==
          (IA32_INSN_MEMORY | IA32_INSN_STRING));
    GOOD(IA32_INSN_FAR, 0xea, 1, 2, 3, 4, 5, 6);
    GOOD(IA32_INSN_FAR, 0xff, 0x18);
    GOOD(IA32_INSN_FAR, 0xcb);
    GOOD(IA32_INSN_NATIVE, 0x90);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0xa4);
    CHECK((d.flags & (IA32_INSN_REP | IA32_INSN_STRING | IA32_INSN_MEMORY)) ==
          (IA32_INSN_REP | IA32_INSN_STRING | IA32_INSN_MEMORY));
    GOOD(IA32_INSN_NATIVE, 0xf0, 0x01, 0x08);
    CHECK((d.flags & (IA32_INSN_LOCK | IA32_INSN_RMW | IA32_INSN_WRITE_FLAGS)) ==
          (IA32_INSN_LOCK | IA32_INSN_RMW | IA32_INSN_WRITE_FLAGS));
    GOOD(IA32_INSN_NATIVE, 0x87, 0x08);
    CHECK(d.flags & IA32_INSN_RMW);
    GOOD(IA32_INSN_NATIVE, 0x66, 0x81, 0x84, 0x8d, 1, 2, 3, 4, 0x34, 0x12);
    CHECK(d.iim == UINT64_C(0x34040302018d8481));
    GOOD(IA32_INSN_NATIVE, 0x9b);
    CHECK(d.flags & IA32_INSN_FP);
    GOOD(IA32_INSN_NATIVE, 0xd9, 0xe8);
    CHECK(d.flags & IA32_INSN_FP);
    GOOD(IA32_INSN_NATIVE, 0xdb, 0xe3);
    GOOD(IA32_INSN_NATIVE, 0xdf, 0xe0);
    GOOD(IA32_INSN_NATIVE, 0x0f, 0xfc, 0xc1);
    CHECK(d.flags & IA32_INSN_MMX);
    GOOD(IA32_INSN_NATIVE, 0x0f, 0x77);
    CHECK(d.flags & IA32_INSN_MMX);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0x58, 0xc1);
    CHECK(d.flags & IA32_INSN_SSE);
    GOOD(IA32_INSN_NATIVE, 0x0f, 0x28, 0xc1);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0x6f, 0x00);
    CHECK(d.access_kind == IA32_ACCESS_READ && d.access_width == 16);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0x7e, 0x00);
    CHECK(d.access_kind == IA32_ACCESS_READ && d.access_width == 8);
    GOOD(IA32_INSN_NATIVE, 0x66, 0x0f, 0x2c, 0x00);
    CHECK(d.access_kind == IA32_ACCESS_READ && d.access_width == 16);
    GOOD(IA32_INSN_NATIVE, 0x0f, 0x5a, 0x00);
    CHECK(d.access_width == 8);
    GOOD(IA32_INSN_NATIVE, 0x0f, 0xae, 0x00);
    CHECK((d.flags & (IA32_INSN_FP | IA32_INSN_SSE)) == (IA32_INSN_FP | IA32_INSN_SSE));
    GOOD(IA32_INSN_NATIVE, 0x0f, 0xae, 0xf8);
    GOOD(IA32_INSN_NATIVE, 0x66, 0x66, 0x90);
    CHECK(d.operand_bits == 16 && d.prefix_length == 2);
    GOOD(IA32_INSN_NATIVE, 0x26, 0x65, 0x90);
    CHECK(d.segment == IA32_SEG_GS && d.intercept_code == 0x22c6);
}
static void refuse(void)
{
    /* Newer host instructions are permitted when their extent is known. */
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0x1e, 0xfb);
    GOOD(IA32_INSN_NATIVE, 0x66, 0x0f, 0x58, 0xc0);
    GOOD(IA32_INSN_NATIVE, 0xf2, 0x0f, 0x58, 0xc0);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0xbc, 0xc0);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0xbd, 0xc0);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0xb8, 0xc0);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x90);
    GOOD(IA32_INSN_NATIVE, 0x0f, 0x01, 0xd0);
    GOOD(IA32_INSN_CLOCK, 0x0f, 0x01, 0xf9);
    GOOD(IA32_INSN_NATIVE, 0xc5, 0xf8, 0x77);
    GOOD(IA32_INSN_NATIVE, 0xc5, 0xf8, 0x58, 0xc0);
    GOOD(IA32_INSN_NATIVE, 0xc4, 0xe2, 0x79, 0x00, 0xc0);
    GOOD(IA32_INSN_NATIVE, 0x0f, 0xae, 0xe8);
    GOOD(IA32_INSN_NATIVE, 0x0f, 0xc7, 0xf0);
    GOOD(IA32_INSN_NATIVE, 0x66, 0x0f, 0x38, 0x00, 0xc0);
    GOOD(IA32_INSN_NATIVE, 0x66, 0x0f, 0x3a, 0x0f, 0xc0, 7);
    const uint8_t illegal[][3] = {
        {0xf0, 0x90},       {0xf0, 0x01, 0xc0}, {0x8e, 0xc8},    {0x0f, 0x00, 0xf8}, {0xd9, 0xd1},
        {0x0f, 0x01, 0xc0}, {0xff, 0xd8},       {0xc7, 0xc8, 0}, {0xf0, 0xd1, 0x20},
    };
    const unsigned lens[] = {2, 3, 2, 3, 2, 3, 2, 3, 3};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); ++i)
        CHECK(ia32_decode(illegal[i], lens[i], &n, &d) == IA32_DECODE_ILLEGAL);
    uint8_t full[16];
    memset(full, 0x66, sizeof(full));
    CHECK(ia32_decode(full, 14, &n, &d) == IA32_DECODE_INCOMPLETE);
    CHECK(ia32_decode(full, 15, &n, &d) == IA32_DECODE_ILLEGAL);
    CHECK(d.overlong && d.length == 15);
    full[14] = 0x90;
    CHECK(ia32_decode(full, sizeof(full), &n, &d) == IA32_DECODE_OK && d.length == 15);
    full[14] = 0xb8;
    CHECK(ia32_decode(full, sizeof(full), &n, &d) == IA32_DECODE_ILLEGAL);
    CHECK(d.overlong);
    const uint8_t locked[] = {0x64, 0xf0, 0x90};
    CHECK(ia32_decode(locked, sizeof(locked), &n, &d) == IA32_DECODE_ILLEGAL);
    CHECK(!d.overlong && d.length == 3 && d.intercept_code == 0x224e && d.iim == 0x90);
}
static void vector_encodings(void)
{
    /* Intel SDM 334569-093US, Tables A-3/A-6. Bits select no prefix,
     * 66, F3 and F2 respectively; blank opcode-map cells are reserved. */
    static const struct {
        uint8_t op, prefixes;
    } forms[] = {
        {0x10, 15}, {0x11, 15}, {0x12, 15}, {0x13, 3},  {0x14, 3},  {0x15, 3},
        {0x16, 7},  {0x17, 3},  {0x28, 3},  {0x29, 3},  {0x2a, 15}, {0x2b, 3},
        {0x2c, 15}, {0x2d, 15}, {0x2e, 3},  {0x2f, 3},  {0x50, 3},  {0x51, 15},
        {0x52, 5},  {0x53, 5},  {0x54, 3},  {0x55, 3},  {0x56, 3},  {0x57, 3},
        {0x58, 15}, {0x59, 15}, {0x5a, 15}, {0x5b, 7},  {0x5c, 15}, {0x5d, 15},
        {0x5e, 15}, {0x5f, 15}, {0x60, 3},  {0x61, 3},  {0x62, 3},  {0x63, 3},
        {0x64, 3},  {0x65, 3},  {0x66, 3},  {0x67, 3},  {0x68, 3},  {0x69, 3},
        {0x6a, 3},  {0x6b, 3},  {0x6c, 2},  {0x6d, 2},  {0x6e, 3},  {0x6f, 7},
        {0x70, 15}, {0x71, 3},  {0x72, 3},  {0x73, 3},  {0x74, 3},  {0x75, 3},
        {0x76, 3},  {0x7c, 10}, {0x7d, 10}, {0x7e, 7},  {0x7f, 7},  {0xc2, 15},
        {0xc4, 3},  {0xc5, 3},  {0xc6, 3},  {0xd0, 10}, {0xd1, 3},  {0xd2, 3},
        {0xd3, 3},  {0xd4, 3},  {0xd5, 3},  {0xd6, 14}, {0xd7, 3},  {0xd8, 3},
        {0xd9, 3},  {0xda, 3},  {0xdb, 3},  {0xdc, 3},  {0xdd, 3},  {0xde, 3},
        {0xdf, 3},  {0xe0, 3},  {0xe1, 3},  {0xe2, 3},  {0xe3, 3},  {0xe4, 3},
        {0xe5, 3},  {0xe6, 14}, {0xe7, 3},  {0xe8, 3},  {0xe9, 3},  {0xea, 3},
        {0xeb, 3},  {0xec, 3},  {0xed, 3},  {0xee, 3},  {0xef, 3},  {0xf0, 8},
        {0xf1, 3},  {0xf2, 3},  {0xf3, 3},  {0xf4, 3},  {0xf5, 3},  {0xf6, 3},
        {0xf7, 3},  {0xf8, 3},  {0xf9, 3},  {0xfa, 3},  {0xfb, 3},  {0xfc, 3},
        {0xfd, 3},  {0xfe, 3},
    };
    static const uint8_t prefix[] = {0, 0x66, 0xf3, 0xf2};
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); ++i) {
        unsigned op = forms[i].op;
        for (unsigned p = 0; p < 4; ++p) {
            uint8_t b[5] = {0};
            unsigned size = 0;
            if (p)
                b[size++] = prefix[p];
            b[size++] = 0x0f;
            b[size++] = op;
            b[size++] = op == 0x50 || op == 0xc5 || op == 0xd7 || op == 0xf7 ||
                                (op == 0xd6 && p >= 2) ? 0xc0 :
                        op >= 0x71 && op <= 0x73 ? 0xd0 : 0;
            if (op == 0x70 || (op >= 0x71 && op <= 0x73) || op == 0xc2 || op == 0xc4 ||
                op == 0xc5 || op == 0xc6)
                b[size++] = 0;
            if (forms[i].prefixes & (1u << p))
                good(b, size, IA32_INSN_NATIVE);
            else {
                CHECK(ia32_decode(b, size, &n, &d) == IA32_DECODE_ILLEGAL);
                CHECK(!d.overlong);
            }
        }
    }
    /* Memory/register-only operands and immediate-shift opcode extensions. */
    const uint8_t invalid[][4] = {
        {0x66, 0x0f, 0x12, 0xc0}, {0x66, 0x0f, 0x16, 0xc0},
        {0x00, 0x0f, 0x13, 0xc0}, {0x00, 0x0f, 0x17, 0xc0},
        {0x00, 0x0f, 0x2b, 0xc0}, {0x66, 0x0f, 0xe7, 0xc0},
        {0xf2, 0x0f, 0xf0, 0xc0}, {0xf2, 0x0f, 0xd6, 0x00},
        {0xf3, 0x0f, 0xd6, 0x00}, {0x00, 0x0f, 0x50, 0x00},
        {0x66, 0x0f, 0xc5, 0x00}, {0x66, 0x0f, 0xd7, 0x00},
        {0x00, 0x0f, 0xf7, 0x00},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        unsigned skip = invalid[i][0] == 0;
        CHECK(ia32_decode(invalid[i] + skip, 4 - skip, &n, &d) == IA32_DECODE_ILLEGAL);
    }
    for (unsigned op = 0x71; op <= 0x73; ++op)
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned g = 0; g < 8; ++g)
                for (unsigned mem = 0; mem < 2; ++mem) {
                    const uint8_t b[] = {0x66, 0x0f, op, (mem ? 0 : 0xc0) | g << 3, 0};
                    bool valid = !mem && (g == 2 || g == 6 || (op != 0x73 && g == 4) ||
                                          (op == 0x73 && p && (g == 3 || g == 7)));
                    CHECK(ia32_decode(b + !p, sizeof(b) - !p, &n, &d) ==
                          (valid ? IA32_DECODE_OK : IA32_DECODE_ILLEGAL));
                }
    /* Combining mandatory selectors has no qualified memory semantics. */
    const uint8_t mixed[] = {0x66, 0xf2, 0x0f, 0x58, 0x00};
    CHECK(ia32_decode(mixed, sizeof(mixed), &n, &d) == IA32_DECODE_UNSUPPORTED);
    GOOD(IA32_INSN_NATIVE, 0xf2, 0x0f, 0x12, 0x00);
    CHECK(d.access_kind == IA32_ACCESS_READ && d.access_width == 8);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0x16, 0x00);
    CHECK(d.access_kind == IA32_ACCESS_READ && d.access_width == 16);
    GOOD(IA32_INSN_NATIVE, 0x0f, 0x60, 0x00);
    CHECK(d.access_kind == IA32_ACCESS_READ && d.access_width == 4);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0xd6, 0xc0);
    CHECK((d.flags & (IA32_INSN_MMX | IA32_INSN_SSE)) == (IA32_INSN_MMX | IA32_INSN_SSE));
    GOOD(IA32_INSN_NATIVE, 0x66, 0x0f, 0xe6, 0x00);
    CHECK(d.access_kind == IA32_ACCESS_READ && d.access_width == 16);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0xe6, 0x00);
    CHECK(d.access_kind == IA32_ACCESS_READ && d.access_width == 8);
}
static void native_features(void)
{
    /* SDM 253666/253667-093US: SSE3 and POPCNT instruction CPUID flags. */
    static const uint8_t sse3[][4] = {
        {0xf2, 0x0f, 0x12, 0}, {0xf3, 0x0f, 0x12, 0}, {0xf3, 0x0f, 0x16, 0},
        {0x66, 0x0f, 0x7c, 0}, {0xf2, 0x0f, 0x7c, 0}, {0x66, 0x0f, 0x7d, 0},
        {0xf2, 0x0f, 0x7d, 0}, {0x66, 0x0f, 0xd0, 0}, {0xf2, 0x0f, 0xd0, 0},
        {0xf2, 0x0f, 0xf0, 0},
    };
    for (size_t i = 0; i < sizeof(sse3) / sizeof(sse3[0]); ++i) {
        good(sse3[i], sizeof(sse3[i]), IA32_INSN_NATIVE);
        CHECK(d.native_features == IA32_NATIVE_SSE3);
    }
    for (unsigned i = 0; i < 3; ++i) {
        const uint8_t b[] = {0xdb + 2 * i, 0x08};
        good(b, sizeof(b), IA32_INSN_NATIVE);
        CHECK(d.native_features == IA32_NATIVE_SSE3);
        CHECK(d.access_kind == IA32_ACCESS_WRITE && d.access_width == (i == 0 ? 4 : i == 1 ? 8 : 2));
    }
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0xb8, 0x00);
    CHECK(d.native_features == IA32_NATIVE_POPCNT);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0xbc, 0x00);
    CHECK(d.native_features == 0);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0xbd, 0x00);
    CHECK(d.native_features == 0);
    GOOD(IA32_INSN_NATIVE, 0x66, 0x0f, 0x12, 0x00);
    CHECK(d.native_features == 0);
    GOOD(IA32_INSN_NATIVE, 0xdb, 0x00);
    CHECK(d.native_features == 0);
}
static void x87_encodings(void)
{
    /* SDM 334569-093US, Tables A-7 through A-22, one bit per /reg
     * memory form and per register encoding from C0 through FF. */
    static const uint8_t memory[] = {0xff, 0xfd, 0xff, 0xaf, 0xff, 0xdf, 0xff, 0xff};
    static const uint64_t registers[] = {
        UINT64_C(0xffffffffffffffff), UINT64_C(0xffff7f330001ffff),
        UINT64_C(0x00000200ffffffff), UINT64_C(0x00ffff0cffffffff),
        UINT64_C(0xffffffff0000ffff), UINT64_C(0x0000ffffffff00ff),
        UINT64_C(0xffffffff0200ffff), UINT64_C(0x00ffff0100000000),
    };
    for (unsigned op = 0; op < 8; ++op)
        for (unsigned m = 0; m < 256; ++m) {
            uint8_t b[7] = {0xd8 + op, m};
            unsigned size = 2;
            if (m < 0xc0) {
                if ((m & 7) == 4)
                    b[size++] = 0;
                size += m >= 0x80 || (m < 0x40 && (m & 7) == 5) ? 4 : m >= 0x40 ? 1 : 0;
            }
            bool valid = m < 0xc0 ? (memory[op] & (1u << ((m >> 3) & 7))) != 0 :
                                   (registers[op] & (UINT64_C(1) << (m - 0xc0))) != 0;
            CHECK(ia32_decode(b, size, &n, &d) ==
                  (valid ? IA32_DECODE_OK : IA32_DECODE_ILLEGAL));
        }
}
static void group15_encodings(void)
{
    /* FXSAVE/FXRSTOR/LDMXCSR/STMXCSR are NP encodings in
     * SDM 253666/253667-093US; NP excludes 66, F2 and F3. */
    static const uint8_t prefix[] = {0x66, 0xf2, 0xf3};
    for (unsigned reg = 0; reg < 4; ++reg) {
        const uint8_t b[] = {0x0f, 0xae, reg << 3};
        good(b, sizeof(b), IA32_INSN_NATIVE);
        for (unsigned p = 0; p < sizeof(prefix); ++p) {
            const uint8_t reserved[] = {prefix[p], 0x0f, 0xae, reg << 3};
            CHECK(ia32_decode(reserved, sizeof(reserved), &n, &d) == IA32_DECODE_ILLEGAL);
        }
    }
    /* Other group 15 memory forms have newer, noncontiguous effects. */
    GOOD(IA32_INSN_NATIVE, 0x66, 0x0f, 0xae, 0x30);
    CHECK(d.access_kind == IA32_ACCESS_UNKNOWN && d.access_width == 0);
    GOOD(IA32_INSN_NATIVE, 0xf3, 0x0f, 0xae, 0x20);
    CHECK(d.access_kind == IA32_ACCESS_UNKNOWN && d.access_width == 0);
}
int main(void)
{
    init();
    jmpe();
    intercepts();
    policy();
    refuse();
    vector_encodings();
    native_features();
    x87_encodings();
    group15_encodings();
    printf("ia32 decode: %u checks passed\n", checks);
    return 0;
}
