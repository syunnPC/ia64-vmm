/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Specialize location decoding and home addressing per point, with table
 * fallback for cold/large frames. Leaves share the region's allocation,
 * sealing, relocation and reclamation. */
#include "engine/tier1/restore.h"
#include "engine/tier1/region.h"

#define RESTORE_BYTES 4096u
#define RESTORE_ENTRIES 32u

static struct t1_x86_mem mem(uint8_t base, size_t offset)
{
    return (struct t1_x86_mem){base, T1_X86_NOREG, 0, (int32_t)offset};
}

static void rr(struct t1_x86_buf *b, uint16_t op, uint8_t dst, uint8_t src)
{
    struct t1_x86_insn i = {.op = op, .opnd = {{.reg = dst}, {.reg = src}}};
    t1_x86_emit(b, &i, NULL);
}

static void ri(struct t1_x86_buf *b, uint16_t op, uint8_t dst, uint64_t imm)
{
    struct t1_x86_insn i = {.op = op, .opnd = {{.reg = dst}, {.imm = (int64_t)imm}}};
    t1_x86_emit(b, &i, NULL);
}

static void load(struct t1_x86_buf *b, uint16_t op, uint8_t dst, struct t1_x86_mem src)
{
    struct t1_x86_insn i = {.op = op, .opnd = {{.reg = dst}}, .mem = src};
    t1_x86_emit(b, &i, NULL);
}

static void store(struct t1_x86_buf *b, uint16_t op, struct t1_x86_mem dst, uint8_t src)
{
    struct t1_x86_insn i = {.op = op, .opnd = {{0}, {.reg = src}}, .mem = dst};
    t1_x86_emit(b, &i, NULL);
}

static void constant(struct t1_x86_buf *b, uint8_t dst, uint64_t value)
{
    if (!value) {
        struct t1_x86_insn zero = {.op = T1_X86_ZERO32, .opnd = {{.reg = dst}}};
        t1_x86_emit(b, &zero, NULL);
        return;
    }
    uint16_t op = value <= UINT32_MAX                          ? T1_X86_MOV32_RI32
                  : (uint64_t)(int64_t)(int32_t)value == value ? T1_X86_MOV64_RI32
                                                               : T1_X86_MOV64_RI64;
    ri(b, op, dst, value);
}

/* RAX is the value being written. Home stores and stacked addressing
 * preserve it; masks and predicate shifts update this exact constant. */
struct rax_value {
    uint64_t value;
    bool known;
};

static void value(struct t1_x86_buf *b, const struct t1_loc *l, struct rax_value *rax)
{
    if (l->kind == T1_LOC_CONST) {
        if (!rax->known || rax->value != l->value)
            constant(b, T1_RAX, l->value);
        *rax = (struct rax_value){l->value, true};
        return;
    }
    rax->known = false;
    bool reg = l->kind == T1_LOC_REG || l->kind == T1_LOC_REG_PLUS;
    size_t offset = reg ? offsetof(struct t1_exec, regs) + 8u * (l->reg & 15)
                        : offsetof(struct t1_exec, spill) + 8u * (l->slot % T1_SPILL_SLOTS);
    load(b, T1_X86_LOAD64, T1_RAX, mem(T1_RDI, offset));
    if (l->kind == T1_LOC_REG_PLUS || l->kind == T1_LOC_SPILL_PLUS) {
        if ((uint64_t)(int64_t)(int32_t)l->value == l->value)
            ri(b, T1_X86_ADD64_RI, T1_RAX, l->value);
        else {
            constant(b, T1_RCX, l->value);
            rr(b, T1_X86_ADD64_RR, T1_RAX, T1_RCX);
        }
    }
}

/* Entry BOF is in R8; RDX becomes the physical offset into gr[32..127]
 * or nat[32..127]. R10 holds the alternative after one ring wrap. */
static struct t1_x86_mem stacked(struct t1_x86_buf *b, unsigned element, bool nat)
{
    rr(b, T1_X86_MOV64_RR, T1_RDX, T1_R8);
    unsigned offset = element - (nat ? T1_ELEMENT_NAT(32) : T1_ELEMENT_GR(32));
    if (offset)
        ri(b, T1_X86_ADD64_RI, T1_RDX, offset);
    rr(b, T1_X86_MOV64_RR, T1_R10, T1_RDX);
    ri(b, T1_X86_SUB64_RI, T1_R10, IA64_N_STACKED_PHYS);
    ri(b, T1_X86_CMP64_RI, T1_RDX, IA64_N_STACKED_PHYS);
    rr(b, T1_X86_CMOV64 + T1_CC_AE, T1_RDX, T1_R10);
    struct t1_x86_mem dst =
        mem(T1_RSI, nat ? offsetof(struct vcpu, s.nat) + 32 : offsetof(struct vcpu, s.gr) + 256);
    dst.index = T1_RDX;
    dst.scale_shift = nat ? 0 : 3;
    return dst;
}

static bool frame_in(struct t1_x86_buf *b, const struct t1_emitted *out)
{
    if (!out->nframe_in || out->nframe_in > T1_MAX_FRAME_IN)
        return false;
    for (unsigned k = 0; k < out->nframe_in; ++k)
        if (!t1_element_stacked(out->frame_in[k]))
            return false;
    load(b, T1_X86_LOAD32, T1_R8, mem(T1_RDI, offsetof(struct t1_exec, entry_bof)));
    for (unsigned k = 0; k < out->nframe_in && !b->failed; ++k) {
        unsigned el = out->frame_in[k];
        bool nat = el >= T1_ELEMENT_NAT(32);
        load(b, nat ? T1_X86_LOAD8ZX : T1_X86_LOAD64, T1_RAX, stacked(b, el, nat));
        store(b, T1_X86_STORE64, mem(T1_RDI, offsetof(struct t1_exec, frame_in) + 8u * k), T1_RAX);
    }
    struct t1_x86_insn ret = {.op = T1_X86_RET};
    t1_x86_emit(b, &ret, NULL);
    return !b->failed;
}

static bool point(struct t1_x86_buf *b, const struct t1_mir *m, const struct t1_mir_point *p)
{
    unsigned count = 0;
    uint64_t pr_mask = 0;
    bool stack = false;
    for (unsigned k = 0; k < p->frame_count; ++k) {
        unsigned f = p->frame_first + k, el = m->frame[f].element;
        unsigned kind = m->frame_loc[f].kind;
        if (kind == T1_LOC_HOME)
            continue;
        if (kind > T1_LOC_SPILL_PLUS || ++count > RESTORE_ENTRIES)
            return false;
        stack |= t1_element_stacked((uint16_t)el);
        if (el >= T1_ELEMENT_PR(1) && el < T1_ELEMENT_LC)
            pr_mask |= UINT64_C(1) << (el - T1_ELEMENT_PR(0));
    }
    if (!count)
        return false;
    if (stack)
        load(b, T1_X86_LOAD32, T1_R8, mem(T1_RDI, offsetof(struct t1_exec, entry_bof)));
    if (pr_mask)
        constant(b, T1_R9, 0);
    struct rax_value rax = {0, false};
    for (unsigned k = 0; k < p->frame_count && !b->failed; ++k) {
        unsigned f = p->frame_first + k;
        uint16_t el = m->frame[f].element;
        const struct t1_loc *l = &m->frame_loc[f];
        if (l->kind == T1_LOC_HOME)
            continue;
        uint32_t offset;
        unsigned bit, bytes;
        bool stack_home = t1_element_stacked(el);
        if (!stack_home && !t1_element_home(el, &offset, &bit, &bytes))
            return false;
        bool nat = el >= T1_ELEMENT_NAT(1) && el < T1_ELEMENT_PR(0);
        bool pr = el >= T1_ELEMENT_PR(1) && el < T1_ELEMENT_LC;
        struct t1_loc source = *l;
        if (source.kind == T1_LOC_CONST && (nat || pr)) {
            source.value &= 1;
            /* The final PR mask clears this bit without adding a set bit. */
            if (pr && !source.value)
                continue;
        }
        value(b, &source, &rax);
        if ((nat || pr) && source.kind != T1_LOC_CONST)
            ri(b, T1_X86_AND64_RI, T1_RAX, 1);
        if (stack_home) {
            store(b, nat ? T1_X86_STORE8 : T1_X86_STORE64, stacked(b, el, nat), T1_RAX);
        } else if (pr) {
            ri(b, T1_X86_SHL64_RI, T1_RAX, bit);
            rr(b, T1_X86_OR64_RR, T1_R9, T1_RAX);
            if (rax.known)
                rax.value <<= bit;
        } else if (el >= T1_ELEMENT_FR_META(2) && el <= T1_ELEMENT_FR_META(127)) {
            rr(b, T1_X86_MOV64_RR, T1_RCX, T1_RAX);
            ri(b, T1_X86_AND64_RI, T1_RAX, 0x1ffff);
            if (rax.known)
                rax.value &= 0x1ffff;
            store(b, T1_X86_STORE32, mem(T1_RSI, offset), T1_RAX);
            ri(b, T1_X86_SHR64_RI, T1_RCX, 17);
            ri(b, T1_X86_AND64_RI, T1_RCX, 1);
            store(b, T1_X86_STORE8,
                  mem(T1_RSI,
                      offset + offsetof(struct ia64_fr, sign) - offsetof(struct ia64_fr, exponent)),
                  T1_RCX);
        } else if (el == T1_ELEMENT_PSR_MF) {
            ri(b, T1_X86_AND64_RI, T1_RAX, T1_PSR_MF_BITS);
            load(b, T1_X86_LOAD64, T1_RCX, mem(T1_RSI, offset));
            rr(b, T1_X86_OR64_RR, T1_RAX, T1_RCX);
            rax.known = false;
            store(b, T1_X86_STORE64, mem(T1_RSI, offset), T1_RAX);
        } else {
            store(b, bytes == 1 ? T1_X86_STORE8 : T1_X86_STORE64, mem(T1_RSI, offset), T1_RAX);
        }
    }
    if (pr_mask) {
        constant(b, T1_RCX, ~pr_mask);
        load(b, T1_X86_LOAD64, T1_RAX, mem(T1_RSI, offsetof(struct vcpu, s.pr)));
        rr(b, T1_X86_AND64_RR, T1_RAX, T1_RCX);
        rr(b, T1_X86_OR64_RR, T1_RAX, T1_R9);
        store(b, T1_X86_STORE64, mem(T1_RSI, offsetof(struct vcpu, s.pr)), T1_RAX);
    }
    struct t1_x86_insn ret = {.op = T1_X86_RET};
    t1_x86_emit(b, &ret, NULL);
    return !b->failed;
}

void t1_restore_emit(const struct t1_mir *m, struct t1_emitted *out)
{
    size_t cap = out->size + RESTORE_BYTES;
    if (cap > out->cap)
        cap = out->cap;
    if (cap > T1_CHUNK_BYTES)
        cap = T1_CHUNK_BYTES;
    if (out->size >= cap)
        return;
    struct t1_x86_buf entry = {out->buf, out->size, cap, false};
    if (frame_in(&entry, out)) {
        out->frame_in_offset = (uint32_t)out->size;
        out->size = entry.size;
    }
    /* Exit homes normally feed the next region; checkpoints also handle
     * budget and notifications. Sites/helpers keep their cold C restore. */
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned p = 0; p < m->ir->npoints; ++p) {
            const struct t1_mir_point *mp = &m->point[p];
            if (mp->insn == T1_MIR_NONE || mp->kind != (pass ? T1_PT_CHECKPOINT : T1_PT_EXIT))
                continue;
            struct t1_x86_buf b = {out->buf, out->size, cap, false};
            if (point(&b, m, mp)) {
                out->pt[p].restore_offset = (uint32_t)out->size;
                out->size = b.size;
            }
        }
}
