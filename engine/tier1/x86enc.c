/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tier1's AMD64 encoder, operand/def/flags table and length decoder.
 * Encodings: Intel SDM Vol.2 (ModRM/SIB
 * Tables 2-2/2-3, REX §2.2.1) for the instructions of T1_X86_OPS only.
 *
 * Register operands of 8-bit instructions always carry a REX prefix when
 * they name register 4..7, so they mean SPL/BPL/SIL/DIL, never AH..BH
 * (x86enc.h). Memory operands are base + index * 2^scale + disp32 with
 * the shortest displacement; RIP-relative addressing is never produced.
 * The length decoder accepts exactly what t1_x86_emit produces: it
 * decodes, re-encodes and compares; and the SSE forms of x86sse.h
 * (t1_sse_decode, op T1_X86_SSE).
 */
#include "engine/tier1/x86enc.h"
#include "engine/tier1/x86sse.h"
#include "ia64vmm/string.h"

/* ---- the table ---- */

#define T1_X86_INFO_X(id, name_, n, o0, o1, iu, idf, fr, fd, fu, at)                               \
    [T1_X86_##id] = {name_, n, {o0, o1}, T1_CC_COUNT, iu, idf, fr, fd, fu, at},
#define T1_X86_INFO_CC(id, cc, n, o0, o1, iu, idf, fd, fu, at, text)                               \
    [T1_X86_##id + T1_CC_##                                                                        \
        cc] = {text, n, {o0, o1}, T1_CC_##cc, iu, idf, T1_X86_CC_READS(T1_CC_##cc), fd, fu, at},
/* The three XCC families of T1_X86_OPS with their expanded names; the
 * unit test checks each name against its row's "<cc>" pattern. */
#define T1_X86_CC_FAMILY(id, n, o0, o1, iu, idf, fd, fu, at, pre, post)                            \
    T1_X86_INFO_CC(id, O, n, o0, o1, iu, idf, fd, fu, at, pre "o" post)                            \
    T1_X86_INFO_CC(id, NO, n, o0, o1, iu, idf, fd, fu, at, pre "no" post)                          \
    T1_X86_INFO_CC(id, B, n, o0, o1, iu, idf, fd, fu, at, pre "b" post)                            \
    T1_X86_INFO_CC(id, AE, n, o0, o1, iu, idf, fd, fu, at, pre "ae" post)                          \
    T1_X86_INFO_CC(id, E, n, o0, o1, iu, idf, fd, fu, at, pre "e" post)                            \
    T1_X86_INFO_CC(id, NE, n, o0, o1, iu, idf, fd, fu, at, pre "ne" post)                          \
    T1_X86_INFO_CC(id, BE, n, o0, o1, iu, idf, fd, fu, at, pre "be" post)                          \
    T1_X86_INFO_CC(id, A, n, o0, o1, iu, idf, fd, fu, at, pre "a" post)                            \
    T1_X86_INFO_CC(id, S, n, o0, o1, iu, idf, fd, fu, at, pre "s" post)                            \
    T1_X86_INFO_CC(id, NS, n, o0, o1, iu, idf, fd, fu, at, pre "ns" post)                          \
    T1_X86_INFO_CC(id, P, n, o0, o1, iu, idf, fd, fu, at, pre "p" post)                            \
    T1_X86_INFO_CC(id, NP, n, o0, o1, iu, idf, fd, fu, at, pre "np" post)                          \
    T1_X86_INFO_CC(id, L, n, o0, o1, iu, idf, fd, fu, at, pre "l" post)                            \
    T1_X86_INFO_CC(id, GE, n, o0, o1, iu, idf, fd, fu, at, pre "ge" post)                          \
    T1_X86_INFO_CC(id, LE, n, o0, o1, iu, idf, fd, fu, at, pre "le" post)                          \
    T1_X86_INFO_CC(id, G, n, o0, o1, iu, idf, fd, fu, at, pre "g" post)
#define CC_PRE_CMOV64 "cmov"
#define CC_POST_CMOV64 "64"
#define CC_PRE_SET8 "set"
#define CC_POST_SET8 "8"
#define CC_PRE_JCC "j"
#define CC_POST_JCC ""
#define T1_X86_INFO_XCC(id, pattern, n, o0, o1, iu, idf, fd, fu, at)                               \
    T1_X86_CC_FAMILY(id, n, o0, o1, iu, idf, fd, fu, at, CC_PRE_##id, CC_POST_##id)

static const struct t1_x86_info x86_info[T1_X86_OP_COUNT] = {
    T1_X86_OPS(T1_X86_INFO_X, T1_X86_INFO_XCC)};

const struct t1_x86_info *t1_x86_info(unsigned op)
{
    static const struct t1_x86_info sse = {
        "sse",      0,         {T1_XO_NONE, T1_XO_NONE}, T1_CC_COUNT, 0, 0, T1_XF_NONE, T1_XF_NONE,
        T1_XF_NONE, T1_XA_NONE};
    return op < T1_X86_OP_COUNT ? &x86_info[op] : op == T1_X86_SSE ? &sse : NULL;
}

unsigned t1_x86_required_isa(unsigned op)
{
    switch (op) {
#define T1_X86_ISA_CASE(unused, id, isa)                                                           \
    case T1_X86_##id:                                                                              \
        return isa;
        T1_X86_SCALAR_ISA(T1_X86_ISA_CASE, op)
#undef T1_X86_ISA_CASE
    default:
        return 0;
    }
}

/* ---- encoding ---- */

struct enc {
    uint8_t bytes[16];
    unsigned n;
    bool bad;
};

static void put(struct enc *e, uint8_t byte)
{
    if (e->n < sizeof(e->bytes))
        e->bytes[e->n++] = byte;
    else
        e->bad = true;
}
static void put32(struct enc *e, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        put(e, (uint8_t)(v >> (8 * i)));
}
static bool reg_ok(uint8_t r)
{
    return r < T1_X86_NREGS;
}
static bool fits8(int64_t v)
{
    return v >= INT8_MIN && v <= INT8_MAX;
}
static bool fits32(int64_t v)
{
    return v >= INT32_MIN && v <= INT32_MAX;
}

/* Intel SDM Vol.2 VEX: inverted R/B/vvvv, W=1, L=0, X unused. */
static void vex_rr(struct enc *e, unsigned map, unsigned pp, unsigned opcode, uint8_t reg,
                   uint8_t rm, uint8_t v)
{
    if (!reg_ok(reg) || !reg_ok(rm) || !reg_ok(v)) {
        e->bad = true;
        return;
    }
    put(e, 0xc4);
    put(e, (uint8_t)((reg & 8 ? 0 : 0x80) | 0x40 | (rm & 8 ? 0 : 0x20) | map));
    put(e, (uint8_t)(0x80 | ((~v & 15) << 3) | pp));
    put(e, (uint8_t)opcode);
    put(e, (uint8_t)(0xc0 | ((reg & 7) << 3) | (rm & 7)));
}

/*
 * One instruction: [66] [REX] opcode... ModRM [SIB] [disp] [imm].
 * reg: the ModRM.reg field (a register or an opcode extension); rm: a
 * register (mem == NULL) or the memory operand. byte_regs: 8-bit register
 * operands (forces REX for registers 4..7).
 */
struct form {
    bool p66, w, byte_regs;
    uint8_t opc[3], nopc;
    uint8_t reg;     /* register number or /digit */
    bool reg_is_reg; /* reg names a register (REX.R, byte REX) */
    uint8_t rm;      /* register, when mem == NULL */
    const struct t1_x86_mem *mem;
};

static void modrm(struct enc *e, const struct form *f)
{
    uint8_t rex = 0x40 | (f->w ? 8 : 0) | (f->reg_is_reg && (f->reg & 8) ? 4 : 0);
    const struct t1_x86_mem *m = f->mem;
    bool need = f->w || (f->reg_is_reg && (f->reg & 8));
    if (m) {
        if (m->base != T1_X86_NOREG && (m->base & 8)) {
            rex |= 1;
            need = true;
        }
        if (m->index != T1_X86_NOREG && (m->index & 8)) {
            rex |= 2;
            need = true;
        }
    } else {
        if (f->rm & 8) {
            rex |= 1;
            need = true;
        }
        if (f->byte_regs && f->rm >= 4 && f->rm <= 7)
            need = true;
    }
    if (f->byte_regs && f->reg_is_reg && f->reg >= 4 && f->reg <= 7)
        need = true;
    if (f->p66)
        put(e, 0x66);
    if (need)
        put(e, rex);
    for (unsigned i = 0; i < f->nopc; ++i)
        put(e, f->opc[i]);
    uint8_t r = (uint8_t)((f->reg & 7) << 3);
    if (!m) {
        put(e, (uint8_t)(0xc0 | r | (f->rm & 7)));
        return;
    }
    if ((m->base != T1_X86_NOREG && !reg_ok(m->base)) ||
        (m->index != T1_X86_NOREG && !reg_ok(m->index)) || m->index == T1_RSP ||
        m->scale_shift > 3) {
        e->bad = true;
        return;
    }
    if (m->base == T1_X86_NOREG) {
        /* [index * scale + disp32] or [disp32]: SIB with base = 101, mod 00. */
        put(e, (uint8_t)(0x04 | r));
        uint8_t idx = m->index == T1_X86_NOREG ? 4 : (m->index & 7);
        put(e, (uint8_t)((m->index == T1_X86_NOREG ? 0 : m->scale_shift << 6) | idx << 3 | 5));
        put32(e, (uint32_t)m->disp);
        return;
    }
    unsigned mod = m->disp == 0 && (m->base & 7) != 5 ? 0 : fits8(m->disp) ? 1 : 2;
    bool sib = m->index != T1_X86_NOREG || (m->base & 7) == 4;
    put(e, (uint8_t)(mod << 6 | r | (sib ? 4 : (m->base & 7))));
    if (sib) {
        uint8_t idx = m->index == T1_X86_NOREG ? 4 : (m->index & 7);
        put(e, (uint8_t)((m->index == T1_X86_NOREG ? 0 : m->scale_shift << 6) | idx << 3 |
                         (m->base & 7)));
    }
    if (mod == 1)
        put(e, (uint8_t)m->disp);
    else if (mod == 2)
        put32(e, (uint32_t)m->disp);
}

static struct form F(bool w, uint8_t o0, uint8_t o1, unsigned nopc) {
    return (struct form){.w = w, .opc = {o0, o1, 0}, .nopc = (uint8_t)nopc};
}

/* reg <- r/m forms and r/m <- reg forms. */
static void
rr(struct enc *e, struct form f, uint8_t reg, uint8_t rm)
{
    if (!reg_ok(reg) || !reg_ok(rm)) {
        e->bad = true;
        return;
    }
    f.reg = reg;
    f.reg_is_reg = true;
    f.rm = rm;
    modrm(e, &f);
}
static void rmem(struct enc *e, struct form f, uint8_t reg, const struct t1_x86_mem *m)
{
    if (!reg_ok(reg)) {
        e->bad = true;
        return;
    }
    f.reg = reg;
    f.reg_is_reg = true;
    f.mem = m;
    modrm(e, &f);
}
static void ext_r(struct enc *e, struct form f, uint8_t digit, uint8_t rm)
{
    if (!reg_ok(rm)) {
        e->bad = true;
        return;
    }
    f.reg = digit;
    f.rm = rm;
    modrm(e, &f);
}
static void ext_m(struct enc *e, struct form f, uint8_t digit, const struct t1_x86_mem *m)
{
    f.reg = digit;
    f.mem = m;
    modrm(e, &f);
}

/* Intel SDM Vol.2 arithmetic entries: prefer 83 /digit imm8, then the
 * accumulator's opcode + imm32 (no ModRM), then 81 /digit imm32. */
static void arith_imm(struct enc *e, bool w, uint8_t digit, uint8_t rm, int64_t imm)
{
    if (!fits32(imm)) {
        e->bad = true;
        return;
    }
    bool short_imm = fits8(imm);
    if (rm == T1_RAX && !short_imm) {
        if (w)
            put(e, 0x48);
        put(e, (uint8_t)(0x05 + (digit << 3)));
    } else {
        ext_r(e, F(w, short_imm ? 0x83 : 0x81, 0, 1), digit, rm);
    }
    if (short_imm)
        put(e, (uint8_t)imm);
    else
        put32(e, (uint32_t)imm);
}

/* Encodes into e; *rel = offset of a rel32 field or -1. */
static void encode(struct enc *e, const struct t1_x86_insn *i, int *rel)
{
    const struct t1_x86_operand *a = &i->opnd[0], *b = &i->opnd[1];
    const struct t1_x86_mem *m = &i->mem;
    *rel = -1;
    unsigned op = i->op;
    if (op >= T1_X86_CMOV64 && op <= T1_X86_CMOV64_LAST) {
        rr(e, F(true, 0x0f, (uint8_t)(0x40 + op - T1_X86_CMOV64), 2), a->reg, b->reg);
        return;
    }
    if (op >= T1_X86_SET8 && op <= T1_X86_SET8_LAST) {
        struct form f = F(false, 0x0f, (uint8_t)(0x90 + op - T1_X86_SET8), 2);
        f.byte_regs = true;
        ext_r(e, f, 0, a->reg);
        return;
    }
    if (op >= T1_X86_JCC && op <= T1_X86_JCC_LAST) {
        put(e, 0x0f);
        put(e, (uint8_t)(0x80 + op - T1_X86_JCC));
        *rel = (int)e->n;
        put32(e, 0);
        return;
    }
    switch (op) {
    case T1_X86_SHLX64:
    case T1_X86_SHRX64:
    case T1_X86_SARX64:
        vex_rr(e, 2,
               op == T1_X86_SHLX64   ? 1
               : op == T1_X86_SHRX64 ? 3
                                     : 2,
               0xf7, a->reg, a->reg, b->reg);
        break;
    case T1_X86_ANDN64:
        vex_rr(e, 2, 0, 0xf2, a->reg, b->reg, a->reg);
        break;
    case T1_X86_BEXTR64:
        vex_rr(e, 2, 0, 0xf7, a->reg, a->reg, b->reg);
        break;
    case T1_X86_BLSI64:
    case T1_X86_BLSMSK64:
    case T1_X86_BLSR64:
        vex_rr(e, 2, 0, 0xf3,
               op == T1_X86_BLSI64     ? 3
               : op == T1_X86_BLSMSK64 ? 2
                                       : 1,
               b->reg, a->reg);
        break;
    case T1_X86_LZCNT64:
        put(e, 0xf3);
        rr(e, F(true, 0x0f, 0xbd, 2), a->reg, b->reg);
        break;
    case T1_X86_RORX64:
        if (b->imm < 0 || b->imm > 63) {
            e->bad = true;
            break;
        }
        vex_rr(e, 3, 3, 0xf0, a->reg, a->reg, 0);
        put(e, (uint8_t)b->imm);
        break;
    case T1_X86_MOV64_RR:
        rr(e, F(true, 0x89, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_MOV32_RR:
        rr(e, F(false, 0x89, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_MOV64_RI64:
        if (!reg_ok(a->reg)) {
            e->bad = true;
            break;
        }
        put(e, (uint8_t)(0x48 | (a->reg & 8 ? 1 : 0)));
        put(e, (uint8_t)(0xb8 + (a->reg & 7)));
        put32(e, (uint32_t)b->imm);
        put32(e, (uint32_t)((uint64_t)b->imm >> 32));
        break;
    case T1_X86_MOV64_RI32:
        if (!fits32(b->imm)) {
            e->bad = true;
            break;
        }
        ext_r(e, F(true, 0xc7, 0, 1), 0, a->reg);
        put32(e, (uint32_t)b->imm);
        break;
    case T1_X86_MOV32_RI32:
        if (!reg_ok(a->reg) || b->imm < INT32_MIN || b->imm > (int64_t)UINT32_MAX) {
            e->bad = true;
            break;
        }
        if (a->reg & 8)
            put(e, 0x41);
        put(e, (uint8_t)(0xb8 + (a->reg & 7)));
        put32(e, (uint32_t)b->imm);
        break;
    case T1_X86_LOAD64:
        rmem(e, F(true, 0x8b, 0, 1), a->reg, m);
        break;
    case T1_X86_LOAD32:
        rmem(e, F(false, 0x8b, 0, 1), a->reg, m);
        break;
    case T1_X86_LOAD16ZX:
        rmem(e, F(false, 0x0f, 0xb7, 2), a->reg, m);
        break;
    case T1_X86_LOAD8ZX:
        rmem(e, F(false, 0x0f, 0xb6, 2), a->reg, m);
        break;
    case T1_X86_STORE64:
        rmem(e, F(true, 0x89, 0, 1), b->reg, m);
        break;
    case T1_X86_STORE32:
        rmem(e, F(false, 0x89, 0, 1), b->reg, m);
        break;
    case T1_X86_STORE16: {
        struct form f = F(false, 0x89, 0, 1);
        f.p66 = true;
        rmem(e, f, b->reg, m);
        break;
    }
    case T1_X86_STORE8: {
        struct form f = F(false, 0x88, 0, 1);
        f.byte_regs = true;
        rmem(e, f, b->reg, m);
        break;
    }
    case T1_X86_STORE64_I32:
    case T1_X86_STORE32_I32:
        if (!fits32(b->imm)) {
            e->bad = true;
            break;
        }
        ext_m(e, F(op == T1_X86_STORE64_I32, 0xc7, 0, 1), 0, m);
        put32(e, (uint32_t)b->imm);
        break;
    case T1_X86_MOVZX32_8: {
        struct form f = F(false, 0x0f, 0xb6, 2);
        f.byte_regs = true;
        rr(e, f, a->reg, b->reg);
        break;
    }
    case T1_X86_MOVZX32_16:
        rr(e, F(false, 0x0f, 0xb7, 2), a->reg, b->reg);
        break;
    case T1_X86_MOVSX64_8: {
        struct form f = F(true, 0x0f, 0xbe, 2);
        f.byte_regs = true;
        rr(e, f, a->reg, b->reg);
        break;
    }
    case T1_X86_MOVSX64_16:
        rr(e, F(true, 0x0f, 0xbf, 2), a->reg, b->reg);
        break;
    case T1_X86_MOVSX64_32:
        rr(e, F(true, 0x63, 0, 1), a->reg, b->reg);
        break;
    case T1_X86_LEA64:
        rmem(e, F(true, 0x8d, 0, 1), a->reg, m);
        break;
    case T1_X86_ADD64_RR:
        rr(e, F(true, 0x01, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_SUB64_RR:
        rr(e, F(true, 0x29, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_AND64_RR:
        rr(e, F(true, 0x21, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_OR64_RR:
        rr(e, F(true, 0x09, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_XOR64_RR:
        rr(e, F(true, 0x31, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_CMP64_RR:
        rr(e, F(true, 0x39, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_CMP32_RR:
        rr(e, F(false, 0x39, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_TEST64_RR:
        rr(e, F(true, 0x85, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_TEST32_RR:
        rr(e, F(false, 0x85, 0, 1), b->reg, a->reg);
        break;
    case T1_X86_ZERO32:
        rr(e, F(false, 0x31, 0, 1), a->reg, a->reg);
        break;
    case T1_X86_MULPAIR64:
    case T1_X86_IMULPAIR64:
        /* MUL/IMUL rdx read both operands before writing RDX:RAX.
         * Only this explicit pair is exposed to selection and allocation. */
        if (a->reg != T1_RAX || b->reg != T1_RDX) {
            e->bad = true;
            break;
        }
        ext_r(e, F(true, 0xf7, 0, 1), op == T1_X86_MULPAIR64 ? 4 : 5, T1_RDX);
        break;
    case T1_X86_ADC64_RI:
        arith_imm(e, true, 2, a->reg, b->imm);
        break;
    case T1_X86_IMUL64_RR:
        rr(e, F(true, 0x0f, 0xaf, 2), a->reg, b->reg);
        break;
    case T1_X86_ADD64_RI:
        arith_imm(e, true, 0, a->reg, b->imm);
        break;
    case T1_X86_OR64_RI:
        arith_imm(e, true, 1, a->reg, b->imm);
        break;
    case T1_X86_AND64_RI:
        arith_imm(e, true, 4, a->reg, b->imm);
        break;
    case T1_X86_SUB64_RI:
        arith_imm(e, true, 5, a->reg, b->imm);
        break;
    case T1_X86_XOR64_RI:
        arith_imm(e, true, 6, a->reg, b->imm);
        break;
    case T1_X86_CMP64_RI:
        arith_imm(e, true, 7, a->reg, b->imm);
        break;
    case T1_X86_CMP32_RI:
        arith_imm(e, false, 7, a->reg, b->imm);
        break;
    case T1_X86_OR32_RM:
        rmem(e, F(false, 0x0b, 0, 1), a->reg, m);
        break;
    case T1_X86_CMP64_RM:
        rmem(e, F(true, 0x3b, 0, 1), a->reg, m);
        break;
    case T1_X86_TEST64_RI:
        if (!fits32(b->imm)) {
            e->bad = true;
            break;
        }
        /* TEST has no sign-extended imm8 form; RAX always saves ModRM. */
        if (a->reg == T1_RAX) {
            put(e, 0x48);
            put(e, 0xa9);
        } else
            ext_r(e, F(true, 0xf7, 0, 1), 0, a->reg);
        put32(e, (uint32_t)b->imm);
        break;
    case T1_X86_NOT64:
        ext_r(e, F(true, 0xf7, 0, 1), 2, a->reg);
        break;
    case T1_X86_NEG64:
        ext_r(e, F(true, 0xf7, 0, 1), 3, a->reg);
        break;
    case T1_X86_INC64_R:
        ext_r(e, F(true, 0xff, 0, 1), 0, a->reg);
        break;
    case T1_X86_INC64_M:
        ext_m(e, F(true, 0xff, 0, 1), 0, m);
        break;
    case T1_X86_LOCK_INC64_M:
        put(e, 0xf0);
        ext_m(e, F(true, 0xff, 0, 1), 0, m);
        break;
    case T1_X86_DEC64_M:
        ext_m(e, F(true, 0xff, 0, 1), 1, m);
        break;
    case T1_X86_SHL64_RI:
    case T1_X86_SHR64_RI:
    case T1_X86_SAR64_RI: {
        static const uint8_t digit[] = {4, 5, 7};
        if (b->imm < 0 || b->imm > 63) {
            e->bad = true;
            break;
        }
        /* Intel SDM Vol.2 SAL/SAR/SHL/SHR: D1 has an implicit count
         * of one, with the same result and flags as C1 /digit, 1. */
        ext_r(e, F(true, b->imm == 1 ? 0xd1 : 0xc1, 0, 1), digit[op - T1_X86_SHL64_RI], a->reg);
        if (b->imm != 1)
            put(e, (uint8_t)b->imm);
        break;
    }
    case T1_X86_SHL64_RCL:
    case T1_X86_SHR64_RCL:
    case T1_X86_SAR64_RCL: {
        static const uint8_t digit[] = {4, 5, 7};
        if (b->reg != T1_RCX) {
            e->bad = true;
            break;
        }
        ext_r(e, F(true, 0xd3, 0, 1), digit[op - T1_X86_SHL64_RCL], a->reg);
        break;
    }
    case T1_X86_JMP:
        put(e, 0xe9);
        *rel = (int)e->n;
        put32(e, 0);
        break;
    case T1_X86_JMP_M:
        ext_m(e, F(false, 0xff, 0, 1), 4, m);
        break;
    case T1_X86_CALL_R:
        ext_r(e, F(false, 0xff, 0, 1), 2, a->reg);
        break;
    case T1_X86_CALL_REL:
        put(e, 0xe8);
        *rel = (int)e->n;
        put32(e, 0);
        break;
    case T1_X86_RET:
        put(e, 0xc3);
        break;
    case T1_X86_PUSH64_R:
    case T1_X86_POP64_R:
        if (!reg_ok(a->reg)) {
            e->bad = true;
            break;
        }
        if (a->reg & 8)
            put(e, 0x41);
        put(e, (uint8_t)((op == T1_X86_PUSH64_R ? 0x50 : 0x58) + (a->reg & 7)));
        break;
    case T1_X86_PUSH64_M:
        ext_m(e, F(false, 0xff, 0, 1), 6, m);
        break;
    case T1_X86_POPFQ:
        put(e, 0x9d);
        break;
    case T1_X86_MFENCE:
        put(e, 0x0f);
        put(e, 0xae);
        put(e, 0xf0);
        break;
    default:
        e->bad = true;
        break;
    }
}

void t1_x86_emit(struct t1_x86_buf *b, const struct t1_x86_insn *i, size_t *fixup)
{
    struct enc e = {{0}, 0, false};
    int rel;
    if (fixup)
        *fixup = SIZE_MAX;
    encode(&e, i, &rel);
    if (e.bad || b->failed || e.n > b->capacity - b->size) {
        b->failed = true;
        return;
    }
    memcpy(b->data + b->size, e.bytes, e.n);
    if (rel >= 0 && fixup)
        *fixup = b->size + (size_t)rel;
    b->size += e.n;
}

void t1_x86_patch_rel32(struct t1_x86_buf *b, size_t fixup, size_t target)
{
    if (fixup == SIZE_MAX || fixup + 4 > b->size) {
        b->failed = true;
        return;
    }
    int64_t rel = (int64_t)target - (int64_t)(fixup + 4);
    if (!fits32(rel)) {
        b->failed = true;
        return;
    }
    for (unsigned i = 0; i < 4; ++i)
        b->data[fixup + i] = (uint8_t)((uint64_t)rel >> (8 * i));
}

/* ---- length decoder ---- */

struct parsed {
    bool p66, w;
    uint8_t rex, opc[2], nopc;
    bool has_modrm, is_reg;
    uint8_t reg, rm; /* full register numbers (REX applied) */
    struct t1_x86_mem mem;
};

static bool parse_modrm(const uint8_t *c, size_t avail, size_t *at, struct parsed *p)
{
    if (*at >= avail)
        return false;
    uint8_t mr = c[(*at)++];
    unsigned mod = mr >> 6, r = (mr >> 3) & 7, rm = mr & 7;
    p->has_modrm = true;
    p->reg = (uint8_t)(r | (p->rex & 4 ? 8 : 0));
    if (mod == 3) {
        p->is_reg = true;
        p->rm = (uint8_t)(rm | (p->rex & 1 ? 8 : 0));
        return true;
    }
    p->mem = (struct t1_x86_mem){T1_X86_NOREG, T1_X86_NOREG, 0, 0};
    if (rm == 4) {
        if (*at >= avail)
            return false;
        uint8_t sib = c[(*at)++];
        unsigned idx = ((sib >> 3) & 7) | (p->rex & 2 ? 8 : 0), base = sib & 7;
        if (idx != 4) {
            p->mem.index = (uint8_t)idx;
            p->mem.scale_shift = sib >> 6;
        }
        if (base == 5 && mod == 0) { /* no base: disp32 follows */
        } else
            p->mem.base = (uint8_t)(base | (p->rex & 1 ? 8 : 0));
        if (base == 5 && mod == 0)
            mod = 2;
    } else if (rm == 5 && mod == 0) {
        return false; /* RIP-relative: never emitted */
    } else {
        p->mem.base = (uint8_t)(rm | (p->rex & 1 ? 8 : 0));
    }
    if (mod == 1) {
        if (*at >= avail)
            return false;
        p->mem.disp = (int8_t)c[(*at)++];
    } else if (mod == 2) {
        if (*at + 4 > avail)
            return false;
        uint32_t d = 0;
        for (unsigned i = 0; i < 4; ++i)
            d |= (uint32_t)c[*at + i] << (8 * i);
        p->mem.disp = (int32_t)d;
        *at += 4;
    }
    return true;
}

static int64_t read_imm(const uint8_t *c, size_t avail, size_t *at, unsigned bytes, bool *ok)
{
    if (*at + bytes > avail) {
        *ok = false;
        return 0;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i)
        v |= (uint64_t)c[*at + i] << (8 * i);
    *at += bytes;
    if (bytes == 1)
        return (int8_t)v;
    if (bytes == 4)
        return (int32_t)v;
    return (int64_t)v;
}

/* Decodes one instruction into an insn (REL operands as 0) and its op. */
static bool decode_insn(const uint8_t *c, size_t avail, struct t1_x86_insn *out, size_t *len)
{
    struct parsed p = {0};
    size_t at = 0;
    bool ok = true;
    bool lock = at < avail && c[at] == 0xf0;
    if (lock)
        ++at;
    bool pf3 = at < avail && c[at] == 0xf3;
    if (pf3)
        ++at;
    if (at < avail && c[at] == 0x66) {
        p.p66 = true;
        ++at;
    }
    if (at < avail && (c[at] & 0xf0) == 0x40) {
        p.rex = c[at++];
        p.w = p.rex & 8;
    }
    if (at >= avail)
        return false;
    uint8_t o = c[at++];
    /* Only the emitted 64-bit LZCNT form owns a legacy F3 prefix. */
    if (pf3 && (p.p66 || !p.w || o != 0x0f || at >= avail || c[at] != 0xbd))
        return false;
    /* Only LOCK REX.W FF /0 with a memory destination is emitted. */
    if (lock && (pf3 || p.p66 || !p.w || o != 0xff))
        return false;
    struct t1_x86_insn i = {.op = UINT16_MAX};
    i.mem = (struct t1_x86_mem){T1_X86_NOREG, T1_X86_NOREG, 0, 0};
#define MODRM()                                                                                    \
    do {                                                                                           \
        if (!parse_modrm(c, avail, &at, &p))                                                       \
            return false;                                                                          \
    } while (0)
    switch (o) {
    case 0xc4: {
        if (at + 3 > avail)
            return false;
        uint8_t v2 = c[at++], v3 = c[at++], opcode = c[at++];
        if (!(v2 & 0x40) || !(v3 & 0x80) || (v3 & 4))
            return false;
        p.rex = (uint8_t)(((~v2 >> 7) & 1) << 2 | ((~v2 >> 5) & 1));
        MODRM();
        if (!p.is_reg)
            return false;
        unsigned v = (~v3 >> 3) & 15, pp = v3 & 3, map = v2 & 31;
        if (map == 2 && opcode == 0xf7) {
            i.op = pp == 0   ? T1_X86_BEXTR64
                   : pp == 1 ? T1_X86_SHLX64
                   : pp == 2 ? T1_X86_SARX64
                             : T1_X86_SHRX64;
            if (p.reg != p.rm)
                return false;
            i.opnd[0].reg = p.reg;
            i.opnd[1].reg = (uint8_t)v;
        } else if (map == 2 && pp == 0 && opcode == 0xf2) {
            if (v != p.reg)
                return false;
            i.op = T1_X86_ANDN64;
            i.opnd[0].reg = p.reg;
            i.opnd[1].reg = p.rm;
        } else if (map == 2 && pp == 0 && opcode == 0xf3) {
            if (p.reg < 1 || p.reg > 3)
                return false;
            i.op = p.reg == 3 ? T1_X86_BLSI64 : p.reg == 2 ? T1_X86_BLSMSK64 : T1_X86_BLSR64;
            i.opnd[0].reg = (uint8_t)v;
            i.opnd[1].reg = p.rm;
        } else if (map == 3 && pp == 3 && opcode == 0xf0) {
            if (p.reg != p.rm || v)
                return false;
            i.op = T1_X86_RORX64;
            i.opnd[0].reg = p.reg;
            i.opnd[1].imm = read_imm(c, avail, &at, 1, &ok) & 255;
        } else
            return false;
        break;
    }
    case 0x0f: {
        if (at >= avail)
            return false;
        uint8_t o2 = c[at++];
        if (o2 >= 0x40 && o2 <= 0x4f) {
            MODRM();
            i.op = (uint16_t)(T1_X86_CMOV64 + (o2 - 0x40));
            i.opnd[0].reg = p.reg;
            i.opnd[1].reg = p.rm;
            break;
        }
        if (o2 >= 0x90 && o2 <= 0x9f) {
            MODRM();
            i.op = (uint16_t)(T1_X86_SET8 + (o2 - 0x90));
            i.opnd[0].reg = p.rm;
            break;
        }
        if (o2 >= 0x80 && o2 <= 0x8f) {
            i.op = (uint16_t)(T1_X86_JCC + (o2 - 0x80));
            read_imm(c, avail, &at, 4, &ok);
            break;
        }
        if (o2 == 0xae) {
            if (at >= avail || c[at++] != 0xf0)
                return false;
            i.op = T1_X86_MFENCE;
            break;
        }
        MODRM();
        switch (o2) {
        case 0xbd:
            if (!pf3 || !p.w || !p.is_reg)
                return false;
            i.op = T1_X86_LZCNT64;
            break;
        case 0xaf:
            i.op = T1_X86_IMUL64_RR;
            break;
        case 0xb6:
            i.op = p.is_reg ? T1_X86_MOVZX32_8 : T1_X86_LOAD8ZX;
            break;
        case 0xb7:
            i.op = p.is_reg ? T1_X86_MOVZX32_16 : T1_X86_LOAD16ZX;
            break;
        case 0xbe:
            i.op = T1_X86_MOVSX64_8;
            break;
        case 0xbf:
            i.op = T1_X86_MOVSX64_16;
            break;
        default:
            return false;
        }
        i.opnd[0].reg = p.reg;
        i.opnd[1].reg = p.rm;
        i.mem = p.mem;
        break;
    }
    case 0x89:
    case 0x88:
        MODRM();
        i.op = o == 0x88  ? T1_X86_STORE8
               : p.is_reg ? (p.w ? T1_X86_MOV64_RR : T1_X86_MOV32_RR)
               : p.p66    ? T1_X86_STORE16
               : p.w      ? T1_X86_STORE64
                          : T1_X86_STORE32;
        if (p.is_reg) {
            i.opnd[0].reg = p.rm;
            i.opnd[1].reg = p.reg;
        } else {
            i.opnd[1].reg = p.reg;
            i.mem = p.mem;
        }
        break;
    case 0x8b:
    case 0x8d:
    case 0x0b:
    case 0x3b:
    case 0x63:
        MODRM();
        i.op = o == 0x8b   ? (p.w ? T1_X86_LOAD64 : T1_X86_LOAD32)
               : o == 0x8d ? T1_X86_LEA64
               : o == 0x0b ? T1_X86_OR32_RM
               : o == 0x3b ? T1_X86_CMP64_RM
                           : T1_X86_MOVSX64_32;
        i.opnd[0].reg = p.reg;
        i.opnd[1].reg = p.rm;
        i.mem = p.mem;
        break;
    case 0x01:
    case 0x29:
    case 0x21:
    case 0x09:
    case 0x31:
    case 0x39:
    case 0x85:
        MODRM();
        i.op = o == 0x01   ? T1_X86_ADD64_RR
               : o == 0x29 ? T1_X86_SUB64_RR
               : o == 0x21 ? T1_X86_AND64_RR
               : o == 0x09 ? T1_X86_OR64_RR
               : o == 0x31 ? (p.w ? T1_X86_XOR64_RR : T1_X86_ZERO32)
               : o == 0x39 ? (p.w ? T1_X86_CMP64_RR : T1_X86_CMP32_RR)
                           : (p.w ? T1_X86_TEST64_RR : T1_X86_TEST32_RR);
        i.opnd[0].reg = p.rm;
        i.opnd[1].reg = p.reg;
        break;
    case 0x05:
    case 0x0d:
    case 0x15:
    case 0x25:
    case 0x2d:
    case 0x35:
    case 0x3d:
    case 0x81:
    case 0x83: {
        unsigned digit;
        if (o == 0x81 || o == 0x83) {
            MODRM();
            digit = p.reg & 7;
            i.opnd[0].reg = p.rm;
        } else {
            digit = o >> 3;
            i.opnd[0].reg = T1_RAX;
        }
        static const uint16_t by_digit[8] = {T1_X86_ADD64_RI, T1_X86_OR64_RI,  T1_X86_ADC64_RI,
                                             UINT16_MAX,      T1_X86_AND64_RI, T1_X86_SUB64_RI,
                                             T1_X86_XOR64_RI, T1_X86_CMP64_RI};
        i.op = !p.w && digit == 7 ? T1_X86_CMP32_RI : by_digit[digit];
        i.opnd[1].imm = read_imm(c, avail, &at, o == 0x83 ? 1 : 4, &ok);
        break;
    }
    case 0xa9:
        i.op = T1_X86_TEST64_RI;
        i.opnd[0].reg = T1_RAX;
        i.opnd[1].imm = read_imm(c, avail, &at, 4, &ok);
        break;
    case 0xc7:
        MODRM();
        i.op = p.is_reg ? T1_X86_MOV64_RI32 : p.w ? T1_X86_STORE64_I32 : T1_X86_STORE32_I32;
        i.opnd[0].reg = p.rm;
        i.mem = p.mem;
        i.opnd[1].imm = read_imm(c, avail, &at, 4, &ok);
        break;
    case 0xf7:
        MODRM();
        i.op = (p.reg & 7) == 0   ? T1_X86_TEST64_RI
               : (p.reg & 7) == 2 ? T1_X86_NOT64
               : (p.reg & 7) == 3 ? T1_X86_NEG64
                                  : UINT16_MAX;
        if ((p.reg & 7) == 4 || (p.reg & 7) == 5) {
            if (!p.is_reg || p.rm != T1_RDX)
                return false;
            i.op = (p.reg & 7) == 4 ? T1_X86_MULPAIR64 : T1_X86_IMULPAIR64;
            i.opnd[0].reg = T1_RAX;
            i.opnd[1].reg = T1_RDX;
        } else
            i.opnd[0].reg = p.rm;
        if (i.op == T1_X86_TEST64_RI)
            i.opnd[1].imm = read_imm(c, avail, &at, 4, &ok);
        break;
    case 0xff:
        MODRM();
        if (lock && (p.is_reg || p.reg != 0))
            return false;
        switch (p.reg & 7) {
        case 0:
            i.op = lock ? T1_X86_LOCK_INC64_M : p.is_reg ? T1_X86_INC64_R : T1_X86_INC64_M;
            break;
        case 1:
            i.op = T1_X86_DEC64_M;
            break;
        case 2:
            i.op = T1_X86_CALL_R;
            break;
        case 4:
            i.op = T1_X86_JMP_M;
            break;
        case 6:
            i.op = T1_X86_PUSH64_M;
            break;
        default:
            return false;
        }
        i.opnd[0].reg = p.rm;
        i.mem = p.mem;
        break;
    case 0xc1:
    case 0xd1:
    case 0xd3: {
        MODRM();
        unsigned d = p.reg & 7;
        unsigned k = d == 4 ? 0 : d == 5 ? 1 : d == 7 ? 2 : 3;
        if (k == 3)
            return false;
        i.op = (uint16_t)((o == 0xd3 ? T1_X86_SHL64_RCL : T1_X86_SHL64_RI) + k);
        i.opnd[0].reg = p.rm;
        if (o == 0xc1)
            i.opnd[1].imm = read_imm(c, avail, &at, 1, &ok) & 0xff;
        else if (o == 0xd1)
            i.opnd[1].imm = 1;
        else
            i.opnd[1].reg = T1_RCX;
        break;
    }
    case 0xe8:
        i.op = T1_X86_CALL_REL;
        read_imm(c, avail, &at, 4, &ok);
        break;
    case 0xe9:
        i.op = T1_X86_JMP;
        read_imm(c, avail, &at, 4, &ok);
        break;
    case 0xc3:
        i.op = T1_X86_RET;
        break;
    case 0x9d:
        i.op = T1_X86_POPFQ;
        break;
    default:
        if (o >= 0xb8 && o <= 0xbf) {
            i.op = p.w ? T1_X86_MOV64_RI64 : T1_X86_MOV32_RI32;
            i.opnd[0].reg = (uint8_t)((o - 0xb8) | (p.rex & 1 ? 8 : 0));
            i.opnd[1].imm = read_imm(c, avail, &at, p.w ? 8 : 4, &ok);
            if (!p.w)
                i.opnd[1].imm &= 0xffffffff;
        } else if (o >= 0x50 && o <= 0x5f) {
            i.op = o < 0x58 ? T1_X86_PUSH64_R : T1_X86_POP64_R;
            i.opnd[0].reg = (uint8_t)((o & 7) | (p.rex & 1 ? 8 : 0));
        } else {
            return false;
        }
    }
#undef MODRM
    if (!ok || i.op == UINT16_MAX)
        return false;
    *out = i;
    *len = at;
    return true;
}

bool t1_x86_decode(const uint8_t *code, size_t avail, struct t1_x86_decoded *out)
{
    struct t1_x86_insn i;
    size_t len;
    if (t1_sse_decode(code, avail, out))
        return true;
    if (!decode_insn(code, avail, &i, &len))
        return false;
    /* Accept exactly the canonical encoding: re-encode and compare (the
     * rel32 of a branch is a label, not part of the comparison). */
    struct enc e = {{0}, 0, false};
    int rel;
    encode(&e, &i, &rel);
    if (e.bad || e.n != len)
        return false;
    for (size_t k = 0; k < len; ++k)
        if ((rel < 0 || k < (size_t)rel || k >= (size_t)rel + 4) && e.bytes[k] != code[k])
            return false;
    const struct t1_x86_info *info = &x86_info[i.op];
    bool mem = false;
    for (unsigned k = 0; k < info->nopnd; ++k)
        mem |= info->opnd[k] >= T1_XO_M8_R && info->opnd[k] <= T1_XO_MA;
    *out = (struct t1_x86_decoded){
        .op = i.op,
        .length = (uint8_t)len,
        .has_mem = mem,
        .mem = mem ? i.mem : (struct t1_x86_mem){T1_X86_NOREG, T1_X86_NOREG, 0, 0}};
    return true;
}
