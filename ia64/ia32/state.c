/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64vmm/ia32_state.h"
#include "ia64/state.h"
#include "ia64vmm/string.h"

static uint64_t read_le(const uint8_t *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void write_le(uint8_t *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = value >> (i * 8);
}
static uint64_t merge(uint64_t original, uint64_t value, uint64_t mask)
{
    return (original & ~mask) | (value & mask);
}

void ia32_segment_decode(uint64_t d, uint16_t selector, bool system, struct ia32_segment *out)
{
    *out = (struct ia32_segment){.base = (uint32_t)d,
                                 .selector = selector,
                                 .limit = (d >> 32) & 0xfffff,
                                 .attrib =
                                     ((d >> 52) & 0xff) | ((d >> 52) & (system ? 0x800 : 0xc00))};
    if (d >> 63)
        out->limit = (out->limit << 12) | 0xfff;
}
enum ia32_state_status ia32_segment_encode(const struct ia32_segment *seg, uint64_t original,
                                           bool system, bool loaded, uint64_t *out)
{
    /* VMX unusable cache fields are unspecified (Intel SDM Vol.3C
     * §25.4.1). A null data/LDT selector is represented by P=0 on Itanium;
     * retain its private cache, not arbitrary native cache fields. */
    if (seg->unusable) {
        if (seg->selector & ~3u)
            return IA32_STATE_UNREPRESENTABLE;
        *out = original & ~(UINT64_C(1) << 59);
        if (loaded)
            *out &= ~((uint64_t)(system ? 7 : 3) << 60);
        return IA32_STATE_OK;
    }
    unsigned allowed = (system ? 0x8ff : 0xcff) | (loaded ? 0x100 : 0);
    if (seg->base > UINT32_MAX || (seg->attrib & ~allowed))
        return IA32_STATE_UNREPRESENTABLE;
    uint32_t limit = seg->limit;
    if (seg->attrib & 0x800) {
        if ((limit & 0xfff) != 0xfff)
            return IA32_STATE_UNREPRESENTABLE;
        limit >>= 12;
    } else if (limit > 0xfffff)
        return IA32_STATE_UNREPRESENTABLE;
    uint64_t ignored = (uint64_t)(system ? 7 : 3) << 60;
    uint64_t value =
        (uint32_t)seg->base | (uint64_t)limit << 32 | (uint64_t)(seg->attrib & ~0x100u) << 52;
    *out = value | (loaded ? 0 : original & ignored);
    return IA32_STATE_OK;
}
/* SDM Vol.2 Fig.10-1/Table 10-2: GDTD still has a 20-bit limit
 * and G scaling. Native GDTR has only base/16-bit limit and no attributes
 * (Intel SDM Vol.3C §25.4.1). Retain the original encoding, including G,
 * and stop when the effective limit cannot be represented by native GDTR. */
static enum ia32_state_status gdt_encode(const struct ia32_segment *seg, uint64_t original,
                                         uint64_t *out)
{
    if (seg->base > UINT32_MAX || seg->limit > 0xffff || seg->attrib || seg->selector ||
        seg->unusable)
        return IA32_STATE_UNREPRESENTABLE;
    uint32_t limit = seg->limit;
    if (original >> 63) {
        if ((limit & 0xfff) != 0xfff)
            return IA32_STATE_UNREPRESENTABLE;
        limit >>= 12;
    }
    *out = merge(original, (uint64_t)limit << 32 | seg->base, UINT64_C(0x000fffffffffffff));
    return IA32_STATE_OK;
}
enum ia32_state_status ia32_x87_encode(const struct ia64_fr *f, uint8_t out[10])
{
    uint32_t e = f->exponent;
    /* SDM Vol.1 §6.2.2.5.2: undefined inputs stop diagnostically. The
     * noncanonical 0xc000 exponent cannot round-trip as extended zero. */
    if (e == 0x1ffff)
        e = 0x7fff;
    else if (e != 0) {
        if (e < 0xc001 || e > 0x13ffe)
            return IA32_STATE_UNDEFINED;
        e -= 0xc000;
    }
    write_le(out, f->significand, 8);
    write_le(out + 8, e | (unsigned)f->sign << 15, 2);
    return IA32_STATE_OK;
}
void ia32_x87_decode(const uint8_t in[10], struct ia64_fr *out)
{
    unsigned high = read_le(in + 8, 2), e = high & 0x7fff;
    *out = (struct ia64_fr){.significand = read_le(in, 8),
                            .sign = high >> 15,
                            .exponent = e == 0x7fff ? 0x1ffff
                                        : e         ? e + 0xc000
                                                    : 0};
}

uint16_t ia32_x87_tag_word(const struct ia32_fp_image *fp)
{
    const uint8_t *b = fp->bytes;
    unsigned top = (read_le(b + 2, 2) >> 11) & 7;
    uint16_t tags = 0;
    for (unsigned physical = 0; physical < 8; ++physical) {
        unsigned tag = 3;
        if (b[4] & (1u << physical)) {
            unsigned st = (physical + 8 - top) & 7;
            uint64_t significand = read_le(b + 32 + 16 * st, 8);
            unsigned e = read_le(b + 40 + 16 * st, 2) & 0x7fff;
            tag = !e && !significand ? 1 : e && e != 0x7fff && (significand >> 63) ? 0 : 2;
        }
        tags |= tag << (physical * 2);
    }
    return tags;
}

/* SDM Vol.1 Table 6-1 and Vol.2 Table 10-1. CS/SS are ARs; never GR25/26. */
static const unsigned descriptor_reg[IA32_SEG_COUNT] = {24, 25, 26, 27, 28, 29, 30, 1, 31};
static bool descriptor_ar(unsigned i)
{
    return i == IA32_SEG_CS || i == IA32_SEG_SS || i == IA32_SEG_TSS;
}
static struct ia32_segment *native_segment(struct ia32_native_state *n, unsigned i)
{
    switch (i) {
    case IA32_SEG_ES:
        return &n->es;
    case IA32_SEG_CS:
        return &n->cs;
    case IA32_SEG_SS:
        return &n->ss;
    case IA32_SEG_DS:
        return &n->ds;
    case IA32_SEG_FS:
        return &n->fs;
    case IA32_SEG_GS:
        return &n->gs;
    case IA32_SEG_LDT:
        return &n->ldtr;
    case IA32_SEG_TSS:
        return &n->tr;
    default:
        return &n->gdtr;
    }
}
static const struct ia32_segment *const_segment(const struct ia32_native_state *n, unsigned i)
{
    switch (i) {
    case IA32_SEG_ES:
        return &n->es;
    case IA32_SEG_CS:
        return &n->cs;
    case IA32_SEG_SS:
        return &n->ss;
    case IA32_SEG_DS:
        return &n->ds;
    case IA32_SEG_FS:
        return &n->fs;
    case IA32_SEG_GS:
        return &n->gs;
    case IA32_SEG_LDT:
        return &n->ldtr;
    case IA32_SEG_TSS:
        return &n->tr;
    default:
        return &n->gdtr;
    }
}
static uint64_t view_gr(const struct ia64_state *s, unsigned r, bool bank)
{
    return bank && r >= 16 && r < 32 ? s->bank_gr[r - 16] : s->gr[r];
}
static bool view_nat(const struct ia64_state *s, unsigned r, bool bank)
{
    return bank && r >= 16 && r < 32 ? s->bank_nat[r - 16] : s->nat[r];
}
static uint64_t descriptor(const struct ia64_state *s, unsigned i, bool bank)
{
    return descriptor_ar(i) ? s->ar[descriptor_reg[i]] : view_gr(s, descriptor_reg[i], bank);
}
static uint16_t selector(const struct ia64_state *s, unsigned i, bool bank)
{
    static const unsigned packed[IA32_SEG_COUNT] = {16, 17, 17, 16, 16, 16, 17, 17, 0};
    static const unsigned shift[IA32_SEG_COUNT] = {16, 0, 16, 0, 32, 48, 32, 48, 0};
    return i == IA32_SEG_GDT ? 0 : view_gr(s, packed[i], bank) >> shift[i];
}
static enum ia32_state_status export_view(const struct ia64_state *s, uint64_t psr,
                                          struct ia32_native_state *n)
{
    bool bank = ((s->psr ^ psr) & IA64_PSR_BN) != 0;
    for (unsigned r = 8; r < 32; ++r) {
        bool mapped = r <= 17 || r == 24 || r >= 27;
        if (mapped && view_nat(s, r, bank))
            return IA32_STATE_UNDEFINED;
    }
    uint8_t fp[8][10] = {{0}};
    bool low_fp = !(psr & (UINT64_C(1) << 18));
    for (unsigned i = 0; low_fp && i < 8; ++i) {
        enum ia32_state_status status = ia32_x87_encode(&s->fr[8 + i], fp[i]);
        if (status != IA32_STATE_OK)
            return status;
    }
    struct ia32_segment gdt;
    ia32_segment_decode(descriptor(s, IA32_SEG_GDT, bank), 0, true, &gdt);
    if (gdt.limit > 0xffff)
        return IA32_STATE_UNREPRESENTABLE;
    gdt.attrib = 0;
    memset(n, 0, sizeof(*n));
    for (unsigned i = 0; i < IA32_GPRS; ++i)
        n->gpr[i] = s->gr[8 + i];
    n->eflag = (s->ar[AR_EFLAG] & IA32_EFLAG_DEFINED) | 2;
    n->cflg = s->ar[AR_CFLG];
    n->kr0 = s->ar[0];
    n->kr1 = s->ar[1];
    n->kr2 = s->ar[2];
    for (unsigned i = 0; i < IA32_SEG_GDT; ++i)
        ia32_segment_decode(descriptor(s, i, bank), selector(s, i, bank), i >= IA32_SEG_LDT,
                            native_segment(n, i));
    n->gdtr = gdt;
    n->cpl = (psr >> 32) & 3;
    n->eip.value = (uint32_t)s->ip - (uint32_t)n->cs.base;

    uint64_t fcr = s->ar[AR_FCR], fsr = s->ar[AR_FSR];
    uint16_t fsw = (fsr & 0x7fff) | ((fsr & 0x80) ? 0x8000 : 0);
    uint8_t *b = n->fp.bytes;
    write_le(b, (fcr & 0x1f3f) | 0x40, 2);
    write_le(b + 2, fsw, 2);
    unsigned top = (fsw >> 11) & 7;
    for (unsigned i = 0; i < 8; ++i) {
        /* FXSAVE FTW names physical registers; its slots name ST(i). */
        if (!(fsr & (UINT64_C(1) << (16 + 2 * i))))
            b[4] |= 1u << i;
        memcpy(b + 32 + 16 * i, fp[(top + i) & 7], 10);
        if (low_fp) {
            write_le(b + 160 + 16 * i, s->fr[16 + 2 * i].significand, 8);
            write_le(b + 168 + 16 * i, s->fr[17 + 2 * i].significand, 8);
        }
    }
    write_le(b + 6, (s->ar[AR_FIR] >> 48) & 0x7ff, 2);
    write_le(b + 8, s->ar[AR_FIR], 4);
    write_le(b + 12, s->ar[AR_FIR] >> 32, 2);
    write_le(b + 16, s->ar[AR_FDR], 4);
    write_le(b + 20, s->ar[AR_FDR] >> 32, 2);
    write_le(b + 24, ((fcr >> 32) & 0xff80) | ((fsr >> 32) & 0x3f), 4);
    write_le(b + 28, 0xffbf, 4); /* SDM Vol.1 Fig.6-1: DAZ is reserved. */
    return IA32_STATE_OK;
}
enum ia32_state_status ia32_state_export(const struct ia64_state *s, struct ia32_native_state *n)
{
    return export_view(s, s->psr, n);
}

static void import_fp(struct ia64_state *s, const struct ia32_fp_image *fp)
{
    const uint8_t *b = fp->bytes;
    unsigned fsw = read_le(b + 2, 2), top = (fsw >> 11) & 7;
    uint64_t fsr = (fsw & 0x7fff) | ((fsw & 0x80) ? 0x8000 : 0);
    uint32_t mxcsr = read_le(b + 24, 4);
    for (unsigned i = 0; i < 8; ++i) {
        ia32_x87_decode(b + 32 + 16 * i, &s->fr[8 + ((top + i) & 7)]);
        fsr |= (uint64_t)(!(b[4] & (1u << i))) << (16 + 2 * i);
        s->fr[16 + 2 * i] = (struct ia64_fr){read_le(b + 160 + 16 * i, 8), 0x1003e, false};
        s->fr[17 + 2 * i] = (struct ia64_fr){read_le(b + 168 + 16 * i, 8), 0x1003e, false};
    }
    fsr |= (uint64_t)(mxcsr & 0x3f) << 32;
    uint64_t fcr = (read_le(b, 2) & 0x1f3f) | (uint64_t)(mxcsr & 0xff80) << 32;
    uint64_t fir = read_le(b + 8, 4) | read_le(b + 12, 2) << 32 | (read_le(b + 6, 2) & 0x7ff) << 48;
    uint64_t fdr = read_le(b + 16, 4) | read_le(b + 20, 2) << 32;
    s->ar[AR_FCR] = merge(s->ar[AR_FCR], fcr, IA32_FCR_DEFINED);
    s->ar[AR_FSR] = merge(s->ar[AR_FSR], fsr, IA32_FSR_DEFINED);
    s->ar[AR_FIR] = merge(s->ar[AR_FIR], fir, IA32_FIR_DEFINED);
    s->ar[AR_FDR] = merge(s->ar[AR_FDR], fdr, IA32_FDR_DEFINED);
}
enum ia32_state_status ia32_state_import(struct ia64_state *s, const struct ia32_native_state *n,
                                         uint64_t dirty, unsigned loaded, bool completed)
{
    uint64_t descriptors[IA32_SEG_COUNT];
    bool low_fp = !(s->psr & (UINT64_C(1) << 18));
    if ((dirty & ~IA32_DIRTY_ALL) || loaded >> IA32_SEG_COUNT ||
        (loaded && !(dirty & IA32_DIRTY_SEGMENTS)) || n->cpl != ((s->psr >> 32) & 3))
        return IA32_STATE_UNREPRESENTABLE;
    if ((dirty & IA32_DIRTY_FP) && low_fp && (read_le(n->fp.bytes + 24, 4) & ~UINT64_C(0xffbf)))
        return IA32_STATE_UNREPRESENTABLE;
    for (unsigned i = 0; i < IA32_SEG_COUNT; ++i) {
        if (!(dirty & IA32_DIRTY_SEGMENTS) && !(i == IA32_SEG_TSS && (dirty & IA32_DIRTY_CONTROL)))
            continue;
        const struct ia32_segment *seg = const_segment(n, i);
        if (seg->unusable && (i == IA32_SEG_CS || i == IA32_SEG_SS || i == IA32_SEG_TSS))
            return IA32_STATE_UNREPRESENTABLE;
        uint64_t original = descriptor(s, i, false);
        enum ia32_state_status status =
            i == IA32_SEG_GDT ? gdt_encode(seg, original, &descriptors[i])
                              : ia32_segment_encode(seg, original, i >= IA32_SEG_LDT,
                                                    (loaded >> i) & 1, &descriptors[i]);
        if (status != IA32_STATE_OK)
            return status;
    }
    /* KR1 and TSSD alias AR1 (SDM Vol.2 Table 10-1). Never let the
     * control group bypass segment validation or overwrite its result. */
    if ((dirty & (IA32_DIRTY_SEGMENTS | IA32_DIRTY_CONTROL)) && n->kr1 != descriptors[IA32_SEG_TSS])
        return IA32_STATE_UNREPRESENTABLE;
    if (dirty & IA32_DIRTY_GPR)
        for (unsigned i = 0; i < IA32_GPRS; ++i) {
            uint32_t value = n->gpr[i];
            s->gr[8 + i] = (uint64_t)value | ((value >> 31) ? UINT64_C(0xffffffff00000000) : 0);
            s->nat[8 + i] = false;
        }
    if (dirty & IA32_DIRTY_FLAGS)
        s->ar[AR_EFLAG] = merge(s->ar[AR_EFLAG], n->eflag, IA32_EFLAG_DEFINED);
    if (dirty & IA32_DIRTY_CONTROL) {
        s->ar[AR_CFLG] = merge(s->ar[AR_CFLG], n->cflg, IA32_CFLG_DEFINED);
        s->ar[0] = n->kr0;
        s->ar[2] = n->kr2;
    }
    if (dirty & IA32_DIRTY_SEGMENTS) {
        s->gr[16] = (uint64_t)n->ds.selector | (uint64_t)n->es.selector << 16 |
                    (uint64_t)n->fs.selector << 32 | (uint64_t)n->gs.selector << 48;
        s->gr[17] = (uint64_t)n->cs.selector | (uint64_t)n->ss.selector << 16 |
                    (uint64_t)n->ldtr.selector << 32 | (uint64_t)n->tr.selector << 48;
        s->nat[16] = s->nat[17] = false;
        for (unsigned i = 0; i < IA32_SEG_COUNT; ++i) {
            if (i == IA32_SEG_TSS)
                continue;
            unsigned reg = descriptor_reg[i];
            if (descriptor_ar(i))
                s->ar[reg] = descriptors[i];
            else {
                s->gr[reg] = descriptors[i];
                s->nat[reg] = false;
            }
        }
    }
    if (dirty & (IA32_DIRTY_SEGMENTS | IA32_DIRTY_CONTROL))
        s->ar[1] = descriptors[IA32_SEG_TSS];
    if (dirty & IA32_DIRTY_IP) {
        s->ip = (uint32_t)(n->cs.base + n->eip.value);
        s->slot = 0;
    }
    if ((dirty & IA32_DIRTY_FP) && low_fp)
        import_fp(s, &n->fp);
    if (completed)
        s->psr &= ~IA64_PSR_ID;
    return IA32_STATE_OK;
}
enum ia32_state_status ia32_state_leave(struct ia64_state *s, const struct ia32_native_state *n,
                                        unsigned loaded, bool completed)
{
    if (!(s->psr & IA64_PSR_IS))
        return IA32_STATE_ILLEGAL;
    enum ia32_state_status status = ia32_state_import(s, n, IA32_DIRTY_ALL, loaded, completed);
    if (status == IA32_STATE_OK && !(s->psr & (UINT64_C(1) << 18)))
        s->psr |= UINT64_C(1) << 4;
    return status;
}

static void empty_frame(struct ia64_state *s)
{
    s->cfm = 0;
    s->rse_dirty = s->rse_missing = 0;
    s->rse_cfle = s->rse_completion_pending = false;
    /* This lazy RSE has no clean partition. Dropped registers cannot be
     * reused without a mandatory fill; the preserved ARs do not enable it
     * while PSR.is=1 (SDM Vol.1 §6.2.4.1). */
}
enum ia32_state_status ia32_state_enter(struct ia64_state *s, enum ia32_entry_kind kind,
                                        uint64_t target, struct ia32_native_state *n,
                                        struct ia32_interruption *completion)
{
    *completion = (struct ia32_interruption){0};
    uint64_t psr = s->psr;
    if (kind == IA32_ENTRY_BRANCH) {
        if (s->rse_dirty || s->ar[AR_BSP] != s->ar[AR_BSPSTORE])
            return IA32_STATE_ILLEGAL;
        if ((psr & IA64_PSR_DI) || s->cpu_model != IA64_CPU_MADISON)
            return IA32_STATE_DISABLED;
        psr = (psr | IA64_PSR_IS) & ~(IA64_PSR_ID | (UINT64_C(3) << 41));
    } else if (kind == IA32_ENTRY_RFI) {
        if ((s->psr >> 32) & 3)
            return IA32_STATE_PRIVILEGED;
        psr = s->cr[CR_IPSR];
        target = s->cr[CR_IIP];
        if (!(psr & IA64_PSR_IS))
            return IA32_STATE_ILLEGAL;
        if (s->cpu_model != IA64_CPU_MADISON)
            return IA32_STATE_DISABLED;
        /* Same reserved-field checks as rfi, before restoring any state. */
        if ((psr >> 46) || (psr & UINT64_C(0xf0011fc1)) || ((psr >> 41) & 3) == 3 ||
            (s->cr[CR_IFS] & (UINT64_C(0x1ffffff) << 38)))
            return IA32_STATE_RESERVED;
    } else
        return IA32_STATE_UNREPRESENTABLE;
    const uint64_t required = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
    if ((psr & required) != required || (psr & (UINT64_C(1) << 35)))
        return IA32_STATE_UNDEFINED;
    struct ia32_native_state native;
    enum ia32_state_status status = export_view(s, psr, &native);
    if (status != IA32_STATE_OK)
        return status;
    native.eip.value = (uint32_t)target - (uint32_t)native.cs.base;
    uint64_t source = s->ip;
    /* SDM Vol.3 rfi p.3:237 and Vol.2 §5.3: clear these before
     * either entry kind; only rfi's PSR.id survives until completion. */
    psr &= ~(IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_IA | IA64_PSR_ED);
    ia64_psr_write(s, psr);
    s->ip = (uint32_t)target;
    s->slot = 0;
    empty_frame(s);
    /* Dirty state on rfi entry is architecturally undefined. Match the
     * lazy-RSE invariant when discarding it, without issuing stores. */
    if (s->ar[AR_BSPSTORE] != s->ar[AR_BSP]) {
        s->ar[AR_BSPSTORE] = s->ar[AR_BSP];
        s->ar[AR_RNAT] = 0;
    }
    *n = native;
    if (kind == IA32_ENTRY_BRANCH && (psr & (IA64_PSR_TB | IA64_PSR_SS))) {
        *completion = (struct ia32_interruption){.valid = true,
                                                 .transition = true,
                                                 .phase = IA32_PHASE_COMPLETED,
                                                 .iip = target,
                                                 .iipa = source,
                                                 .iipa_valid = true,
                                                 .ivt = (psr & IA64_PSR_TB) ? 0x5f00 : 0x6000,
                                                 .isr = ((psr & IA64_PSR_TB) ? 4 : 0) |
                                                        ((psr & IA64_PSR_SS) ? 8 : 0)};
    }
    return IA32_STATE_OK;
}
enum ia32_state_status ia32_state_jmpe_check(const struct ia64_state *s,
                                             const struct ia32_native_state *n)
{
    if (!(s->psr & IA64_PSR_IS))
        return IA32_STATE_ILLEGAL;
    if ((s->psr & IA64_PSR_DI) || s->cpu_model != IA64_CPU_MADISON)
        return IA32_STATE_DISABLED;
    if (read_le(n->fp.bytes + 2, 2) & 0x80)
        return IA32_STATE_FP_PENDING;
    return IA32_STATE_OK;
}
enum ia32_state_status ia32_state_jmpe(struct ia64_state *s, const struct ia32_native_state *n,
                                       uint32_t operand, ia32_eip_t next, unsigned data_breakpoints,
                                       struct ia32_interruption *completion)
{
    *completion = (struct ia32_interruption){0};
    enum ia32_state_status status = ia32_state_jmpe_check(s, n);
    if (status != IA32_STATE_OK)
        return status;
    if (data_breakpoints > 15)
        return IA32_STATE_UNREPRESENTABLE;
    uint32_t source = n->cs.base + n->eip.value;
    uint32_t target = (uint32_t)(n->cs.base + operand) & ~UINT32_C(15);
    status = ia32_state_leave(s, n, 0, true);
    if (status != IA32_STATE_OK)
        return status;
    s->gr[1] = (uint32_t)(n->cs.base + next.value);
    s->nat[1] = false;
    s->ar[AR_EFLAG] &= ~UINT64_C(0x10000);
    s->psr &= ~(IA64_PSR_IS | IA64_PSR_ID | (UINT64_C(3) << 41));
    s->ip = target;
    s->slot = 0;
    empty_frame(s);
    unsigned code = data_breakpoints << 4;
    if (s->psr & IA64_PSR_TB)
        code |= 4;
    if ((s->psr & IA64_PSR_SS) || (n->eflag & 0x100))
        code |= 8;
    if (code) {
        *completion = (struct ia32_interruption){.valid = true,
                                                 .transition = true,
                                                 .ivt = 0x6900,
                                                 .phase = IA32_PHASE_COMPLETED,
                                                 .isr = UINT64_C(0x10000) | code,
                                                 .iip = target,
                                                 .iipa = source,
                                                 .iipa_valid = (code & 4) != 0};
    }
    return IA32_STATE_OK;
}

enum ia32_state_status ia32_state_jmpe_memory(struct ia64_state *s,
                                              const struct ia32_native_state *n,
                                              ia32_jmpe_reader reader, void *context,
                                              ia32_eip_t next, unsigned data_breakpoints,
                                              struct ia32_interruption *completion)
{
    *completion = (struct ia32_interruption){0};
    enum ia32_state_status status = ia32_state_jmpe_check(s, n);
    if (status != IA32_STATE_OK)
        return status;
    if (!reader || data_breakpoints > 15)
        return IA32_STATE_UNREPRESENTABLE;
    uint32_t operand;
    status = reader(context, &operand);
    if (status != IA32_STATE_OK)
        return status;
    return ia32_state_jmpe(s, n, operand, next, data_breakpoints, completion);
}
