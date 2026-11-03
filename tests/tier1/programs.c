/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The guest programs: setup and self-checks; see programs.h. */
#include <stdio.h>
#include <string.h>
#include "tests/tier1/programs.h"
#include "ia64/tlb.h"

const struct guest_program guest_programs[] = {
    {"gather_phys", "gather_phys.bin", GUEST_GATHER, 0},
    {"gather_phys_ss", "gather_phys.bin", GUEST_GATHER, GUEST_SINGLE_STEP},
    {"gather_virt", "gather_virt.bin", GUEST_GATHER, GUEST_VIRT},
    {"gather_virt_stride", "gather_virt_stride.bin", GUEST_GATHER, GUEST_VIRT | GUEST_STRIDE},
    {"gather_rot", "gather_rot.bin", GUEST_GATHER, 0},
    {"gather_rotp", "gather_rotp.bin", GUEST_GATHER, 0},
    {"gather_2page", "gather_2page.bin", GUEST_GATHER, 0},
    {"gather_load_after_store", "gather_load_after_store.bin", GUEST_GATHER,
     GUEST_LOAD_AFTER_STORE},
    {"gather_alias", "gather_alias.bin", GUEST_GATHER, GUEST_VIRT | GUEST_ALIAS},
    {"gather_alias_phys63", "gather_alias.bin", GUEST_GATHER, GUEST_PHYS63},
    {"shladd_xor", "shladd_xor.bin", GUEST_SHLADD_XOR, 0},
    {"mid_block", "mid_block.bin", GUEST_MID_BLOCK, 0},
    {"window_tags", "window_tags.bin", GUEST_WINDOW_TAGS,
     0}, /* virtual mode, but no guest faults */
    {"rse_return", "rse_return.bin", GUEST_RSE_RETURN,
     0}, /* br.ret onto a loop header, frame unloaded */
    {"rse_recursion", "rse_recursion.bin", GUEST_RSE_RETURN,
     GUEST_RSE_RECURSION}, /* ... at 12-14 frame bases, one wrapping */
    {"mmu_log_writers", "mmu_log_writers.bin", GUEST_MMU_LOG_WRITERS,
     0}, /* every MMU-log writer between region entries */
    {"stitched_outer", "stitched_outer.bin", GUEST_STITCHED_OUTER,
     0}, /* two back-edges: inner, stitched outer */
    {"loop_nest", "loop_nest.bin", GUEST_LOOP_NEST,
     0}, /* a loop nest: inner and outer back-edges */
    {"call_return", "call_return.bin", GUEST_CALL_RETURN,
     0}, /* calls, returns, alloc and chk.s in a loop */
    {"rse_spill", "rse_spill.bin", GUEST_RSE_SPILL, 0}, /* allocs that spill inside regions */
    {"rse_spill_virt", "rse_spill.bin", GUEST_RSE_SPILL,
     GUEST_VIRT}, /* ... backing store translated, pages purged */
    {"callee_pages", "callee_pages.bin", GUEST_CALLEE_PAGES,
     0}, /* calls to other code pages, one rewritten */
    {"callee_pages_virt", "callee_pages.bin", GUEST_CALLEE_PAGES,
     GUEST_VIRT}, /* ... under one ITR, then remapped */
    {"callee_pages_early", "callee_pages.bin", GUEST_CALLEE_PAGES,
     GUEST_VIRT | GUEST_EARLY_REMAP}, /* ... remapped before it is hot */
    {"speed", "speed.bin", GUEST_SPEED, 0},
};
const unsigned guest_program_count = IA64VMM_ARRAY_SIZE(guest_programs);

const struct guest_program *guest_program(const char *name)
{
    for (unsigned i = 0; i < guest_program_count; ++i)
        if (!strcmp(guest_programs[i].name, name))
            return &guest_programs[i];
    return NULL;
}

struct guest_params guest_sweep_params(const struct guest_program *p)
{
    if (p->kind == GUEST_SHLADD_XOR)
        return (struct guest_params){.n = 16, .passes = 1, .seed = 2};
    if (p->kind == GUEST_MID_BLOCK || p->kind == GUEST_WINDOW_TAGS)
        return (struct guest_params){.n = 6, .passes = 1, .seed = 5};
    /* rse-recursion: 12 levels reach BOF 90 (level 10), whose frame wraps. */
    if (p->kind == GUEST_RSE_RETURN && (p->flags & GUEST_RSE_RECURSION))
        return (struct guest_params){.n = 3, .passes = 12, .seed = 10};
    if (p->kind == GUEST_RSE_RETURN)
        return (struct guest_params){.n = 8, .passes = 3, .seed = 8};
    if (p->kind == GUEST_MMU_LOG_WRITERS)
        return (struct guest_params){.n = 3, .passes = 1, .seed = 10}; /* 3: one entry per loop */
    if (p->kind == GUEST_STITCHED_OUTER)
        return (struct guest_params){.n = 4, .passes = 3, .seed = 12};
    if (p->kind == GUEST_LOOP_NEST)
        return (struct guest_params){.n = 3, .passes = 4, .seed = 14};
    /* call-return: passes is the recursion depth; 20 levels make alloc spill.
     * Iterations 2, 3 and 6 run the whole body: three occurrences of every
     * point of the header's region. */
    if (p->kind == GUEST_CALL_RETURN)
        return (struct guest_params){.n = 7, .passes = 20, .seed = 16};
    /* rse-spill: every depth 0..8 once, and the first again. */
    if (p->kind == GUEST_RSE_SPILL)
        return (struct guest_params){.n = 10, .passes = 1, .seed = 18};
    /* callee-pages: passes is the rewrite iteration; the remap follows two
     * iterations later. */
    if (p->kind == GUEST_CALLEE_PAGES)
        return (struct guest_params){.n = 6, .passes = 1, .seed = 20};
    return (struct guest_params){.n = 8, .passes = 2, .seed = 1};
}

struct guest_params guest_completion_params(const struct guest_program *p)
{
    if (p->kind == GUEST_SHLADD_XOR)
        return (struct guest_params){.n = 1000, .passes = 1, .seed = 3};
    if (p->kind == GUEST_MID_BLOCK)
        return (struct guest_params){.n = 400, .passes = 1, .seed = 6};
    if (p->kind == GUEST_WINDOW_TAGS)
        return (struct guest_params){.n = 100, .passes = 1, .seed = 7};
    if (p->kind == GUEST_RSE_RETURN && (p->flags & GUEST_RSE_RECURSION))
        return (struct guest_params){.n = 40, .passes = 14, .seed = 11};
    if (p->kind == GUEST_RSE_RETURN)
        return (struct guest_params){.n = 64, .passes = 6, .seed = 9};
    if (p->kind == GUEST_MMU_LOG_WRITERS)
        return (struct guest_params){.n = 12, .passes = 1, .seed = 11};
    if (p->kind == GUEST_STITCHED_OUTER)
        return (struct guest_params){.n = 40, .passes = 6, .seed = 13};
    /* Past the product loop threshold (64) at the outer header. */
    if (p->kind == GUEST_LOOP_NEST)
        return (struct guest_params){.n = 6, .passes = 120, .seed = 15};
    /* call-return: 18 levels of sol 5 beyond the caller's 10 fill the
     * 96-register ring, so alloc spills (SDM Vol.2 §6.5). */
    if (p->kind == GUEST_CALL_RETURN)
        return (struct guest_params){.n = 40, .passes = 20, .seed = 17};
    /* rse-spill: five cycles of the depths 0..8. */
    if (p->kind == GUEST_RSE_SPILL)
        return (struct guest_params){.n = 45, .passes = 1, .seed = 19};
    if (p->kind == GUEST_CALLEE_PAGES)
        return (struct guest_params){.n = 300, .passes = 100, .seed = 21};
    return (struct guest_params){.n = (p->flags & GUEST_STRIDE) ? 40 : 48, .passes = 3, .seed = 4};
}

struct vcpu_config guest_vcpu_config(const struct guest_program *p)
{
    (void)p;
    return (struct vcpu_config){.engine = ENGINE_INTERP, .itc_icount = true};
}

/* ---- gather data ----------------------------------------------------------- */

#define SENTINEL UINT64_C(0xf1f1f1f1f1f1f1f1)
#define THRESHOLD (UINT64_C(1) << 63)
#define OUT_START (T1H_OUT + 0xfe0) /* the 5th store crosses into the next page */
#define STATUS_VALUE UINT64_C(0x5354415455535f31)
#define VRN(n) ((uint64_t)(n) << 61)
/* PTE flags (SDM Vol.2 §4.1.1.5): p = 1, ma = WB, a = 0x20, d = 0x40, ar << 9. */
#define PTE_P 1u
#define PTE_A 0x20u
#define PTE_D 0x40u
#define PTE_AR(x) ((uint64_t)(x) << 9)

struct gather_layout {
    unsigned table_pages, sentinel_iteration;
    uint64_t index_gva, table_gva, out_gva, status_gva;
    uint64_t code_vrn_alt, out_vrn, out_vrn_alt;
};

static uint64_t mix(uint64_t x)
{
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static uint64_t index_value(const struct guest_program *p, const struct guest_params *q,
                            const struct gather_layout *l, unsigned i)
{
    if (i % 6 == 1)
        return 0; /* skip iteration */
    uint64_t r = mix(q->seed * 1000003 + i);
    if (p->flags & GUEST_STRIDE)
        return (uint64_t)(i + 1) * 512 + r % 512;
    return 1 + r % (l->table_pages * UINT64_C(512) - 1);
}

static uint64_t table_value(const struct guest_program *p, const struct guest_params *q,
                            const struct gather_layout *l, uint64_t index)
{
    if (index == index_value(p, q, l, l->sentinel_iteration))
        return SENTINEL;
    uint64_t v = mix(q->seed * 7919 + index * 31 + 17) | 1;
    return v == SENTINEL ? v ^ 2 : v;
}

static void gather_layout(const struct guest_program *p, const struct guest_params *q,
                          struct gather_layout *l)
{
    bool virt = p->flags & GUEST_VIRT;
    T1H_CHECK(q->n >= 4 && q->passes >= 1);
    T1H_CHECK(q->n * 8 <= 0x2000);
    *l = (struct gather_layout){
        .table_pages = (p->flags & GUEST_STRIDE) ? q->n + 2
                       : virt                    ? 40
                                                 : 8,
        .sentinel_iteration = (q->n - 2) % 6 == 1 ? q->n - 3 : q->n - 2,
        .index_gva = (virt ? VRN(1) : 0) | T1H_INDEX,
        .table_gva = virt ? VRN(2) | T1H_TABLE_VA : T1H_TABLE,
        .out_gva = (virt ? VRN(3) : 0) | OUT_START,
        .status_gva = T1H_STATUS,
        .code_vrn_alt = (p->flags & GUEST_ALIAS) ? 4 : 0,
        .out_vrn = virt ? 3 : 0,
        .out_vrn_alt = (p->flags & GUEST_ALIAS) ? 5 : 0,
    };
    T1H_CHECK(l->table_pages <= T1H_TABLE_PAGES);
    uint64_t stores = q->bench ? q->n : (uint64_t)q->n * q->passes;
    T1H_CHECK(OUT_START + stores * 8 <= T1H_OUT + T1H_OUT_PAGES * UINT64_C(4096));
}

/* Direct writes of TR and RR log like the CPU's writers (the Tier1 harness runs hostwin's completeness check). */
static void tr(struct vcpu *v, unsigned side, unsigned slot, uint64_t va, gpa_t pa, unsigned ps,
               uint64_t flags)
{
    T1H_CHECK(!(pa & ia64_low_mask(ps)));
    v->s.tr[side][slot] = (struct ia64_tlb_entry){.va = va & ia64_low_mask(61) & ~ia64_low_mask(ps),
                                                  .pte = pa | flags,
                                                  .itir = (uint64_t)ps << 2,
                                                  .rid = (v->s.rr[va >> 61] >> 8) & 0xffffff,
                                                  .page_shift = ps,
                                                  .valid = true};
    ia64_mmu_changed(v, IA64_MMU_TR, side, slot);
}
static void set_rr(struct vcpu *v, unsigned r, uint64_t value)
{
    v->s.rr[r] = value;
    ia64_mmu_changed(v, IA64_MMU_RR, 0, r);
}

/*
 * gather-virt guest MMU:
 *   code + IVT: ITR0, VRN 0, 64 KiB at 0x100000 (gather-alias: also ITR1, VRN 4)
 *   index array (+ status page): DTR0, VRN 1, 64 KiB at 0x110000
 *   table: VRN 2, VHPT walker enabled (rr.ve). Its first 8 pages hash into
 *     VHPT page 0 (DTR1, present entries: walker-filled); the others hash
 *     into VHPT page 1, mapped with PTE.a = 0 (DTR2): the walker's own
 *     reference would fault, so ia64/mmu.c aborts the walk with the
 *     original Data TLB fault (an implementation-specific abort, which SDM
 *     Vol.2 Table 4-10 permits) and the IVT's 0x0800 handler inserts the
 *     page with itc.d. A VHPT entry with p = 0 is still installed by the
 *     walker (SDM Vol.2 §4.1.5.2) and raises Page Not Present (0x5000).
 *   output: VRN 3 (gather-alias: also 5), walker disabled -> Alternate Data TLB
 *     handler inserts with d = 0 -> the first store per page takes Dirty-Bit.
 * VA payload == GPA for every data page except the table (VA payload
 * T1H_TABLE_VA, PA T1H_TABLE); the handlers add bank-0 r30 for it.
 */
static void gather_virtual_mmu(const struct guest_program *p, struct vcpu *v)
{
    static const uint64_t rid[6] = {0x100, 0x101, 0x102, 0x103, 0x104, 0x105};
    static const unsigned ps[6] = {16, 12, 12, 12, 16, 12};
    for (unsigned r = 0; r < 6; ++r)
        set_rr(v, r, (rid[r] << 8) | ((uint64_t)ps[r] << 2) | (r == 2 ? 1 : 0));
    v->s.cr[CR_PTA] = T1H_VHPT | (15u << 2) | 1; /* 32 KiB, short format, ve */
    uint64_t rwx = PTE_P | PTE_A | PTE_D | PTE_AR(3), rw = PTE_P | PTE_A | PTE_D | PTE_AR(2);
    tr(v, 0, 0, T1H_CODE, T1H_CODE, 16, rwx);
    if (p->flags & GUEST_ALIAS)
        tr(v, 0, 1, VRN(4) | T1H_CODE, T1H_CODE, 16, rwx);
    tr(v, 1, 0, VRN(1) | T1H_INDEX, T1H_INDEX, 16, rw);
    tr(v, 1, 1, VRN(2) | T1H_VHPT, T1H_VHPT, 12, rw);
    tr(v, 1, 2, VRN(2) | (T1H_VHPT + 0x1000), T1H_VHPT + 0x1000, 12, rw & ~(uint64_t)PTE_A);
}

static void gather_virtual_vhpt(const struct gather_layout *l)
{
    uint8_t *vhpt = t1h_ram() + (T1H_VHPT - T1H_RAM_BASE);
    memset(vhpt, 0, 0x8000);
    for (unsigned page = 0; page < l->table_pages; ++page) {
        uint64_t va = (l->table_gva & ia64_low_mask(61)) + page * UINT64_C(4096);
        uint64_t offset = ((va >> 12) * 8) & 0x7fff;
        if (offset >= 0x1000)
            continue; /* VHPT page 1: walker aborts */
        uint64_t pte = (T1H_TABLE + page * UINT64_C(4096)) | PTE_P | PTE_A | PTE_AR(2);
        memcpy(vhpt + offset, &pte, 8);
    }
    code_notify_vmm_write(T1H_VHPT, 0x8000);
}

static void gather_setup(const struct guest_program *p, const struct guest_params *q,
                         struct vcpu *v)
{
    struct gather_layout l;
    gather_layout(p, q, &l);
    uint8_t *ram = t1h_ram();
    for (unsigned i = 0; i < q->n; ++i) {
        uint64_t x = index_value(p, q, &l, i);
        memcpy(ram + (T1H_INDEX - T1H_RAM_BASE) + 8 * i, &x, 8);
    }
    for (uint64_t i = 0; i < l.table_pages * UINT64_C(512); ++i) {
        uint64_t x = table_value(p, q, &l, i);
        memcpy(ram + (T1H_TABLE - T1H_RAM_BASE) + 8 * i, &x, 8);
    }
    uint64_t status = STATUS_VALUE;
    memcpy(ram + (T1H_STATUS - T1H_RAM_BASE), &status, 8);
    code_notify_vmm_write(T1H_INDEX, T1H_TABLE + l.table_pages * UINT64_C(4096) - T1H_INDEX);

    bool virt = p->flags & GUEST_VIRT;
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE | ((p->flags & GUEST_PHYS63) ? UINT64_C(1) << 63 : 0),
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN |
               (virt ? IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_RT : 0) |
               ((p->flags & GUEST_SINGLE_STEP) ? IA64_PSR_SS : 0),
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    regs.gr[2] = l.table_gva;
    regs.gr[3] = l.out_gva;
    regs.gr[4] = THRESHOLD;
    regs.gr[5] = SENTINEL;
    regs.gr[6] = l.status_gva;
    regs.gr[7] = l.code_vrn_alt;
    regs.gr[8] = q->n - 1;
    regs.gr[9] = q->passes;
    regs.gr[10] = q->bench ? 1 : 0;
    regs.gr[11] = l.index_gva;
    regs.gr[12] = l.out_vrn;
    regs.gr[13] = l.out_vrn_alt;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
    if (virt) {
        gather_virtual_mmu(p, v);
        gather_virtual_vhpt(&l);
        /* Bank-0 r30 (PSR.bn = 1 keeps it in bank_gr): the IVT's region-2
         * fill offset, table PA - table VA payload (programs/ivt.inc). */
        v->s.bank_gr[30 - 16] = T1H_TABLE - T1H_TABLE_VA;
    }
}

/* What the gather loop computes (tests/tier1/programs/gather.inc), from the data alone. */
struct gather_model {
    uint64_t sum, skips, found_lc, out_gva, outputs;
    unsigned table_handler_pages, out_pages;
};

static bool seen(uint64_t *set, unsigned *n, unsigned cap, uint64_t key)
{
    for (unsigned i = 0; i < *n; ++i)
        if (set[i] == key)
            return true;
    T1H_CHECK(*n < cap);
    set[(*n)++] = key;
    return false;
}

static bool gather_model(const struct guest_program *p, const struct guest_params *q,
                         struct gather_model *m, char *why, size_t why_size)
{
    struct gather_layout l;
    gather_layout(p, q, &l);
    *m = (struct gather_model){.out_gva = l.out_gva};
    uint64_t table_pages[64], out_pages[64];
    unsigned nt = 0, no = 0;
    const uint8_t *ram = t1h_ram();
    for (unsigned pass = 0; pass < q->passes; ++pass) {
        bool last = pass + 1 == q->passes;
        if (q->bench)
            m->out_gva = (l.out_gva & ia64_low_mask(61)) | (m->out_gva & ~ia64_low_mask(61));
        for (unsigned i = 0; i < q->n; ++i) {
            uint64_t index = index_value(p, q, &l, i);
            if (!index) {
                ++m->skips;
                continue;
            }
            uint64_t value = table_value(p, q, &l, index);
            m->sum += value;
            uint64_t tva = l.table_gva + index * 8;
            if ((tva & ia64_low_mask(61)) >= 0x200000) {
                if (!seen(table_pages, &nt, 64, tva >> 12))
                    ++m->table_handler_pages;
            }
            if (value < THRESHOLD) {
                uint64_t gpa = m->out_gva & ia64_low_mask(61);
                uint64_t got;
                memcpy(&got, ram + (gpa - T1H_RAM_BASE), 8);
                if (got != value && !q->bench) {
                    snprintf(why, why_size, "output %#llx = %#llx, expected %#llx",
                             (unsigned long long)m->out_gva, (unsigned long long)got,
                             (unsigned long long)value);
                    return false;
                }
                if (!seen(out_pages, &no, 64, m->out_gva >> 12))
                    ++m->out_pages;
                m->out_gva += 8;
                ++m->outputs;
            } else if (last && value == SENTINEL) {
                m->found_lc = q->n - 1 - i;
                break;
            }
        }
        /* gather-alias: the output alias switches with the code alias. */
        if (l.code_vrn_alt && !last)
            m->out_gva =
                (m->out_gva & ia64_low_mask(61)) | VRN(pass % 2 ? l.out_vrn : l.out_vrn_alt);
    }
    return true;
}

/* ---- shladd_xor ---------------------------------------------------------------- */

static void shladd_xor_setup(const struct guest_params *q, struct vcpu *v)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    regs.gr[2] = mix(q->seed);
    regs.gr[3] = mix(q->seed + 1);
    regs.gr[4] = mix(q->seed + 2);
    regs.gr[8] = q->n - 1;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
}

/* ---- mid-block-exit, window-tags ------------------------------------ */

#define TMID_DATA T1H_INDEX /* up to 1024 words */

static void tmid_setup(const struct guest_params *q, struct vcpu *v)
{
    T1H_CHECK(q->n >= 1 && q->n <= 1024);
    for (unsigned i = 0; i < q->n; ++i) {
        uint64_t x = mix(q->seed * 131 + i);
        memcpy(t1h_ram() + (TMID_DATA - T1H_RAM_BASE) + 8 * i, &x, 8);
    }
    code_notify_vmm_write(TMID_DATA, 8 * q->n);
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    regs.gr[2] = mix(q->seed);
    regs.gr[3] = mix(q->seed + 1);
    regs.gr[8] = q->n - 1;
    regs.gr[11] = TMID_DATA;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
}

/* Twelve data pages, one per tag: VRN 1..7 and payload bit 40 (hosted
 * windows: w = 40 or 36, so every address has its own tag), each mapped by
 * a D-TC entry to a page of the table area. */
#define TTAGS_N 12u
static uint64_t ttags_va(unsigned k)
{
    return VRN(1 + k % 7) | ((uint64_t)(k / 7) << 40) | (UINT64_C(0x200000) + k * UINT64_C(0x1000));
}
static gpa_t ttags_pa(unsigned k)
{
    return T1H_TABLE + k * UINT64_C(4096);
}

static void ttags_setup(const struct guest_params *q, struct vcpu *v)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN | IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_RT,
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    for (unsigned k = 0; k < TTAGS_N; ++k) {
        uint64_t x = mix(q->seed * 977 + k);
        memcpy(t1h_ram() + (ttags_pa(k) - T1H_RAM_BASE), &x, 8);
        regs.gr[2 + k] = ttags_va(k);
    }
    code_notify_vmm_write(T1H_TABLE, TTAGS_N * UINT64_C(4096));
    regs.gr[14] = q->n - 1;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
    for (unsigned r = 0; r < 8; ++r)
        set_rr(v, r, ((UINT64_C(0x200) + r) << 8) | (12u << 2));
    tr(v, 0, 0, T1H_CODE, T1H_CODE, 16, PTE_P | PTE_A | PTE_D | PTE_AR(3));
    for (unsigned k = 0; k < TTAGS_N; ++k)
        ia64_tc_insert(v, ttags_va(k), 1, ttags_pa(k) | PTE_P | PTE_A | PTE_D | PTE_AR(2),
                       12u << 2);
}

/* ---- mmu-log-writers -------------------------------- */

/* programs/mmu_log_writers.S. Data pages in the table area: A (region 1, VA payload = PA,
 * so the Alternate Data TLB handler refills it), B (region 3, DTR 0),
 * W's 64 KiB page (region 6, VHPT walker; the loads read W + 0x3000). The
 * TR and I-side VAs are never accessed. */
#define MMU_LOG_PROG_A UINT64_C(0x1c0000)
#define MMU_LOG_PROG_B UINT64_C(0x1c1000)
#define MMU_LOG_PROG_W3 UINT64_C(0x1c3000)
#define MMU_LOG_PROG_LOOPS_AB 13u /* loops whose body loads A and B */
#define MMU_LOG_PROG_REP 12u      /* loads of each per iteration */
#define MMU_LOG_PROG_KEY(k) ((uint64_t)(k) << 8 | 1)

static uint64_t mmu_log_prog_word(const struct guest_params *q, unsigned k)
{
    return mix(q->seed * 389 + k);
}

static void mmu_log_prog_setup(const struct guest_params *q, struct vcpu *v)
{
    T1H_CHECK(q->n >= 2);
    uint8_t *ram = t1h_ram();
    const gpa_t data[3] = {MMU_LOG_PROG_A, MMU_LOG_PROG_B, MMU_LOG_PROG_W3};
    for (unsigned k = 0; k < 3; ++k) {
        uint64_t x = mmu_log_prog_word(q, k);
        memcpy(ram + (data[k] - T1H_RAM_BASE), &x, 8);
    }
    code_notify_vmm_write(MMU_LOG_PROG_A, MMU_LOG_PROG_W3 + 8 - MMU_LOG_PROG_A);
    uint64_t rw = PTE_P | PTE_A | PTE_D | PTE_AR(2);
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN | IA64_PSR_IT | IA64_PSR_DT,
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    regs.gr[2] = VRN(1) | MMU_LOG_PROG_A;
    regs.gr[3] = VRN(3) | MMU_LOG_PROG_B;
    regs.gr[4] = VRN(4) | 0x1c2000;
    regs.gr[5] = VRN(4) | 0x1c4000;
    regs.gr[6] = VRN(4) | 0x1c6000;
    regs.gr[7] = VRN(7) | 0x1c5000;
    regs.gr[8] = VRN(7) | 0x1c7000;
    regs.gr[9] = 12u << 2;
    regs.gr[10] = 16u << 2;
    regs.gr[11] = UINT64_C(0x1c2000) | rw;
    regs.gr[12] = MMU_LOG_PROG_A | rw;
    regs.gr[13] = VRN(6) | MMU_LOG_PROG_W3;
    regs.gr[14] = q->n - 1;
    regs.gr[17] = VRN(6) | MMU_LOG_PROG_A;
    regs.gr[18] = VRN(5);
    regs.gr[19] = (UINT64_C(0x205) << 8) | (12u << 2);
    regs.gr[20] = (UINT64_C(0x305) << 8) | (12u << 2);
    regs.gr[21] = MMU_LOG_PROG_KEY(0x10);
    regs.gr[22] = MMU_LOG_PROG_KEY(0x11);
    regs.gr[23] = MMU_LOG_PROG_KEY(0x12);
    regs.gr[24] = MMU_LOG_PROG_A | rw;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
    for (unsigned r = 0; r < 8; ++r)
        set_rr(v, r, ((UINT64_C(0x200) + r) << 8) | ((r == 6 ? 16u : 12u) << 2) | (r == 6 ? 1 : 0));
    tr(v, 0, 0, T1H_CODE, T1H_CODE, 16, PTE_P | PTE_A | PTE_D | PTE_AR(3));
    tr(v, 1, 0, VRN(3) | MMU_LOG_PROG_B, MMU_LOG_PROG_B, 12, rw);
    tr(v, 1, 1, VRN(6) | T1H_VHPT, T1H_VHPT, 12, rw);
    /* Short-format VHPT (SDM Vol.2 §4.1.5.3), 32 KiB at T1H_VHPT; W's
     * entry: (VA payload >> 16) * 8. */
    v->s.cr[CR_PTA] = T1H_VHPT | (15u << 2) | 1;
    uint8_t *vhpt = ram + (T1H_VHPT - T1H_RAM_BASE);
    memset(vhpt, 0, 0x8000);
    uint64_t w = MMU_LOG_PROG_A | rw;
    memcpy(vhpt + ((MMU_LOG_PROG_A >> 16) * 8 & 0x7fff), &w, 8);
    code_notify_vmm_write(T1H_VHPT, 0x8000);
}

/* ---- stitched-outer ----------------------------------------- */

#define TOUTER_DATA T1H_INDEX /* up to 1024 words */

static uint64_t touter_word(const struct guest_params *q, unsigned i)
{
    return mix(q->seed * 433 + i);
}

static void touter_setup(const struct guest_params *q, struct vcpu *v)
{
    T1H_CHECK(q->n >= 1 && q->n <= 1024 && q->passes >= 1);
    for (unsigned i = 0; i < q->n; ++i) {
        uint64_t x = touter_word(q, i);
        memcpy(t1h_ram() + (TOUTER_DATA - T1H_RAM_BASE) + 8 * i, &x, 8);
    }
    code_notify_vmm_write(TOUTER_DATA, 8 * q->n);
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    regs.gr[8] = q->passes;
    regs.gr[9] = q->n - 1;
    regs.gr[11] = TOUTER_DATA;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
}

/* ---- loop-nest -------------------------------------------- */

#define TNEST_INNER T1H_INDEX /* up to 512 words */
#define TNEST_OUTER T1H_TABLE /* one word per pass */
#define TNEST_OUTPUT T1H_OUT  /* one word per pass */

static uint64_t tnest_word(const struct guest_params *q, unsigned i)
{
    return mix(q->seed * 977 + i);
}
static uint64_t tnest_outer(const struct guest_params *q, unsigned k)
{
    return mix(q->seed * 977 + 4096 + k);
}

/* call-return: the data word r10 points at; the backing store starts at slot
 * 57 of its 64-slot group, so the frames the calls preserve cross the
 * RNaT collection slot (SDM Vol.2 §6.1). */
#define CALL_PROG_WORD T1H_INDEX
#define CALL_PROG_BSP (T1H_BSP + 8 * 57)
static void call_prog_setup(const struct guest_params *q, struct vcpu *v)
{
    T1H_CHECK(q->n >= 1 && q->passes >= 1);
    uint64_t word = mix(q->seed + 1);
    memcpy(t1h_ram() + (CALL_PROG_WORD - T1H_RAM_BASE), &word, 8);
    code_notify_vmm_write(CALL_PROG_WORD, 8);
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
        .bsp = CALL_PROG_BSP,
        .bspstore = CALL_PROG_BSP,
    };
    regs.gr[2] = mix(q->seed);
    regs.gr[8] = q->n - 1;
    regs.gr[9] = q->passes;
    regs.gr[10] = CALL_PROG_WORD;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
}

/* rse-spill: the word ld8.fill reads, and the backing store starting at
 * slot 32 of its group, 96 slots below a page boundary, so the spills of
 * the deep descents cross RNaT collection slots and that boundary (SDM
 * Vol.2 §6.1). With GUEST_VIRT (PSR.dt = PSR.rt = 1, PSR.it = 0) data and
 * backing store are in VRN 1, whose misses take the Alternate Data TLB
 * vector (VHPT walker off); the program purges the page above the
 * boundary, so a spill that starts below it faults in the middle. */
#define SPILL_PROG_WORD T1H_INDEX
#define SPILL_PROG_BSP (T1H_BSP + 0xd00)
static void spill_prog_setup(const struct guest_program *p, const struct guest_params *q,
                             struct vcpu *v)
{
    T1H_CHECK(q->n >= 1);
    bool virt = p->flags & GUEST_VIRT;
    uint64_t word = mix(q->seed + 1), vrn = virt ? VRN(1) : 0;
    memcpy(t1h_ram() + (SPILL_PROG_WORD - T1H_RAM_BASE), &word, 8);
    code_notify_vmm_write(SPILL_PROG_WORD, 8);
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN | (virt ? IA64_PSR_DT | IA64_PSR_RT : 0),
        .bsp = vrn | SPILL_PROG_BSP,
        .bspstore = vrn | SPILL_PROG_BSP,
    };
    regs.gr[2] = mix(q->seed);
    regs.gr[8] = q->n - 1;
    regs.gr[10] = vrn | SPILL_PROG_WORD;
    regs.gr[11] = UINT64_C(1) << ((SPILL_PROG_WORD >> 3) & 63);
    regs.gr[12] = regs.gr[13] = virt ? vrn | ((SPILL_PROG_BSP & ~UINT64_C(4095)) + 4096) : 0;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
    if (virt)
        for (unsigned r = 0; r < 8; ++r)
            set_rr(v, r, ((UINT64_C(0x300) + r) << 8) | (12u << 2));
}

/* callee-pages: the rewrite after iteration `passes`, the remap (GUEST_VIRT)
 * after iteration 2 * passes (GUEST_EARLY_REMAP: 0). GUEST_VIRT: PSR.it = PSR.dt = 1, the code at
 * VRN 0 and its alias at VRN 4 through 64 KiB ITRs 0 and 1; data misses
 * take the Alternate Data TLB vector (VHPT walker off). .Lremap runs at
 * the alias (programs/callee_pages.S, image offset 0x2400). */
#define CALLEE_PAGES_REMAP_VA (VRN(4) | (T1H_CODE + 0x2400))
static unsigned callee_pages_remap(const struct guest_program *p, const struct guest_params *q)
{
    return p->flags & GUEST_EARLY_REMAP ? 0 : 2 * q->passes;
}
static void callee_pages_setup(const struct guest_program *p, const struct guest_params *q,
                               struct vcpu *v)
{
    bool virt = p->flags & GUEST_VIRT;
    T1H_CHECK(q->n >= 1 && q->passes < q->n);
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN | (virt ? IA64_PSR_IT | IA64_PSR_DT : 0),
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    regs.gr[8] = q->n - 1;
    regs.gr[9] = q->passes;
    regs.gr[10] = virt ? callee_pages_remap(p, q) : ~UINT64_C(0);
    regs.gr[11] = virt ? CALLEE_PAGES_REMAP_VA : 0;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
    if (!virt)
        return;
    for (unsigned r = 0; r < 8; ++r)
        set_rr(v, r, ((UINT64_C(0x400) + r) << 8) | (12u << 2));
    uint64_t rwx = PTE_P | PTE_A | PTE_D | PTE_AR(3);
    tr(v, 0, 0, T1H_CODE, T1H_CODE, 16, rwx);
    tr(v, 0, 1, VRN(4) | T1H_CODE, T1H_CODE, 16, rwx);
}

static void tnest_setup(const struct guest_params *q, struct vcpu *v)
{
    T1H_CHECK(q->n >= 1 && q->n <= 512 && q->passes >= 1 && q->passes <= 4096);
    uint8_t *ram = t1h_ram();
    for (unsigned i = 0; i < q->n; ++i) {
        uint64_t x = tnest_word(q, i);
        memcpy(ram + (TNEST_INNER - T1H_RAM_BASE) + 8 * i, &x, 8);
    }
    for (unsigned k = 0; k < q->passes; ++k) {
        uint64_t x = tnest_outer(q, k);
        memcpy(ram + (TNEST_OUTER - T1H_RAM_BASE) + 8 * k, &x, 8);
    }
    memset(ram + (TNEST_OUTPUT - T1H_RAM_BASE), 0, 8 * (size_t)q->passes);
    code_notify_vmm_write(TNEST_INNER, 8 * q->n);
    code_notify_vmm_write(TNEST_OUTER, 8 * q->passes);
    code_notify_vmm_write(TNEST_OUTPUT, 8 * q->passes);
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    regs.gr[8] = q->passes;
    regs.gr[9] = q->n - 1;
    regs.gr[11] = TNEST_INNER;
    regs.gr[18] = TNEST_OUTER;
    regs.gr[19] = TNEST_OUTPUT;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
}

/* ---- rse-return ---------------------------------------------------- */

static void trse_setup(const struct guest_params *q, struct vcpu *v)
{
    T1H_CHECK(q->n >= 1 && q->passes >= 1);
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    regs.gr[2] = mix(q->seed);
    regs.gr[8] = q->n - 1;
    regs.gr[9] = q->passes;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
}

void guest_setup(const struct guest_program *p, const struct guest_params *q, struct vcpu *v)
{
    t1h_load_image(p->image);
    switch (p->kind) {
    case GUEST_GATHER:
        gather_setup(p, q, v);
        break;
    case GUEST_SHLADD_XOR:
        shladd_xor_setup(q, v);
        break;
    case GUEST_MID_BLOCK:
        tmid_setup(q, v);
        break;
    case GUEST_WINDOW_TAGS:
        ttags_setup(q, v);
        break;
    case GUEST_RSE_RETURN:
        trse_setup(q, v);
        break;
    case GUEST_MMU_LOG_WRITERS:
        mmu_log_prog_setup(q, v);
        break;
    case GUEST_STITCHED_OUTER:
        touter_setup(q, v);
        break;
    case GUEST_LOOP_NEST:
        tnest_setup(q, v);
        break;
    case GUEST_CALL_RETURN:
        call_prog_setup(q, v);
        break;
    case GUEST_RSE_SPILL:
        spill_prog_setup(p, q, v);
        break;
    case GUEST_CALLEE_PAGES:
        callee_pages_setup(p, q, v);
        break;
    case GUEST_SPEED:
        /* As tests/ia64/main.c runs it: physical mode, PSR = 0. */
        vcpu_reset(v, &(struct ia64_entry_regs){.ip = T1H_CODE, .rsc = 3});
        break;
    }
}

#define FAIL(...)                                                                                  \
    do {                                                                                           \
        snprintf(why, why_size, __VA_ARGS__);                                                      \
        return false;                                                                              \
    } while (0)

bool guest_check(const struct guest_program *p, const struct guest_params *q, const struct vcpu *v,
                 const struct vcpu_exit *exit, const struct t1h_logs *logs, char *why,
                 size_t why_size)
{
    if (exit->reason != VCPU_EXIT_DEBUG || logs->last_break_imm != T1H_BREAK_END)
        FAIL("exit %u at %#llx:%u (%s), last break %#llx: %s", exit->reason,
             (unsigned long long)exit->ip, exit->slot, exit->detail ? exit->detail : "-",
             (unsigned long long)logs->last_break_imm,
             logs->last_break_imm == T1H_BREAK_UNEXPECTED ? "unexpected IVT vector"
                                                          : "not the end marker");
    if (p->kind == GUEST_SPEED) {
        if (v->s.gr[8] != 0x2dc6c0 || v->s.gr[9] != 0x5b8d80)
            FAIL("speed: r8 %#llx r9 %#llx", (unsigned long long)v->s.gr[8],
                 (unsigned long long)v->s.gr[9]);
        return true;
    }
    if (p->kind == GUEST_MID_BLOCK) {
        uint64_t r16 = mix(q->seed), r17 = mix(q->seed + 1), r20 = 0;
        for (unsigned i = 0; i < q->n; ++i) {
            uint64_t x = mix(q->seed * 131 + i);
            r16 += r17;
            r17 += r16 << 2;
            r20 += (x & 8) ? x : 2 * x;
            r16 ^= x;
        }
        if (t1h_bank1(v, 16) != r16 || t1h_bank1(v, 17) != r17 || t1h_bank1(v, 20) != r20)
            FAIL("mid-block-exit: r16 %#llx r17 %#llx r20 %#llx, expected %#llx %#llx %#llx",
                 (unsigned long long)t1h_bank1(v, 16), (unsigned long long)t1h_bank1(v, 17),
                 (unsigned long long)t1h_bank1(v, 20), (unsigned long long)r16,
                 (unsigned long long)r17, (unsigned long long)r20);
        return true;
    }
    if (p->kind == GUEST_WINDOW_TAGS) {
        uint64_t sum = 0;
        for (unsigned k = 0; k < TTAGS_N; ++k)
            sum += mix(q->seed * 977 + k);
        if (v->s.gr[15] != sum * q->n)
            FAIL("window-tags: r15 %#llx, expected %#llx", (unsigned long long)v->s.gr[15],
                 (unsigned long long)(sum * q->n));
        return true;
    }
    if (p->kind == GUEST_CALL_RETURN) {
        /* The functions of call_return.S: leaf(a) = 2a + 1, sum(a, b) = leaf(a)
         * + a + 3b, two(a) = sum(a, a + 3) + 7, rec(d) = d(d + 1) / 2. */
        uint64_t r32 = mix(q->seed), word = mix(q->seed + 1), d = q->passes;
        uint64_t r14 = 0, r15 = 0, r16 = 0, r17 = 0, r18 = 0, r33 = 0;
        for (unsigned i = 0; i < q->n; ++i) {
            if ((r33 & 3) == 0)
                r14 += 2 * r32 + 1 + r32 + 3 * r33;
            r15 += 1;
            r14 += 2 * r14 + 1;
            if ((r33 & 3) == 1)
                r14 += 2 * r33 + 1 + r33 + 3 * (r33 + 3) + 7;
            r14 += 2 * r15 + 1;
            r17 += 40;
            r18 += 1;
            r16 += 1;
            r14 += word;
            if (!(r33 & 3))
                r14 += d * (d + 1) / 2;
            r14 += 2 * r33 + 1;
            r33 += 1;
        }
        const struct ia64_state *s = &v->s;
        /* r19: ar.pfs of the call after .Lpfs's return, PFM sof 13, sol 10. */
        uint64_t r19 = 13 | 10 << 7 | UINT64_C(40) << 52;
        if (s->gr[14] != r14 || s->gr[15] != r15 || s->gr[16] != r16 || s->gr[17] != r17 ||
            s->gr[18] != r18 || s->gr[19] != r19 || ia64_gr_read(s, 33) != r33 ||
            s->ar[AR_EC] != 40 || (s->cfm & 0x3fff) != (14 | 10 << 7) || s->rse_missing ||
            s->rse_cfle)
            FAIL(
                "call-return: r14 %#llx r15 %llu r16 %llu r17 %llu r18 %llu r19 %#llx r33 %llu ec %llu cfm %#llx, "
                "expected %#llx %llu %llu %llu %llu %#llx %llu 40 sof 14 sol 10",
                (unsigned long long)s->gr[14], (unsigned long long)s->gr[15],
                (unsigned long long)s->gr[16], (unsigned long long)s->gr[17],
                (unsigned long long)s->gr[18], (unsigned long long)s->gr[19],
                (unsigned long long)ia64_gr_read(s, 33), (unsigned long long)s->ar[AR_EC],
                (unsigned long long)s->cfm, (unsigned long long)r14, (unsigned long long)r15,
                (unsigned long long)r16, (unsigned long long)r17, (unsigned long long)r18,
                (unsigned long long)r19, (unsigned long long)r33);
        return true;
    }
    if (p->kind == GUEST_RSE_SPILL) {
        /* wide1(a) = 2a + 3, wide2(a) = 2a + 7; down(0) = .Lbottom + 1 +
         * 100 with .Lbottom = wide1(5) + wide2(7) + wide1(9); down(k) =
         * down(k - 1) + (3k + 1) + (k + 100). Iteration i adds wide1(1),
         * descends to depth i mod 9 and finds the NaT of each of its d + 1
         * levels. */
        uint64_t r32 = mix(q->seed), word = mix(q->seed + 1), r14 = 0, r15 = 0;
        for (unsigned i = 0; i < q->n; ++i) {
            unsigned d = i % 9;
            uint64_t down = (2 * 5 + 3) + (2 * 7 + 7) + (2 * 9 + 3) + 1 + 100;
            for (unsigned k = 1; k <= d; ++k)
                down += 3 * k + 1 + k + 100;
            r14 += (2 * 1 + 3) + down + r32 + word;
            r15 += d + 1;
        }
        const struct ia64_state *s = &v->s;
        uint64_t alt = t1h_bank0(v, 21), dirty = t1h_bank0(v, 22);
        bool virt = p->flags & GUEST_VIRT;
        if (s->gr[14] != r14 || s->gr[15] != r15 || ia64_gr_read(s, 32) != r32 || s->rse_missing ||
            s->rse_cfle || (virt && (!alt || !dirty)) || (!virt && (alt || dirty)))
            FAIL(
                "rse-spill: r14 %#llx r15 %llu r32 %#llx (missing %u, cfle %d), Alternate Data TLB fills %llu, "
                "Dirty-Bit faults %llu; expected %#llx %llu %#llx",
                (unsigned long long)s->gr[14], (unsigned long long)s->gr[15],
                (unsigned long long)ia64_gr_read(s, 32), s->rse_missing, s->rse_cfle,
                (unsigned long long)alt, (unsigned long long)dirty, (unsigned long long)r14,
                (unsigned long long)r15, (unsigned long long)r32);
        return true;
    }
    if (p->kind == GUEST_CALLEE_PAGES) {
        /* far1(a) = a + c (c = 5; 33 after the rewrite of iteration passes;
         * 77 at the copy after the remap), near(a) = 2a + 1, far2(a) =
         * far1(3a) + 11. */
        bool virt = p->flags & GUEST_VIRT;
        uint64_t r14 = 0;
        for (unsigned i = 0; i < q->n; ++i) {
            uint64_t c = virt && i > callee_pages_remap(p, q) ? 77 : i > q->passes ? 33 : 5;
            r14 += (i + c) + (2 * (uint64_t)i + 1) + (3 * (uint64_t)i + c + 11);
        }
        const struct ia64_state *s = &v->s;
        if (s->gr[14] != r14 || ia64_gr_read(s, 33) != q->n)
            FAIL("callee-pages: r14 %#llx r33 %llu, expected %#llx %u",
                 (unsigned long long)s->gr[14], (unsigned long long)ia64_gr_read(s, 33),
                 (unsigned long long)r14, q->n);
        return true;
    }
    if (p->kind == GUEST_RSE_RETURN && (p->flags & GUEST_RSE_RECURSION)) {
        /* Level k (0 = outermost) loops with r32 = r2 + 7k + 100 (reloaded
         * from the backing store .Lflush changed) and r38 = depth - k - 1;
         * the deepest level loops first. */
        uint64_t r14 = 0, r15 = 0;
        for (unsigned k = q->passes; k-- > 0;) {
            uint64_t a = mix(q->seed) + 7u * k + 100, b = q->passes - k - 1;
            for (unsigned i = 0; i < q->n; ++i) {
                r14 += a;
                r15 += b;
                r14 = (r14 << 1) + b;
            }
        }
        if (v->s.gr[14] != r14 || v->s.gr[15] != r15 || v->s.gr[9] || v->s.gr[11] != q->passes ||
            v->s.rse_missing || v->s.rse_cfle)
            FAIL(
                "rse-recursion: r14 %#llx r15 %#llx r9 %llu r11 %llu (missing %u, cfle %d), expected %#llx %#llx",
                (unsigned long long)v->s.gr[14], (unsigned long long)v->s.gr[15],
                (unsigned long long)v->s.gr[9], (unsigned long long)v->s.gr[11], v->s.rse_missing,
                v->s.rse_cfle, (unsigned long long)r14, (unsigned long long)r15);
        return true;
    }
    if (p->kind == GUEST_RSE_RETURN) {
        /* Each call returns with r32 reloaded from the backing store, where
         * the callee added 100. */
        uint64_t r32 = mix(q->seed), r14 = 0;
        for (unsigned c = 0; c < q->passes; ++c) {
            r32 += 100;
            for (unsigned i = 0; i < q->n; ++i) {
                r14 += r32;
                r14 = (r14 << 1) + r32;
            }
        }
        if (v->s.gr[14] != r14 || ia64_gr_read(&v->s, 32) != r32 || v->s.rse_missing ||
            v->s.rse_cfle)
            FAIL("rse-return: r14 %#llx r32 %#llx (missing %u, cfle %d), expected %#llx %#llx",
                 (unsigned long long)v->s.gr[14], (unsigned long long)ia64_gr_read(&v->s, 32),
                 v->s.rse_missing, v->s.rse_cfle, (unsigned long long)r14, (unsigned long long)r32);
        return true;
    }
    if (p->kind == GUEST_MMU_LOG_WRITERS) {
        uint64_t a = mmu_log_prog_word(q, 0), b = mmu_log_prog_word(q, 1),
                 w = mmu_log_prog_word(q, 2);
        uint64_t sum = (uint64_t)q->n * MMU_LOG_PROG_REP * ((a + b) * MMU_LOG_PROG_LOOPS_AB + w);
        /* A is refilled once at the start and after each of the n purges
         * of the five purge loops (the load after the purge); the walker
         * never gives up. */
        uint64_t alt = t1h_bank0(v, 21), dtlb = t1h_bank0(v, 20);
        bool odd = q->n & 1;
        if (v->s.gr[15] != sum || alt != 1 + 5 * (uint64_t)q->n || dtlb)
            FAIL(
                "mmu-log-writers: r15 %#llx (expected %#llx), Alternate Data TLB fills %llu (expected %llu), "
                "Data TLB fills %llu",
                (unsigned long long)v->s.gr[15], (unsigned long long)sum, (unsigned long long)alt,
                (unsigned long long)(1 + 5 * (uint64_t)q->n), (unsigned long long)dtlb);
        /* The last turn of each alternating loop (p6 on odd turns). */
        uint64_t rr5 =
            odd ? (UINT64_C(0x205) << 8) | (12u << 2) : (UINT64_C(0x305) << 8) | (12u << 2);
        uint64_t pkr5 = odd ? MMU_LOG_PROG_KEY(0x10) : MMU_LOG_PROG_KEY(0x11);
        bool tr4 = v->s.tr[1][4].valid, itr5 = v->s.tr[0][5].valid;
        if (v->s.rr[5] != rr5 || v->s.pkr[5] != pkr5 || tr4 != odd || itr5 != odd ||
            (v->s.pkr[odd ? 6 : 7] & 1) != 1 || (v->s.pkr[odd ? 7 : 6] & 1) != 0)
            FAIL(
                "mmu-log-writers: rr5 %#llx pkr5 %#llx pkr6 %#llx pkr7 %#llx dtr4 %d itr5 %d after %u turns",
                (unsigned long long)v->s.rr[5], (unsigned long long)v->s.pkr[5],
                (unsigned long long)v->s.pkr[6], (unsigned long long)v->s.pkr[7], tr4, itr5, q->n);
        return true;
    }
    if (p->kind == GUEST_STITCHED_OUTER) {
        uint64_t r14 = 0;
        for (unsigned k = 0; k < q->passes; ++k)
            for (unsigned i = 0; i < q->n; ++i)
                r14 = (r14 << 1) + touter_word(q, i);
        if (v->s.gr[14] != r14 || v->s.gr[8] != UINT64_MAX)
            FAIL("stitched-outer: r14 %#llx r8 %#llx, expected %#llx and all passes run",
                 (unsigned long long)v->s.gr[14], (unsigned long long)v->s.gr[8],
                 (unsigned long long)r14);
        return true;
    }
    if (p->kind == GUEST_LOOP_NEST) {
        uint64_t r14 = 0, r15 = 0;
        for (unsigned k = 0; k < q->passes; ++k) {
            for (unsigned i = 0; i < q->n; ++i) {
                r14 = (r14 << 1) + tnest_word(q, i);
                r15 += tnest_outer(q, k);
            }
            uint64_t out;
            memcpy(&out, t1h_ram() + (TNEST_OUTPUT - T1H_RAM_BASE) + 8 * k, 8);
            if (out != r14)
                FAIL("loop-nest: output word %u %#llx, expected %#llx", k, (unsigned long long)out,
                     (unsigned long long)r14);
        }
        if (v->s.gr[14] != r14 || v->s.gr[15] != r15 || v->s.gr[8])
            FAIL("loop-nest: r14 %#llx r15 %#llx r8 %#llx, expected %#llx %#llx and all passes run",
                 (unsigned long long)v->s.gr[14], (unsigned long long)v->s.gr[15],
                 (unsigned long long)v->s.gr[8], (unsigned long long)r14, (unsigned long long)r15);
        return true;
    }
    if (p->kind == GUEST_SHLADD_XOR) {
        uint64_t r16 = mix(q->seed), r17 = mix(q->seed + 1), r18 = mix(q->seed + 2);
        for (unsigned i = 0; i < q->n; ++i) {
            r16 = (r16 << 3) + r17;
            r16 ^= r18;
            r17 += r16;
        }
        if (t1h_bank1(v, 16) != r16 || t1h_bank1(v, 17) != r17)
            FAIL("shladd_xor: r16 %#llx r17 %#llx, expected %#llx %#llx",
                 (unsigned long long)t1h_bank1(v, 16), (unsigned long long)t1h_bank1(v, 17),
                 (unsigned long long)r16, (unsigned long long)r17);
        return true;
    }
    struct gather_model m;
    if (!gather_model(p, q, &m, why, why_size))
        return false;
    const struct ia64_state *s = &v->s;
    if (s->gr[14] != m.sum || s->gr[15] != m.skips)
        FAIL("gather: sum %#llx skips %llu, expected %#llx %llu", (unsigned long long)s->gr[14],
             (unsigned long long)s->gr[15], (unsigned long long)m.sum, (unsigned long long)m.skips);
    if (t1h_bank1(v, 22) != m.found_lc || !m.found_lc)
        FAIL("gather: side exit at ar.lc %llu, expected %llu (the last pass must take .Lfound)",
             (unsigned long long)t1h_bank1(v, 22), (unsigned long long)m.found_lc);
    if (ia64_gr_read(s, 34) != m.out_gva)
        FAIL("gather: r34 %#llx, expected %#llx", (unsigned long long)ia64_gr_read(s, 34),
             (unsigned long long)m.out_gva);
    if (ia64_gr_read(s, 9) != 0)
        FAIL("gather: r9 %llu passes left", (unsigned long long)s->gr[9]);
    if ((p->flags & GUEST_LOAD_AFTER_STORE) && t1h_bank1(v, 19) != STATUS_VALUE)
        FAIL("gather-load-after-store: r19 is not the status word");
    /* Each program instruction traps once; the handler retires 6 slots. */
    if ((p->flags & GUEST_SINGLE_STEP) && t1h_bank0(v, 27) < v->retired / 8)
        FAIL("PSR.ss: %llu single-step traps in %llu instructions",
             (unsigned long long)t1h_bank0(v, 27), (unsigned long long)v->retired);
    if ((p->flags & GUEST_ALIAS) && (exit->ip >> 61) != (q->passes % 2 ? 0u : 4u))
        FAIL("gather-alias: the last pass ran at %#llx", (unsigned long long)exit->ip);
    if ((p->flags & GUEST_PHYS63) && !(exit->ip >> 63))
        FAIL("gather-alias (bit 63): ended at %#llx", (unsigned long long)exit->ip);
    if (p->flags & GUEST_VIRT) {
        uint64_t dtlb = t1h_bank0(v, 20), alt = t1h_bank0(v, 21);
        uint64_t dirty = t1h_bank0(v, 22);
        if (dtlb != m.table_handler_pages || alt != m.out_pages || dirty != m.out_pages)
            FAIL("gather-virt: Data TLB fills %llu (expected %u), Alternate Data TLB %llu and "
                 "Dirty-Bit %llu (expected %u each)",
                 (unsigned long long)dtlb, m.table_handler_pages, (unsigned long long)alt,
                 (unsigned long long)dirty, m.out_pages);
        if (!m.table_handler_pages || !m.out_pages)
            FAIL("gather-virt: the data did not exercise both fill paths");
    }
    return true;
}
