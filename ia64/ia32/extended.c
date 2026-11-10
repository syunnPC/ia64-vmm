/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/ia32/extended.h"
#include "ia64/ia32/access.h"
#include "ia64vmm/string.h"

static uint64_t get(const uint8_t *p, unsigned bytes)
{
    uint64_t x = 0;
    for (unsigned i = 0; i < bytes; ++i)
        x |= (uint64_t)p[i] << (8 * i);
    return x;
}
static void put(uint8_t *p, uint64_t x, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = x >> (8 * i);
}
static bool failed(struct ia64_result r, const struct ia32_interruption *record)
{
    return r.kind != IA64_STEP_COMPLETE || record->valid;
}
struct ia64_result ia32_extended_check(struct vcpu *v, const struct ia32_native_state *n,
                                       const struct ia32_decoded *d,
                                       struct ia32_interruption *record)
{
    if (d->kind == IA32_INSN_INTERCEPT)
        return ia64_complete();
    unsigned op = d->opcode, group = (d->modrm >> 3) & 7;
    bool x87 = (d->flags & IA32_INSN_FP) && op >= 0xd8 && op <= 0xdf;
    bool no_wait = (op == 0xdb && (d->modrm == 0xe2 || d->modrm == 0xe3)) ||
                   (op == 0xd9 && d->operand_memory && group >= 6) ||
                   (op == 0xdd && d->operand_memory && group >= 6) ||
                   (op == 0xdf && d->modrm == 0xe0);
    bool fx = op == 0x0fae && d->operand_memory && group <= 1;
    if ((x87 || fx) && (n->cflg & 12))
        return ia32_exception(n, 7, 0, record);
    if ((d->flags & (IA32_INSN_MMX | IA32_INSN_SSE)) && (n->cflg & 8))
        return ia32_exception(n, 7, 0, record);
    if (op == 0x9b && (n->cflg & 10) == 10)
        return ia32_exception(n, 7, 0, record);
    if ((n->fp.bytes[2] & 0x80) && ((x87 && !no_wait) || op == 0x9b || (d->flags & IA32_INSN_MMX)))
        return ia32_exception(n, 16, 0, record);
    unsigned alignment = 1, size = d->access_width;
    bool write = d->access_kind == IA32_ACCESS_WRITE;
    if (fx) {
        alignment = 16;
        size = 512;
        write = group == 0;
    } else if (d->operand_memory && op == 0x0fe7)
        alignment = d->flags & IA32_INSN_SSE ? 16 : 8;
    else if (d->operand_memory && (d->flags & IA32_INSN_SSE) && size == 16) {
        bool unaligned = op == 0x0f10 || op == 0x0f11 || op == 0x0ff0 ||
                         ((op == 0x0f6f || op == 0x0f7f) && (d->intercept_code & 0x10));
        if (!unaligned)
            alignment = 16;
    }
    if (alignment > 1) {
        struct ia64_result r =
            ia32_operand_prepare(v, n, d->segment, d->offset, size, write, 1, record);
        if (failed(r, record))
            return r;
        uint32_t address = (uint32_t)ia32_segment(n, d->segment)->base + d->offset;
        if (address & (alignment - 1))
            return ia32_exception(n, 13, 0, record);
    }
    return ia64_complete();
}
static uint32_t stack_address(const struct ia32_native_state *n, uint32_t sp)
{
    return n->ss.attrib & 0x400 ? sp : (uint16_t)sp;
}
static uint32_t stack_value(const struct ia32_native_state *n, uint32_t old, uint32_t sp)
{
    return n->ss.attrib & 0x400 ? sp : (old & 0xffff0000) | (sp & 0xffff);
}
static struct ia64_result enter(struct vcpu *v, struct ia32_native_state *n,
                                const struct ia32_decoded *d, const uint8_t *bytes,
                                struct ia32_interruption *record)
{
    const uint8_t *immediate = bytes + d->prefix_length + 1;
    unsigned allocation = get(immediate, 2), level = immediate[2] & 31;
    unsigned width = d->operand_bits / 8, pushes = level ? level + 1 : 1;
    uint32_t old_sp = n->gpr[IA32_ESP], old_bp = n->gpr[IA32_EBP];
    uint32_t destination[32], source[30], sp = stack_address(n, old_sp);
    struct ia64_result r;
    for (unsigned i = 0; i < pushes; ++i) {
        if (i && i < level) {
            source[i - 1] = stack_address(n, old_bp - i * width);
            r = ia32_operand_prepare(v, n, IA32_SEG_SS, source[i - 1], width, false, width, record);
            if (failed(r, record))
                return r;
        }
        sp = stack_address(n, sp - width);
        destination[i] = sp;
        r = ia32_operand_prepare(v, n, IA32_SEG_SS, sp, width, true, width, record);
        if (failed(r, record))
            return r;
    }
    uint32_t final_sp = stack_address(n, sp - allocation);
    r = ia32_operand_prepare(v, n, IA32_SEG_SS, final_sp, 1, true, 1, record);
    if (failed(r, record))
        return r;
    uint32_t frame = stack_value(n, old_sp, destination[0]);
    for (unsigned i = 0; i < pushes; ++i) {
        uint8_t value[4];
        if (!i || i == level)
            put(value, i ? frame : old_bp, width);
        else {
            r = ia32_operand_bytes(v, n, IA32_SEG_SS, source[i - 1], width, false, value, record);
            if (failed(r, record))
                return r;
        }
        r = ia32_operand_bytes(v, n, IA32_SEG_SS, destination[i], width, true, value, record);
        if (failed(r, record))
            return r;
    }
    n->gpr[IA32_EBP] = width == 2 ? (old_bp & 0xffff0000) | (frame & 0xffff) : frame;
    n->gpr[IA32_ESP] = stack_value(n, old_sp, final_sp);
    return ia64_complete();
}
static struct ia64_result fxstate(struct vcpu *v, struct ia32_native_state *n,
                                  const struct ia32_decoded *d, struct ia32_interruption *record)
{
    if (n->cflg & 12)
        return ia32_exception(n, 7, 0, record);
    bool save = ((d->modrm >> 3) & 7) == 0;
    struct ia64_result r = ia32_operand_prepare(v, n, d->segment, d->offset, 512, save, 1, record);
    if (failed(r, record))
        return r;
    uint32_t address = (uint32_t)ia32_segment(n, d->segment)->base + d->offset;
    if (address & 15)
        return ia32_exception(n, 13, 0, record);
    bool sse = (n->cflg & (UINT64_C(1) << 41)) != 0;
    if (save) {
        uint8_t header[32];
        memcpy(header, n->fp.bytes, sizeof(header));
        header[5] = header[14] = header[15] = header[22] = header[23] = 0;
        put(header + 6, get(header + 6, 2) & 0x7ff, 2);
        put(header + 28, 0xffbf, 4);
        r = ia32_operand_bytes(v, n, d->segment, d->offset, sse ? 32 : 24, true, header, record);
        if (failed(r, record))
            return r;
        for (unsigned i = 0; i < 8; ++i) {
            uint8_t data[10];
            memcpy(data, n->fp.bytes + 32 + 16 * i, sizeof(data));
            r = ia32_operand_bytes(v, n, d->segment, d->offset + 32 + 16 * i, 10, true, data,
                                   record);
            if (failed(r, record))
                return r;
        }
        if (sse) {
            uint8_t data[128];
            memcpy(data, n->fp.bytes + 160, sizeof(data));
            r = ia32_operand_bytes(v, n, d->segment, d->offset + 160, sizeof(data), true, data,
                                   record);
        }
        return r;
    }
    uint8_t image[512] = {0};
    r = ia32_operand_bytes(v, n, d->segment, d->offset, sse ? 288 : 160, false, image, record);
    if (failed(r, record))
        return r;
    if (sse && (get(image + 24, 4) & ~UINT64_C(0xffbf)))
        return ia32_exception(n, 13, 0, record);
    struct ia32_fp_image replacement = n->fp;
    memcpy(replacement.bytes, image, 24);
    put(replacement.bytes, (get(image, 2) & 0x1f3f) | 0x40, 2);
    replacement.bytes[5] = replacement.bytes[14] = replacement.bytes[15] = 0;
    replacement.bytes[22] = replacement.bytes[23] = 0;
    put(replacement.bytes + 6, get(image + 6, 2) & 0x7ff, 2);
    for (unsigned i = 0; i < 8; ++i)
        memcpy(replacement.bytes + 32 + 16 * i, image + 32 + 16 * i, 10);
    if (sse) {
        memcpy(replacement.bytes + 24, image + 24, 4);
        memcpy(replacement.bytes + 160, image + 160, 128);
    }
    n->fp = replacement;
    return ia64_complete();
}
static struct ia64_result maskmov(struct vcpu *v, struct ia32_native_state *n,
                                  const struct ia32_decoded *d, struct ia32_interruption *record)
{
    bool xmm = (d->flags & IA32_INSN_SSE) != 0;
    if (n->cflg & 8)
        return ia32_exception(n, 7, 0, record);
    if (!xmm && (n->fp.bytes[2] & 0x80))
        return ia32_exception(n, 16, 0, record);
    unsigned width = xmm ? 16 : 8, top = (get(n->fp.bytes + 2, 2) >> 11) & 7;
    unsigned data_reg = (d->modrm >> 3) & 7, mask_reg = d->modrm & 7;
    unsigned data_slot = xmm ? 160 + 16 * data_reg : 32 + 16 * ((data_reg + 8 - top) & 7);
    unsigned mask_slot = xmm ? 160 + 16 * mask_reg : 32 + 16 * ((mask_reg + 8 - top) & 7);
    uint8_t data[16], mask[16];
    memcpy(data, n->fp.bytes + data_slot, width);
    memcpy(mask, n->fp.bytes + mask_slot, width);
    uint32_t offset = n->gpr[IA32_EDI];
    if (d->address_bits == 16)
        offset &= 0xffff;
    struct ia64_result r =
        ia32_operand_prepare(v, n, d->segment, offset, width, true, xmm ? 1 : 8, record);
    if (failed(r, record))
        return r;
    for (unsigned i = 0; i < width; ++i) {
        if (!(mask[i] & 0x80))
            continue;
        r = ia32_operand_bytes(v, n, d->segment, offset + i, 1, true, data + i, record);
        if (failed(r, record))
            return r;
    }
    if (!xmm) {
        uint8_t physical[128];
        for (unsigned i = 0; i < 8; ++i)
            memcpy(physical + 16 * i, n->fp.bytes + 32 + 16 * ((i + 8 - top) & 7), 16);
        memcpy(n->fp.bytes + 32, physical, sizeof(physical));
        put(n->fp.bytes + 2, get(n->fp.bytes + 2, 2) & ~UINT64_C(0x3800), 2);
        n->fp.bytes[4] = 0xff;
    }
    return ia64_complete();
}
static void rotate_top(struct ia32_fp_image *fp, unsigned next)
{
    unsigned old = (get(fp->bytes + 2, 2) >> 11) & 7;
    uint8_t slots[128];
    for (unsigned st = 0; st < 8; ++st)
        memcpy(slots + 16 * st, fp->bytes + 32 + 16 * ((st + next + 8 - old) & 7), 16);
    memcpy(fp->bytes + 32, slots, sizeof(slots));
}
static void pending(struct ia32_fp_image *fp)
{
    unsigned cw = get(fp->bytes, 2), sw = get(fp->bytes + 2, 2);
    unsigned unmasked = (sw & 0x3f) & ~cw;
    put(fp->bytes + 2, (sw & ~0x8080u) | (unmasked ? 0x8080 : 0), 2);
}
static void environment_save(const struct ia32_native_state *n, bool wide, uint8_t *out)
{
    const uint8_t *fp = n->fp.bytes;
    bool protected = (n->cflg & 1) && !(n->eflag & 0x20000);
    unsigned stride = wide ? 4 : 2;
    memset(out, 0xff, wide ? 28 : 14);
    put(out, get(fp, 2), 2);
    put(out + stride, get(fp + 2, 2), 2);
    put(out + 2 * stride, ia32_x87_tag_word(&n->fp), 2);
    uint32_t ip = get(fp + 8, 4), dp = get(fp + 16, 4);
    unsigned cs = get(fp + 12, 2), ds = get(fp + 20, 2), opcode = get(fp + 6, 2) & 0x7ff;
    if (protected) {
        put(out + 3 * stride, ip, stride);
        put(out + 4 * stride, cs | (wide ? (uint32_t)opcode << 16 : 0), stride);
        put(out + 5 * stride, dp, stride);
        put(out + 6 * stride, ds, 2);
    } else {
        ip += cs << 4;
        dp += ds << 4;
        put(out + 3 * stride, ip & 0xffff, stride);
        put(out + 4 * stride, (((ip >> 16) & (wide ? 0xffff : 15)) << 12) | opcode, stride);
        put(out + 5 * stride, dp & 0xffff, stride);
        put(out + 6 * stride, ((dp >> 16) & (wide ? 0xffff : 15)) << 12, stride);
    }
}
static void environment_load(const struct ia32_native_state *n, struct ia32_fp_image *fp, bool wide,
                             const uint8_t *in, bool registers)
{
    bool protected = (n->cflg & 1) && !(n->eflag & 0x20000);
    unsigned stride = wide ? 4 : 2, sw = get(in + stride, 2);
    if (!registers)
        rotate_top(fp, (sw >> 11) & 7);
    put(fp->bytes, (get(in, 2) & 0x1f3f) | 0x40, 2);
    put(fp->bytes + 2, sw, 2);
    unsigned tags = get(in + 2 * stride, 2);
    fp->bytes[4] = 0;
    /* Itanium FSR.TG stores empty/nonempty; full FTW classes are derived. */
    for (unsigned i = 0; i < 8; ++i)
        if (((tags >> (2 * i)) & 3) != 3)
            fp->bytes[4] |= 1u << i;
    uint32_t ip = get(in + 3 * stride, stride), dp = get(in + 5 * stride, stride);
    uint32_t ip_extra = get(in + 4 * stride, stride), dp_extra = get(in + 6 * stride, stride);
    if (protected) {
        put(fp->bytes + 12, ip_extra, 2);
        put(fp->bytes + 20, dp_extra, 2);
        if (wide)
            put(fp->bytes + 6, (ip_extra >> 16) & 0x7ff, 2);
    } else {
        ip = (ip & 0xffff) | (((ip_extra >> 12) & (wide ? 0xffff : 15)) << 16);
        dp = (dp & 0xffff) | (((dp_extra >> 12) & (wide ? 0xffff : 15)) << 16);
        put(fp->bytes + 12, 0, 2);
        put(fp->bytes + 20, 0, 2);
        put(fp->bytes + 6, ip_extra & 0x7ff, 2);
    }
    put(fp->bytes + 8, ip, 4);
    put(fp->bytes + 16, dp, 4);
    pending(fp);
}
static struct ia64_result x87_environment(struct vcpu *v, struct ia32_native_state *n,
                                          const struct ia32_decoded *d,
                                          struct ia32_interruption *record)
{
    unsigned op = d->opcode, group = (d->modrm >> 3) & 7;
    if (n->cflg & 12)
        return ia32_exception(n, 7, 0, record);
    if (op == 0xdb) {
        if (d->modrm == 0xe2)
            put(n->fp.bytes + 2, get(n->fp.bytes + 2, 2) & 0x7f00, 2);
        else {
            rotate_top(&n->fp, 0);
            memset(n->fp.bytes, 0, 24);
            put(n->fp.bytes, 0x37f, 2);
        }
        return ia64_complete();
    }
    bool load = group == 4 || (op == 0xd9 && group == 5);
    if (load && (n->fp.bytes[2] & 0x80))
        return ia32_exception(n, 16, 0, record);
    bool status = op == 0xdf || (op == 0xdd && group == 7);
    bool word = status || (op == 0xd9 && (group == 5 || group == 7));
    unsigned width = d->operand_bits / 8, environment = width == 4 ? 28 : 14;
    bool registers = op == 0xdd && group != 7;
    unsigned size = word ? 2 : environment + (registers ? 80 : 0);
    if (op == 0xdf) {
        n->gpr[IA32_EAX] = (n->gpr[IA32_EAX] & 0xffff0000) | get(n->fp.bytes + 2, 2);
        return ia64_complete();
    }
    struct ia64_result r =
        ia32_operand_prepare(v, n, d->segment, d->offset, size, !load, word ? 2 : width, record);
    if (failed(r, record))
        return r;
    uint8_t image[108];
    if (!load) {
        if (word)
            put(image, get(n->fp.bytes + (status ? 2 : 0), 2), 2);
        else {
            environment_save(n, width == 4, image);
            if (registers)
                for (unsigned i = 0; i < 8; ++i)
                    memcpy(image + environment + 10 * i, n->fp.bytes + 32 + 16 * i, 10);
        }
    }
    r = ia32_operand_bytes(v, n, d->segment, d->offset, size, !load, image, record);
    if (failed(r, record))
        return r;
    if (load) {
        struct ia32_fp_image replacement = n->fp;
        if (word) {
            put(replacement.bytes, (get(image, 2) & 0x1f3f) | 0x40, 2);
            pending(&replacement);
        } else {
            environment_load(n, &replacement, width == 4, image, registers);
            if (registers)
                for (unsigned i = 0; i < 8; ++i)
                    memcpy(replacement.bytes + 32 + 16 * i, image + environment + 10 * i, 10);
        }
        n->fp = replacement;
    } else if (!word) {
        if (registers) {
            rotate_top(&n->fp, 0);
            memset(n->fp.bytes, 0, 24);
            put(n->fp.bytes, 0x37f, 2);
        } else {
            put(n->fp.bytes, get(n->fp.bytes, 2) | 0x3f, 2);
            pending(&n->fp);
        }
    }
    return ia64_complete();
}
struct ia64_result ia32_extended(struct vcpu *v, struct ia32_native_state *n,
                                 const struct ia32_decoded *d, const uint8_t *bytes, bool *handled,
                                 struct ia32_interruption *record)
{
    *handled = false;
    if (d->kind == IA32_INSN_INTERCEPT)
        return ia64_complete();
    unsigned group = (d->modrm >> 3) & 7;
    bool enter_instruction = d->opcode == 0xc8;
    bool fx = d->opcode == 0x0fae && d->operand_memory && group <= 1;
    bool masked = d->opcode == 0x0ff7 && d->has_modrm && !d->operand_memory;
    bool x87 = (d->operand_memory && ((d->opcode == 0xd9 && group >= 4) ||
                                      (d->opcode == 0xdd && (group == 4 || group >= 6)))) ||
               (d->opcode == 0xdf && d->modrm == 0xe0) ||
               (d->opcode == 0xdb && (d->modrm == 0xe2 || d->modrm == 0xe3));
    if (!enter_instruction && !fx && !masked && !x87)
        return ia64_complete();
    *handled = true;
    struct ia64_result r = enter_instruction ? enter(v, n, d, bytes, record)
                           : fx              ? fxstate(v, n, d, record)
                           : x87             ? x87_environment(v, n, d, record)
                                             : maskmov(v, n, d, record);
    if (!failed(r, record)) {
        n->eip.value += d->length;
        n->eflag &= ~0x10000u;
    }
    return r;
}
