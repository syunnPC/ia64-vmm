/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Encodings of x86sse.h (Intel SDM Vol.2 opcode maps): legacy SSE with a
 * 66/F2/F3 prefix, REX.W for MOVQ with R11, and three-byte VEX for FMA3.
 * Registers: xmm0-xmm7, R11 for the MOVQ forms, R14 for the store base. */
#include "engine/tier1/x86sse.h"

struct sse_enc {
    uint8_t bytes[12];
    unsigned n;
};
static void byte(struct sse_enc *e, unsigned v)
{
    e->bytes[e->n++] = (uint8_t)v;
}
static unsigned modrm_rr(unsigned reg, unsigned rm)
{
    return 0xc0u | (reg & 7) << 3 | (rm & 7);
}

static bool encode_rr(struct sse_enc *e, enum t1_sse_op op, unsigned dst, unsigned src)
{
    if (dst > 7 || src > 7)
        return false;
    switch (op) {
    case T1_SSE_MOVQ_XR: /* 66 REX.W+B 0F 6E /r: movq xmm, r11 */
        byte(e, 0x66);
        byte(e, 0x49);
        byte(e, 0x0f);
        byte(e, 0x6e);
        byte(e, modrm_rr(dst, T1_R11));
        return true;
    case T1_SSE_MOVQ_RX: /* 66 REX.W+B 0F 7E /r: movq r11, xmm */
        byte(e, 0x66);
        byte(e, 0x49);
        byte(e, 0x0f);
        byte(e, 0x7e);
        byte(e, modrm_rr(src, T1_R11));
        return true;
    case T1_SSE_POR:
        byte(e, 0x66);
        byte(e, 0x0f);
        byte(e, 0xeb);
        break;
    case T1_SSE_PCMPEQD:
        byte(e, 0x66);
        byte(e, 0x0f);
        byte(e, 0x76);
        break;
    case T1_SSE_ADDSD:
        byte(e, 0xf2);
        byte(e, 0x0f);
        byte(e, 0x58);
        break;
    case T1_SSE_MULSD:
        byte(e, 0xf2);
        byte(e, 0x0f);
        byte(e, 0x59);
        break;
    case T1_SSE_ADDSS:
        byte(e, 0xf3);
        byte(e, 0x0f);
        byte(e, 0x58);
        break;
    case T1_SSE_MULSS:
        byte(e, 0xf3);
        byte(e, 0x0f);
        byte(e, 0x59);
        break;
    case T1_SSE_PXOR:
        byte(e, 0x66);
        byte(e, 0x0f);
        byte(e, 0xef);
        break;
    case T1_SSE_MULPS:
        byte(e, 0x0f);
        byte(e, 0x59);
        break;
    default:
        return false;
    }
    byte(e, modrm_rr(dst, src));
    return true;
}

/* C4 [~R ~X ~B 00010] [W ~vvvv L=0 pp=01] A9 (scalar) / A8 (packed) /r */
static bool encode_fma(struct sse_enc *e, enum t1_sse_op op, unsigned dst, unsigned src2,
                       unsigned src3)
{
    if ((op != T1_SSE_FMADD213SD && op != T1_SSE_FMADD213SS && op != T1_SSE_FMADD213PS) ||
        dst > 7 || src2 > 7 || src3 > 7)
        return false;
    byte(e, 0xc4);
    byte(e, 0xe2);
    byte(e, (op == T1_SSE_FMADD213SD ? 0x80u : 0u) | (~src2 & 15u) << 3 | 1u);
    byte(e, op == T1_SSE_FMADD213PS ? 0xa8 : 0xa9);
    byte(e, modrm_rr(dst, src3));
    return true;
}

/* 66 0F 73 /6 ib */
static bool encode_psllq(struct sse_enc *e, unsigned xmm, unsigned count)
{
    if (xmm > 7 || count > 63)
        return false;
    byte(e, 0x66);
    byte(e, 0x0f);
    byte(e, 0x73);
    byte(e, 0xf0u | xmm);
    byte(e, count);
    return true;
}

/* 66 REX.B 0F D6 /r with mod 10, rm 110 (R14) and disp32 */
static bool encode_store(struct sse_enc *e, unsigned xmm, int32_t disp)
{
    if (xmm > 7)
        return false;
    byte(e, 0x66);
    byte(e, 0x41);
    byte(e, 0x0f);
    byte(e, 0xd6);
    byte(e, 0x80u | xmm << 3 | (T1_R14 & 7));
    for (unsigned i = 0; i < 4; ++i)
        byte(e, (uint32_t)disp >> (8 * i));
    return true;
}

static void append(struct t1_x86_buf *b, const struct sse_enc *e, bool ok)
{
    if (!ok || b->failed || e->n > b->capacity - b->size) {
        b->failed = true;
        return;
    }
    for (unsigned i = 0; i < e->n; ++i)
        b->data[b->size++] = e->bytes[i];
}

void t1_sse_rr(struct t1_x86_buf *b, enum t1_sse_op op, unsigned dst, unsigned src)
{
    struct sse_enc e = {{0}, 0};
    append(b, &e, encode_rr(&e, op, dst, src));
}
void t1_sse_fma(struct t1_x86_buf *b, enum t1_sse_op op, unsigned dst, unsigned src2, unsigned src3)
{
    struct sse_enc e = {{0}, 0};
    append(b, &e, encode_fma(&e, op, dst, src2, src3));
}
void t1_sse_psllq(struct t1_x86_buf *b, unsigned xmm, unsigned count)
{
    struct sse_enc e = {{0}, 0};
    append(b, &e, encode_psllq(&e, xmm, count));
}
void t1_sse_store(struct t1_x86_buf *b, unsigned xmm, int32_t disp)
{
    struct sse_enc e = {{0}, 0};
    append(b, &e, encode_store(&e, xmm, disp));
}

static bool same(const uint8_t *code, size_t avail, const struct sse_enc *e)
{
    if (e->n > avail)
        return false;
    for (unsigned i = 0; i < e->n; ++i)
        if (code[i] != e->bytes[i])
            return false;
    return true;
}

/* Accepts exactly the encodings above: candidate fields are read from the
 * bytes, re-encoded and compared. */
bool t1_sse_decode(const uint8_t *code, size_t avail, struct t1_x86_decoded *out)
{
    static const enum t1_sse_op rr[] = {
        T1_SSE_MOVQ_XR, T1_SSE_MOVQ_RX, T1_SSE_POR,   T1_SSE_PCMPEQD, T1_SSE_ADDSD,
        T1_SSE_MULSD,   T1_SSE_ADDSS,   T1_SSE_MULSS, T1_SSE_PXOR,    T1_SSE_MULPS};
    if (avail < 3)
        return false;
    *out = (struct t1_x86_decoded){
        .op = T1_X86_SSE, .has_mem = false, .mem = {T1_X86_NOREG, T1_X86_NOREG, 0, 0}};
    struct sse_enc e;
    for (unsigned k = 0; k < sizeof(rr) / sizeof(rr[0]); ++k) {
        unsigned at = rr[k] <= T1_SSE_MOVQ_RX ? 4 : rr[k] == T1_SSE_MULPS ? 2 : 3;
        if (avail <= at)
            continue;
        unsigned m = code[at], reg = (m >> 3) & 7, rm = m & 7;
        e = (struct sse_enc){{0}, 0};
        bool ok = rr[k] == T1_SSE_MOVQ_XR   ? encode_rr(&e, rr[k], reg, 0)
                  : rr[k] == T1_SSE_MOVQ_RX ? encode_rr(&e, rr[k], 0, reg)
                                            : encode_rr(&e, rr[k], reg, rm);
        if (ok && same(code, avail, &e)) {
            out->length = (uint8_t)e.n;
            return true;
        }
    }
    if (avail >= 5) {
        unsigned src2 = ~(code[2] >> 3) & 15, m = code[4];
        static const enum t1_sse_op fma[] = {T1_SSE_FMADD213SD, T1_SSE_FMADD213SS,
                                             T1_SSE_FMADD213PS};
        for (unsigned k = 0; k < 3; ++k) {
            e = (struct sse_enc){{0}, 0};
            if (encode_fma(&e, fma[k], (m >> 3) & 7, src2, m & 7) && same(code, avail, &e)) {
                out->length = (uint8_t)e.n;
                return true;
            }
        }
        e = (struct sse_enc){{0}, 0};
        if (encode_psllq(&e, code[3] & 7, code[4]) && same(code, avail, &e)) {
            out->length = (uint8_t)e.n;
            return true;
        }
    }
    if (avail >= 9) {
        int32_t disp = (int32_t)((uint32_t)code[5] | (uint32_t)code[6] << 8 |
                                 (uint32_t)code[7] << 16 | (uint32_t)code[8] << 24);
        e = (struct sse_enc){{0}, 0};
        if (encode_store(&e, (code[4] >> 3) & 7, disp) && same(code, avail, &e)) {
            out->length = (uint8_t)e.n;
            out->has_mem = true;
            out->mem = (struct t1_x86_mem){T1_R14, T1_X86_NOREG, 0, disp};
            return true;
        }
    }
    return false;
}
