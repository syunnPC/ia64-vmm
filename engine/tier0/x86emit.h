/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_X86EMIT_H
#define IA64VMM_X86EMIT_H
#include "ia64vmm/types.h"

enum x86_reg {
    X_RAX,
    X_RCX,
    X_RDX,
    X_RBX,
    X_RSP,
    X_RBP,
    X_RSI,
    X_RDI,
    X_R8,
    X_R9,
    X_R10,
    X_R11,
    X_R12,
    X_R13,
    X_R14,
    X_R15
};
enum x86_cc {
    X_B = 2,
    X_AE = 3,
    X_E = 4,
    X_NE = 5,
    X_BE = 6,
    X_A = 7,
    X_L = 12,
    X_GE = 13,
    X_LE = 14,
    X_G = 15
};
/* data NULL: a measuring pass (size only). Full capacity sets failed. */
struct x86_buffer {
    uint8_t *data;
    size_t size, capacity;
    bool failed;
};
void x86_bytes_slow(struct x86_buffer *b, uint64_t v, unsigned n);
static inline void x86_u8(struct x86_buffer *b, uint8_t v)
{
    if (b->data && b->size < b->capacity)
        b->data[b->size++] = v;
    else
        x86_bytes_slow(b, v, 1);
}
static inline void x86_u32(struct x86_buffer *b, uint32_t v)
{
    if (b->data && b->capacity - b->size >= 4 && b->size <= b->capacity) {
        __builtin_memcpy(b->data + b->size, &v, 4);
        b->size += 4;
    } else
        x86_bytes_slow(b, v, 4);
}
static inline void x86_u64(struct x86_buffer *b, uint64_t v)
{
    if (b->data && b->capacity - b->size >= 8 && b->size <= b->capacity) {
        __builtin_memcpy(b->data + b->size, &v, 8);
        b->size += 8;
    } else
        x86_bytes_slow(b, v, 8);
}
void x86_push(struct x86_buffer *b, unsigned r);
void x86_pop(struct x86_buffer *b, unsigned r);
void x86_mov_imm(struct x86_buffer *b, unsigned r, uint64_t v);
/* Fixed width for host pointers: code size independent of the address. */
void x86_movabs(struct x86_buffer *b, unsigned r, uint64_t v);
void x86_mov(struct x86_buffer *b, unsigned dst, unsigned src);
void x86_load(struct x86_buffer *b, unsigned dst, unsigned base, int32_t disp, unsigned size);
/* Sign-extend an 8-, 16- or 32-bit memory operand to a 64-bit register. */
void x86_load_signed(struct x86_buffer *b, unsigned dst, unsigned base, int32_t disp,
                     unsigned size);
void x86_store(struct x86_buffer *b, unsigned base, int32_t disp, unsigned src, unsigned size);
void x86_load_index(struct x86_buffer *b, unsigned dst, unsigned base, unsigned index,
                    unsigned scale, int32_t disp, unsigned size);
void x86_load_index_signed(struct x86_buffer *b, unsigned dst, unsigned base, unsigned index,
                           unsigned scale, int32_t disp, unsigned size);
void x86_lea(struct x86_buffer *b, unsigned dst, unsigned base, unsigned index, unsigned scale);
void x86_store_index(struct x86_buffer *b, unsigned base, unsigned index, unsigned scale,
                     int32_t disp, unsigned src, unsigned size);
void x86_add_memory(struct x86_buffer *b, unsigned base, int32_t disp, int8_t value);
void x86_add_store(struct x86_buffer *b, unsigned base, int32_t disp, unsigned reg);
/* cmp/add reg, qword [base + disp]; test/cmp byte [base + disp], imm8. */
void x86_cmp_load(struct x86_buffer *b, unsigned reg, unsigned base, int32_t disp);
void x86_add_load(struct x86_buffer *b, unsigned reg, unsigned base, int32_t disp);
void x86_test_byte(struct x86_buffer *b, unsigned base, int32_t disp, uint8_t imm);
void x86_cmp_byte(struct x86_buffer *b, unsigned base, int32_t disp, uint8_t imm);
void x86_sfence(struct x86_buffer *b);
void x86_mfence(struct x86_buffer *b);
/* Opcodes: add=01, or=09, and=21, sub=29, xor=31, cmp=39, test=85. */
void x86_alu(struct x86_buffer *b, unsigned opcode, unsigned dst, unsigned src, bool word);
void x86_imm(struct x86_buffer *b, unsigned group, unsigned r, int32_t imm);
/* CMP reg, imm8/imm32; word selects 32-bit, otherwise 64-bit sign-extension. */
void x86_cmp_imm(struct x86_buffer *b, unsigned r, int32_t imm, bool word);
void x86_unary(struct x86_buffer *b, unsigned group, unsigned r);
void x86_shift(struct x86_buffer *b, unsigned group, unsigned r, unsigned count, bool cl);
/* Scalar VEX.W1/L=0 forms (Intel SDM Vol.2); caller gates by hal_host_isa. */
void x86_shiftx(struct x86_buffer *b, unsigned group, unsigned dst, unsigned src, unsigned count);
void x86_andn(struct x86_buffer *b, unsigned dst, unsigned inverted, unsigned src);
void x86_bextr(struct x86_buffer *b, unsigned dst, unsigned src, unsigned control);
void x86_lzcnt(struct x86_buffer *b, unsigned dst, unsigned src);
void x86_rorx(struct x86_buffer *b, unsigned dst, unsigned src, unsigned count);
/* SUB reg, [base + disp] (64-bit); BT rm, bit (64-bit, bit mod 64). */
void x86_sub_load(struct x86_buffer *b, unsigned reg, unsigned base, int32_t disp);
void x86_bt_reg(struct x86_buffer *b, unsigned rm, unsigned bit);
/* SHRD dst, src, count: dst = low 64 bits of (src:dst) >> count. */
void x86_shrd(struct x86_buffer *b, unsigned dst, unsigned src, unsigned count);
void x86_bt(struct x86_buffer *b, unsigned group, unsigned r, unsigned bit);
void x86_setcc(struct x86_buffer *b, unsigned cc, unsigned r);
void x86_call(struct x86_buffer *b, uintptr_t fn);
size_t x86_jcc(struct x86_buffer *b, unsigned cc);
size_t x86_jmp(struct x86_buffer *b);
void x86_patch(struct x86_buffer *b, size_t fixup, size_t target);
#endif
