/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/tier0/x86emit.h"

/* Little-endian bytes one at a time: measuring passes and the buffer end. */
void x86_bytes_slow(struct x86_buffer *b, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        if (b->size < b->capacity) {
            if (b->data)
                b->data[b->size] = (uint8_t)(v >> (8 * i));
            ++b->size;
        } else
            b->failed = true;
    }
}
static void rex(struct x86_buffer *b, bool wide, unsigned reg, unsigned base, bool byte)
{
    unsigned v = 0x40 | (wide ? 8 : 0) | ((reg >> 3) << 2) | (base >> 3);
    if (v != 0x40 || byte)
        x86_u8(b, v);
}
/* Register operands have a single ModRM byte and never a SIB or displacement.
 * Keep this small path separate so each register encoder can inline it. */
static void modrm_register(struct x86_buffer *b, unsigned reg, unsigned base)
{
    x86_u8(b, 0xc0 | ((reg & 7) << 3) | (base & 7));
}
static void modrm(struct x86_buffer *b, unsigned reg, unsigned base, bool memory, int32_t disp)
{
    unsigned mode = !memory                        ? 0xc0
                    : !disp && (base & 7) != X_RBP ? 0
                    : disp >= -128 && disp <= 127  ? 0x40
                                                   : 0x80;
    x86_u8(b, mode | ((reg & 7) << 3) | (base & 7));
    if (memory) {
        if ((base & 7) == X_RSP)
            x86_u8(b, 0x24);
        if (mode == 0x40)
            x86_u8(b, disp);
        else if (mode == 0x80)
            x86_u32(b, (uint32_t)disp);
    }
}
void x86_push(struct x86_buffer *b, unsigned r)
{
    if (r >= 8)
        x86_u8(b, 0x41);
    x86_u8(b, 0x50 + (r & 7));
}
void x86_pop(struct x86_buffer *b, unsigned r)
{
    if (r >= 8)
        x86_u8(b, 0x41);
    x86_u8(b, 0x58 + (r & 7));
}
void x86_mov_imm(struct x86_buffer *b, unsigned r, uint64_t v)
{
    if (v <= UINT32_MAX) {
        rex(b, false, 0, r, false);
        x86_u8(b, 0xb8 + (r & 7));
        x86_u32(b, v);
    } else if (v == (uint64_t)(int64_t)(int32_t)v) {
        rex(b, true, 0, r, false);
        x86_u8(b, 0xc7);
        modrm_register(b, 0, r);
        x86_u32(b, v);
    } else
        x86_movabs(b, r, v);
}
void x86_movabs(struct x86_buffer *b, unsigned r, uint64_t v)
{
    rex(b, true, 0, r, false);
    x86_u8(b, 0xb8 + (r & 7));
    x86_u64(b, v);
}
void x86_mov(struct x86_buffer *b, unsigned dst, unsigned src)
{
    rex(b, true, src, dst, false);
    x86_u8(b, 0x89);
    modrm_register(b, src, dst);
}
void x86_load(struct x86_buffer *b, unsigned dst, unsigned base, int32_t disp, unsigned size)
{
    rex(b, size == 8, dst, base, false);
    if (size < 4) {
        x86_u8(b, 0x0f);
        x86_u8(b, size == 1 ? 0xb6 : 0xb7);
    } else
        x86_u8(b, 0x8b);
    modrm(b, dst, base, true, disp);
}
void x86_store(struct x86_buffer *b, unsigned base, int32_t disp, unsigned src, unsigned size)
{
    if (size == 2)
        x86_u8(b, 0x66);
    rex(b, size == 8, src, base, size == 1 && (src & 7) >= X_RSP);
    x86_u8(b, size == 1 ? 0x88 : 0x89);
    modrm(b, src, base, true, disp);
}
void x86_load_signed(struct x86_buffer *b, unsigned dst, unsigned base, int32_t disp, unsigned size)
{
    rex(b, true, dst, base, false);
    if (size == 4)
        x86_u8(b, 0x63);
    else {
        x86_u8(b, 0x0f);
        x86_u8(b, size == 1 ? 0xbe : 0xbf);
    }
    modrm(b, dst, base, true, disp);
}
static void indexed(struct x86_buffer *b, unsigned reg, unsigned base, unsigned index,
                    unsigned scale, int32_t disp)
{
    unsigned mode = !disp && (base & 7) != X_RBP ? 0 : disp >= -128 && disp <= 127 ? 0x40 : 0x80;
    x86_u8(b, mode | 4 | ((reg & 7) << 3));
    x86_u8(b, (scale << 6) | ((index & 7) << 3) | (base & 7));
    if (mode == 0x40)
        x86_u8(b, disp);
    else if (mode == 0x80)
        x86_u32(b, (uint32_t)disp);
}
void x86_load_index(struct x86_buffer *b, unsigned dst, unsigned base, unsigned index,
                    unsigned scale, int32_t disp, unsigned size)
{
    unsigned prefix =
        0x40 | (size == 8 ? 8 : 0) | ((dst >> 3) << 2) | ((index >> 3) << 1) | (base >> 3);
    if (prefix != 0x40)
        x86_u8(b, prefix);
    if (size < 4) {
        x86_u8(b, 0x0f);
        x86_u8(b, size == 1 ? 0xb6 : 0xb7);
    } else
        x86_u8(b, 0x8b);
    indexed(b, dst, base, index, scale, disp);
}
void x86_lea(struct x86_buffer *b, unsigned dst, unsigned base, unsigned index, unsigned scale)
{
    x86_u8(b, 0x48 | ((dst >> 3) << 2) | ((index >> 3) << 1) | (base >> 3));
    x86_u8(b, 0x8d);
    indexed(b, dst, base, index, scale, 0);
}
void x86_load_index_signed(struct x86_buffer *b, unsigned dst, unsigned base, unsigned index,
                           unsigned scale, int32_t disp, unsigned size)
{
    x86_u8(b, 0x48 | ((dst >> 3) << 2) | ((index >> 3) << 1) | (base >> 3));
    if (size == 4)
        x86_u8(b, 0x63);
    else {
        x86_u8(b, 0x0f);
        x86_u8(b, size == 1 ? 0xbe : 0xbf);
    }
    indexed(b, dst, base, index, scale, disp);
}
void x86_store_index(struct x86_buffer *b, unsigned base, unsigned index, unsigned scale,
                     int32_t disp, unsigned src, unsigned size)
{
    if (size == 2)
        x86_u8(b, 0x66);
    unsigned prefix =
        0x40 | (size == 8 ? 8 : 0) | ((src >> 3) << 2) | ((index >> 3) << 1) | (base >> 3);
    if (prefix != 0x40 || (size == 1 && (src & 7) >= X_RSP))
        x86_u8(b, prefix);
    x86_u8(b, size == 1 ? 0x88 : 0x89);
    indexed(b, src, base, index, scale, disp);
}
void x86_add_memory(struct x86_buffer *b, unsigned base, int32_t disp, int8_t value)
{
    rex(b, true, 0, base, false);
    x86_u8(b, 0x83);
    modrm(b, 0, base, true, disp);
    x86_u8(b, value);
}
void x86_add_store(struct x86_buffer *b, unsigned base, int32_t disp, unsigned reg)
{
    rex(b, true, reg, base, false);
    x86_u8(b, 0x01);
    modrm(b, reg, base, true, disp);
}
void x86_cmp_load(struct x86_buffer *b, unsigned reg, unsigned base, int32_t disp)
{
    rex(b, true, reg, base, false);
    x86_u8(b, 0x3b);
    modrm(b, reg, base, true, disp);
}
void x86_sub_load(struct x86_buffer *b, unsigned reg, unsigned base, int32_t disp)
{
    rex(b, true, reg, base, false);
    x86_u8(b, 0x2b);
    modrm(b, reg, base, true, disp);
}
void x86_bt_reg(struct x86_buffer *b, unsigned rm, unsigned bit)
{
    rex(b, true, bit, rm, false);
    x86_u8(b, 0x0f);
    x86_u8(b, 0xa3);
    modrm_register(b, bit, rm);
}
void x86_add_load(struct x86_buffer *b, unsigned reg, unsigned base, int32_t disp)
{
    rex(b, true, reg, base, false);
    x86_u8(b, 0x03);
    modrm(b, reg, base, true, disp);
}
void x86_test_byte(struct x86_buffer *b, unsigned base, int32_t disp, uint8_t imm)
{
    rex(b, false, 0, base, false);
    x86_u8(b, 0xf6);
    modrm(b, 0, base, true, disp);
    x86_u8(b, imm);
}
void x86_cmp_byte(struct x86_buffer *b, unsigned base, int32_t disp, uint8_t imm)
{
    rex(b, false, 0, base, false);
    x86_u8(b, 0x80);
    modrm(b, 7, base, true, disp);
    x86_u8(b, imm);
}
void x86_sfence(struct x86_buffer *b)
{
    x86_u8(b, 0x0f);
    x86_u8(b, 0xae);
    x86_u8(b, 0xf8);
}
void x86_mfence(struct x86_buffer *b)
{
    x86_u8(b, 0x0f);
    x86_u8(b, 0xae);
    x86_u8(b, 0xf0);
}
void x86_alu(struct x86_buffer *b, unsigned opcode, unsigned dst, unsigned src, bool word)
{
    rex(b, !word, src, dst, false);
    x86_u8(b, opcode);
    modrm_register(b, src, dst);
}
void x86_imm(struct x86_buffer *b, unsigned group, unsigned r, int32_t imm)
{
    bool small = imm >= -128 && imm <= 127;
    /* Accumulator forms omit ModRM; the sign-extended imm8 form is still
     * shorter when available (Intel SDM Vol.2, group 1 instructions). */
    if (!small && r == X_RAX) {
        rex(b, true, 0, r, false);
        x86_u8(b, 0x05 | ((group & 7) << 3));
        x86_u32(b, imm);
        return;
    }
    rex(b, true, 0, r, false);
    x86_u8(b, small ? 0x83 : 0x81);
    modrm_register(b, group, r);
    if (small)
        x86_u8(b, imm);
    else
        x86_u32(b, imm);
}
void x86_unary(struct x86_buffer *b, unsigned group, unsigned r)
{
    rex(b, true, 0, r, false);
    x86_u8(b, 0xf7);
    modrm_register(b, group, r);
}
void x86_shift(struct x86_buffer *b, unsigned group, unsigned r, unsigned count, bool cl)
{
    bool one = !cl && count == 1;
    rex(b, true, 0, r, false);
    x86_u8(b, cl ? 0xd3 : one ? 0xd1 : 0xc1);
    modrm_register(b, group, r);
    if (!cl && !one)
        x86_u8(b, count);
}
void x86_bt(struct x86_buffer *b, unsigned group, unsigned r, unsigned bit)
{
    rex(b, true, 0, r, false);
    x86_u8(b, 0x0f);
    x86_u8(b, 0xba);
    modrm_register(b, group, r);
    x86_u8(b, bit);
}

static void vex(struct x86_buffer *b, unsigned map, unsigned pp, unsigned reg, unsigned rm,
                unsigned v)
{
    if (reg > 15 || rm > 15 || v > 15) {
        b->failed = true;
        return;
    }
    x86_u8(b, 0xc4);
    x86_u8(b, ((reg & 8) ? 0 : 0x80) | 0x40 | ((rm & 8) ? 0 : 0x20) | map);
    x86_u8(b, 0x80 | ((~v & 15) << 3) | pp);
}
void x86_shiftx(struct x86_buffer *b, unsigned group, unsigned dst, unsigned src, unsigned count)
{
    if (group != 4 && group != 5 && group != 7) {
        b->failed = true;
        return;
    }
    vex(b, 2, group == 4 ? 1 : group == 5 ? 3 : 2, dst, src, count);
    x86_u8(b, 0xf7);
    modrm_register(b, dst, src);
}
void x86_andn(struct x86_buffer *b, unsigned dst, unsigned inverted, unsigned src)
{
    vex(b, 2, 0, dst, src, inverted);
    x86_u8(b, 0xf2);
    modrm_register(b, dst, src);
}
void x86_bextr(struct x86_buffer *b, unsigned dst, unsigned src, unsigned control)
{
    vex(b, 2, 0, dst, src, control);
    x86_u8(b, 0xf7);
    modrm_register(b, dst, src);
}
void x86_lzcnt(struct x86_buffer *b, unsigned dst, unsigned src)
{
    x86_u8(b, 0xf3);
    rex(b, true, dst, src, false);
    x86_u8(b, 0x0f);
    x86_u8(b, 0xbd);
    modrm_register(b, dst, src);
}
void x86_rorx(struct x86_buffer *b, unsigned dst, unsigned src, unsigned count)
{
    vex(b, 3, 3, dst, src, 0);
    x86_u8(b, 0xf0);
    modrm_register(b, dst, src);
    x86_u8(b, count);
}
void x86_shrd(struct x86_buffer *b, unsigned dst, unsigned src, unsigned count)
{
    rex(b, true, src, dst, false);
    x86_u8(b, 0x0f);
    x86_u8(b, 0xac);
    modrm_register(b, src, dst);
    x86_u8(b, count);
}
void x86_setcc(struct x86_buffer *b, unsigned cc, unsigned r)
{
    rex(b, false, 0, r, (r & 7) >= X_RSP);
    x86_u8(b, 0x0f);
    x86_u8(b, 0x90 + cc);
    modrm_register(b, 0, r);
}
void x86_call(struct x86_buffer *b, uintptr_t fn)
{
    x86_movabs(b, X_R11, fn);
    x86_u8(b, 0x41);
    x86_u8(b, 0xff);
    x86_u8(b, 0xd3);
}
size_t x86_jcc(struct x86_buffer *b, unsigned cc)
{
    x86_u8(b, 0x0f);
    x86_u8(b, 0x80 + cc);
    size_t p = b->size;
    x86_u32(b, 0);
    return p;
}
size_t x86_jmp(struct x86_buffer *b)
{
    x86_u8(b, 0xe9);
    size_t p = b->size;
    x86_u32(b, 0);
    return p;
}
void x86_patch(struct x86_buffer *b, size_t p, size_t target)
{
    if (p > b->size || b->size - p < 4 || target > b->size || b->size > INT32_MAX) {
        b->failed = true;
        return;
    }
    uint32_t d = (uint32_t)(target - (p + 4));
    if (b->data)
        for (unsigned i = 0; i < 4; ++i)
            b->data[p + i] = d >> (i * 8);
}

void x86_cmp_imm(struct x86_buffer *b, unsigned r, int32_t imm, bool word)
{
    bool small = imm >= -128 && imm <= 127;
    rex(b, !word, 0, r, false);
    if (!small && r == X_RAX)
        x86_u8(b, 0x3d);
    else {
        x86_u8(b, small ? 0x83 : 0x81);
        x86_u8(b, 0xf8 | (r & 7));
    }
    if (small)
        x86_u8(b, imm);
    else
        x86_u32(b, imm);
}
