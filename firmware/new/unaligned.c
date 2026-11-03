/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Rev 2.3 Vol.2 §3.5/§7.1; Vol.3 M1-M10 instruction formats.
 * Byte emulation is confined to ordinary RAM, never device transactions.
 * FP bit transfers ported from ia64/fp.c (fork 5f67e29 origin).
 */
#include "unaligned.h"

bool unaligned_m_slot(unsigned code, unsigned slot)
{
    /* SDM Vol.1 bundle template table, including the internal stop in M;MI. */
    static const char *const units[32] = {"MII", "MII", "MII", "MII", "MLX", "MLX", 0,     0,
                                          "MMI", "MMI", "MMI", "MMI", "MFI", "MFI", "MMF", "MMF",
                                          "MIB", "MIB", "MBB", "MBB", 0,     0,     "BBB", "BBB",
                                          "MMB", "MMB", 0,     0,     "MFB", "MFB", 0,     0};
    return code < 32 && slot < 3 && units[code] && units[code][slot] == 'M';
}
static bool nat(const struct unaligned_frame *f, unsigned reg)
{
    return reg && ((reg < 64 ? f->nat : f->nat_hi) >> (reg & 63) & 1);
}
static void set_gr(struct unaligned_frame *f, unsigned reg, uint64_t value, bool n)
{
    if (!reg)
        return;
    f->gr[reg] = value;
    uint64_t *bits = reg < 64 ? &f->nat : &f->nat_hi, mask = UINT64_C(1) << (reg & 63);
    *bits = (*bits & ~mask) | (n ? mask : 0);
}
static bool gr_valid(const struct unaligned_frame *f, unsigned reg)
{
    return reg < 32 || reg - 32 < (f->cfm & 127);
}
static void fence(void)
{
#ifdef __ia64__
    __asm__ volatile("mf;;" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
static uint64_t read_bytes(const volatile uint8_t *p, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static void write_bytes(volatile uint8_t *p, unsigned count, uint64_t value)
{
    for (unsigned i = 0; i < count; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

bool unaligned_emulate(struct unaligned_frame *f, uint64_t raw, volatile uint8_t *memory)
{
    unsigned major = (unsigned)(raw >> 37), x6 = (unsigned)(raw >> 30) & 63;
    bool fp = major == 6 || major == 7, store = x6 >= 0x30 && x6 <= (fp ? 0x33u : 0x37u);
    bool spill = x6 == 0x1b || x6 == 0x3b;
    if (x6 == 0x3b)
        store = true;
    bool speculative = x6 >= 4 && x6 <= 15 && (x6 & 4);
    bool advanced = x6 >= 8 && x6 <= 15;
    unsigned reg = (unsigned)(raw >> (store ? 13 : 6)) & 127;
    unsigned base = (unsigned)(raw >> 20) & 127, inc = (unsigned)(raw >> 13) & 127;
    unsigned width = fp ? (x6 & 3) == 0 ? 10 : (x6 & 3) == 2 ? 4 : 8 : 1u << (x6 & 3);
    if (spill)
        width = fp ? 16 : 8;
    unsigned align = width == 10 ? 16 : width, slot = (unsigned)(f->ipsr >> 41) & 3;
    bool immediate = major == 5 || major == 7;
    bool post = immediate || (raw & (UINT64_C(1) << 36));
    bool load_ok =
        fp ? x6 <= 15 || (x6 >= 0x20 && x6 <= 0x27) : x6 <= 0x17 || (x6 >= 0x20 && x6 <= 0x2b);
    /* PSR.ac=1 leaves non-speculative references to the OS; a speculative
     * load still defers (SDM Vol.2 §5.5), as the OS loader runs with ac=1
     * (SAL §3.2.5). */
    if (((f->ipsr & 8) && !speculative) || slot > 2 || major < 4 || major > 7 ||
        (!store && !load_ok && !spill) || (!immediate && (raw & (UINT64_C(1) << 27))) ||
        !gr_valid(f, base) || !base || (!fp && !gr_valid(f, reg)) || (fp && !store && reg < 2) ||
        !(f->ifa & (align - 1)) || (post && !fp && !store && reg == base) ||
        (post && store && !immediate) || (!speculative && nat(f, base)) ||
        (!fp && store && !spill && nat(f, reg)))
        return false;
    if (fp && ((reg < 32 && (f->ipsr & (UINT64_C(1) << 18))) ||
               (reg >= 32 && (f->ipsr & (UINT64_C(1) << 19)))))
        return false;
    uint64_t delta = 0;
    bool delta_nat = false;
    if (immediate) {
        unsigned bits = (unsigned)(raw >> (store ? 6 : 13)) & 127;
        bits |= ((unsigned)(raw >> 27) & 1) << 7;
        bits |= ((unsigned)(raw >> 36) & 1) << 8;
        delta = (uint64_t)(int64_t)((int)bits - (bits & 256 ? 512 : 0));
    } else if (post) {
        if (!gr_valid(f, inc))
            return false;
        delta = inc ? f->gr[inc] : 0;
        delta_nat = nat(f, inc);
    }
    /* Vol.2 §5.5: software deferral reexecutes just this instruction with ED. */
    if (speculative && (f->isr & (UINT64_C(1) << 43))) {
        f->ipsr |= UINT64_C(1) << 43;
        return true;
    }
    bool deferred = speculative; /* Physical WB is limited-speculation memory. */
    uint64_t low = 0, high = 0;
    if (fp && store) {
        low = f->fp[reg][0];
        high = f->fp[reg][1];
        unsigned exp = high & 0x1ffff;
        if (!spill && !low && high == 0x1fffe)
            return false; /* NaTVal consumption. */
        if (!spill && (x6 & 3) == 0)
            high = ((exp >> 2) & 0x4000) | (exp & 0x3fff) | ((high >> 17) << 15);
        else if (!spill && (x6 & 3) >= 2) {
            bool single = (x6 & 3) == 2;
            unsigned bits = single ? 23 : 52;
            uint64_t e =
                single ? ((exp >> 9) & 0x80) | (exp & 0x7f) : ((exp >> 6) & 0x400) | (exp & 0x3ff);
            if (!(low >> 63))
                e = 0;
            low = ((high >> 17) << (single ? 31 : 63)) | (e << bits) |
                  ((low >> (63 - bits)) & ((UINT64_C(1) << bits) - 1));
        }
    } else if (store)
        low = reg && !(spill && nat(f, reg)) ? f->gr[reg] : 0;
    if (!deferred && !memory)
        return false;
    bool release = !fp && store && x6 >= 0x34 && x6 <= 0x37;
    bool acquire = !fp && !store && ((x6 >= 0x10 && x6 <= 0x13) || (x6 >= 0x28 && x6 <= 0x2b));
    if (release)
        fence();
    if (store) {
        write_bytes(memory, width > 8 ? 8 : width, low);
        if (width > 8)
            write_bytes(memory + 8, width - 8, high);
        if (!fp && spill) {
            uint64_t bit = UINT64_C(1) << ((f->ifa >> 3) & 63);
            f->unat = (f->unat & ~bit) | (nat(f, reg) ? bit : 0);
        }
    } else if (fp) {
        if (deferred)
            high = 0x1fffe;
        else {
            low = read_bytes(memory, width > 8 ? 8 : width);
            if (width > 8)
                high = read_bytes(memory + 8, width - 8);
            if (!spill && (x6 & 3) == 0) {
                unsigned exp = high & 0x7fff;
                high = (exp == 0x7fff ? 0x1ffff : exp ? exp + 0xc000 : 0) | ((high >> 15) << 17);
            } else if (!spill && (x6 & 3) == 1)
                high = 0x1003e;
            else if (!spill) {
                bool single = (x6 & 3) == 2;
                unsigned bits = single ? 23 : 52, mask = single ? 0xff : 0x7ff;
                unsigned bias = single ? 0xff80 : 0xfc00, exp = (low >> bits) & mask;
                uint64_t fraction = low & ((UINT64_C(1) << bits) - 1);
                high = (low >> (single ? 31 : 63)) << 17;
                low = fraction << (63 - bits);
                if (exp) {
                    low |= UINT64_C(1) << 63;
                    high |= exp == mask ? 0x1ffff : bias + exp;
                } else if (fraction)
                    high |= bias + 1;
            }
        }
        f->fp[reg][0] = low;
        f->fp[reg][1] = high;
        f->ipsr |= UINT64_C(1) << (reg < 32 ? 4 : 5);
    } else {
        low = deferred ? 0 : read_bytes(memory, width);
        bool n = deferred || (spill && ((f->unat >> ((f->ifa >> 3) & 63)) & 1));
        set_gr(f, reg, low, n);
    }
    if (acquire)
        fence();
    if (post)
        set_gr(f, base, f->gr[base] + delta, nat(f, base) || delta_nat);
#ifdef __ia64__
    /* Unaligned advanced references cannot retain a useful ALAT entry.
     * The VMM's permitted zero-entry ALAT agrees with this invalidation. */
    if (advanced || store)
        __asm__ volatile("invala;;" ::: "memory");
#else
    (void)advanced;
#endif
    if (slot == 2) {
        f->iip += 16;
        slot = 0;
    } else
        ++slot;
    f->ipsr = (f->ipsr & ~(UINT64_C(3) << 41)) | ((uint64_t)slot << 41);
    return true;
}
#ifndef UNALIGNED_HOST_TEST
#include "ivt-layout.h"
#include <ia64vmm/platform.h>
extern const struct ia64vmm_handoff *unaligned_handoff;
static bool ram(uint64_t address, unsigned size)
{
    const struct ia64vmm_handoff *h = unaligned_handoff;
    address &= ~(UINT64_C(1) << 63);
    for (unsigned i = 0; i < h->ram_range_count; ++i) {
        if (address >= h->ram[i].base && address - h->ram[i].base < h->ram[i].size &&
            size <= h->ram[i].size - (address - h->ram[i].base))
            return true;
    }
    return false;
}
static uint64_t stacked_address(uint64_t bsp, unsigned offset)
{
    if (((bsp >> 3) & 63) == 63)
        bsp += 8;
    while (offset--) {
        bsp += 8;
        if (((bsp >> 3) & 63) == 63)
            bsp += 8;
    }
    return bsp;
}
static unsigned fp_context(unsigned reg)
{
    return reg < 32 ? (IPF_CONTEXT_F2 - 16) / 8 + (reg - 2) * 2 : (4096 - 16) / 8 + (reg - 32) * 2;
}
void unaligned_dispatch(uint64_t vector, uint64_t *context)
{
    (void)vector;
    struct unaligned_frame f;
    volatile uint8_t *clear = (volatile uint8_t *)&f;
    for (unsigned i = 0; i < sizeof(f); ++i)
        clear[i] = 0;
    uint64_t addresses[96];
    for (unsigned i = 1; i < 32; ++i)
        f.gr[i] = context[i];
    f.nat = context[(IPF_CONTEXT_IntNat - 16) / 8];
    f.ipsr = context[(IPF_CONTEXT_CrIpsr - 16) / 8];
    f.isr = context[(IPF_CONTEXT_CrIsr - 16) / 8];
    f.cfm = context[(IPF_CONTEXT_CrIfs - 16) / 8];
    f.unat = context[(IPF_CONTEXT_ArUnat - 16) / 8];
    f.iip = context[(IPF_CONTEXT_CrIip - 16) / 8];
    f.ifa = context[(IPF_CONTEXT_CrIfa - 16) / 8];
    unsigned sof = f.cfm & 127, sor = ((f.cfm >> 14) & 15) * 8, rrb = (f.cfm >> 18) & 127;
    /* This runs with PSR.dt=0 (ivt.S), so every access below is physical.
     * IPSR.dt=1: the faulting address is virtual. IPSR.rt=1: the stacked
     * slots and the RNAT word are RSE addresses, translated (SDM Vol.2
     * §3.3.2 Table 3-2, PSR.rt), even when data translation is off. Either
     * way the access would land on unrelated physical memory: stop. */
    if ((f.ipsr & ((UINT64_C(1) << 17) | (UINT64_C(1) << 27))) || sof > 96 || sor > sof ||
        (sor && rrb >= sor))
        goto panic;
    uint64_t bsp = context[(IPF_CONTEXT_ArBsp - 16) / 8];
    uint64_t bspstore = context[(CONTROL_RseBspstore - 16) / 8],
             rnat = context[(CONTROL_RseRnat - 16) / 8];
    for (unsigned i = 0; i < sof; ++i) {
        unsigned offset = i < sor ? (i + rrb) % sor : i;
        uint64_t a = stacked_address(bsp, offset), word = a | 0x1f8;
        if (!ram(a, 8) || (word < bspstore && !ram(word, 8)))
            goto panic;
        addresses[i] = a;
        uint64_t bits = word >= bspstore ? rnat : *(volatile uint64_t *)(uintptr_t)word;
        set_gr(&f, 32 + i, *(volatile uint64_t *)(uintptr_t)a, (bits >> ((a >> 3) & 63)) & 1);
    }
    f.fp[1][0] = UINT64_C(1) << 63;
    f.fp[1][1] = 0xffff;
    for (unsigned i = 2; i < 128; ++i) {
        unsigned at = fp_context(i);
        f.fp[i][0] = context[at];
        f.fp[i][1] = context[at + 1];
    }
    if (!ram(f.iip, 16))
        goto panic;
    const volatile uint8_t *bundle = (const volatile uint8_t *)(uintptr_t)f.iip;
    uint64_t lo = read_bytes(bundle, 8), hi = read_bytes(bundle + 8, 8);
    unsigned slot = (unsigned)(f.ipsr >> 41) & 3;
    if (!unaligned_m_slot((unsigned)lo & 31, slot))
        goto panic;
    uint64_t raw = slot == 0 ? lo >> 5 : slot == 1 ? (lo >> 46) | (hi << 18) : hi >> 23;
    raw &= UINT64_C(0x1ffffffffff);
    unsigned major = raw >> 37, x6 = (raw >> 30) & 63;
    bool fp = major == 6 || major == 7;
    unsigned width = fp ? (x6 & 3) == 0 ? 10 : (x6 & 3) == 2 ? 4 : 8 : 1u << (x6 & 3);
    if (x6 == 0x1b || x6 == 0x3b)
        width = fp ? 16 : 8;
    bool speculative = x6 >= 4 && x6 <= 15 && (x6 & 4);
    if ((!speculative && !ram(f.ifa, width)) ||
        !unaligned_emulate(&f, raw, (volatile uint8_t *)(uintptr_t)f.ifa))
        goto panic;
    for (unsigned i = 1; i < 32; ++i)
        context[i] = f.gr[i];
    context[(IPF_CONTEXT_IntNat - 16) / 8] = f.nat;
    context[(IPF_CONTEXT_ArUnat - 16) / 8] = f.unat;
    context[(IPF_CONTEXT_CrIpsr - 16) / 8] = f.ipsr;
    context[(IPF_CONTEXT_CrIip - 16) / 8] = f.iip;
    /* cover/flushrs made the interrupted frame memory-resident. Update the
     * physical slots and RNAT, then rfi refills it with the original rotation. */
    for (unsigned i = 0; i < sof; ++i) {
        uint64_t a = addresses[i], word = a | 0x1f8, bit = UINT64_C(1) << ((a >> 3) & 63);
        *(volatile uint64_t *)(uintptr_t)a = f.gr[32 + i];
        if (word < bspstore) {
            volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)word;
            *p = (*p & ~bit) | (nat(&f, 32 + i) ? bit : 0);
        } else
            rnat = (rnat & ~bit) | (nat(&f, 32 + i) ? bit : 0);
    }
    context[(CONTROL_RseRnat - 16) / 8] = rnat;
    /* High FRs were saved with the interrupted rrb.fr, and are restored
     * with CFM=0 before rfi. Arrange the spill image in physical order. */
    unsigned fr_rotation = (f.cfm >> 25) & 127;
    for (unsigned i = 2; i < 128; ++i) {
        unsigned physical = i < 32 ? i : 32 + (i - 32 + fr_rotation) % 96;
        unsigned at = fp_context(physical);
        context[at] = f.fp[i][0];
        context[at + 1] = f.fp[i][1];
    }
    return;
panic:
    *(volatile uint64_t *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_DEBUG) = f.iip;
    *(volatile uint64_t *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_DEBUG) = f.ifa;
    *(volatile uint64_t *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_PANIC) = 0x47335541;
    for (;;)
        __asm__ volatile("hint @pause");
}
#endif
