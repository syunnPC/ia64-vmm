/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/tier1/restore.h"

/* SDM Vol.2 register-stack mapping: entry images use the new entry's BOF,
 * preserve all captured registers and architectural state, and copy only
 * the requested slots, including zero-extended NaTs. */
static void t_native_frame_in(void)
{
    static struct t1_ir ir;
    static struct t1_mir m;
    static struct t1_exec x, want_x;
    static struct vcpu v, want_v;
    m.ir = &ir;
    uint8_t *code = hal_alloc_pages(1, HAL_PAGE_EXEC);
    uint8_t *moved = hal_alloc_pages(1, HAL_PAGE_EXEC);
    CHECK(code != NULL && moved != NULL);
    static const unsigned counts[] = {0, 1, 5, 32, 64, T1_MAX_FRAME_IN};
    unsigned executions = 0;
    for (unsigned test = 0; test < sizeof(counts) / sizeof(counts[0]); ++test) {
        unsigned count = counts[test];
        struct t1_emitted e = {.buf = code, .size = 1, .cap = 4096, .nframe_in = count};
        for (unsigned k = 0; k < count; ++k) {
            unsigned element = (k * 73) % T1_MAX_FRAME_IN;
            e.frame_in[k] = element < 96 ? T1_ELEMENT_GR(32 + element)
                                          : T1_ELEMENT_NAT(32 + element - 96);
        }
        t1_restore_emit(&m, &e);
        if (!count || count == T1_MAX_FRAME_IN) {
            CHECK(!e.frame_in_offset && e.size == 1);
            continue;
        }
        CHECK(e.frame_in_offset == 1 && e.size > 1);
        for (size_t at = 1; at < e.size;) {
            struct t1_x86_decoded d;
            CHECK(t1_x86_decode(code + at, e.size - at, &d));
            CHECK(!t1_x86_required_isa(d.op));
            at += d.length;
        }
        memcpy(moved, code, e.size);
        hal_code_set_executable(code, 1);
        hal_code_set_executable(moved, 1);
        for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof) {
            memset(&x, 0xa5, sizeof(x));
            memset(&v, 0xa5, sizeof(v));
            x.entry_bof = bof;
            v.s.rse_bof = (bof + 17) % IA64_N_STACKED_PHYS;
            for (unsigned k = 0; k < 128; ++k) {
                v.s.gr[k] = rnd64();
                v.s.nat[k] = (rnd64() & 1) != 0;
            }
            want_x = x;
            want_v = v;
            for (unsigned k = 0; k < count; ++k) {
                unsigned el = e.frame_in[k];
                bool nat = el >= T1_ELEMENT_NAT(32);
                unsigned phys = t1t_phys(bof, nat ? el - 128 : el);
                want_x.frame_in[k] = nat ? (uint64_t)v.s.nat[phys] : v.s.gr[phys];
            }
            typedef void (*image_fn)(struct t1_exec *, const struct vcpu *);
            image_fn fill = (image_fn)(uintptr_t)((bof & 1 ? moved : code) + e.frame_in_offset);
            fill(&x, &v);
            CHECK(!memcmp(&x, &want_x, sizeof(x)) && !memcmp(&v, &want_v, sizeof(v)));
            ++executions;
        }
        hal_code_set_writable(code, 1);
        hal_code_set_writable(moved, 1);
    }
    hal_free_pages(code, 1);
    hal_free_pages(moved, 1);
    printf("unit_runtime: native frame-in: %u exact entry images, every BOF, GR/NaT slots, relocation and bounds\n",
           executions);
}

/* Normalization of one home must not change the value used by a later
 * home. Constants interleave with captured values and modular recipes. */
static void t_native_restore_constants(void)
{
    static const struct { uint16_t element; struct t1_loc loc; } frame[] = {
        {T1_ELEMENT_GR(1), {T1_LOC_CONST, 0, 0, UINT64_MAX}},
        {T1_ELEMENT_NAT(32), {T1_LOC_CONST, 0, 0, UINT64_MAX}},
        {T1_ELEMENT_GR(2), {T1_LOC_CONST, 0, 0, UINT64_MAX}},
        {T1_ELEMENT_PR(63), {T1_LOC_CONST, 0, 0, UINT64_MAX}},
        {T1_ELEMENT_GR(3), {T1_LOC_CONST, 0, 0, 1}},
        {T1_ELEMENT_FR_META(2), {T1_LOC_CONST, 0, 0, UINT64_MAX}},
        {T1_ELEMENT_GR(4), {T1_LOC_CONST, 0, 0, UINT64_MAX}},
        {T1_ELEMENT_PSR_MF, {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_GR(5), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_GR(6), {T1_LOC_REG_PLUS, 0, 0, UINT64_MAX}},
        {T1_ELEMENT_GR(7), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_GR(8), {T1_LOC_SPILL_PLUS, 0, 0, UINT64_MAX}},
        {T1_ELEMENT_GR(9), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_FR_META(127), {T1_LOC_REG, 1, 0, 0}},
        {T1_ELEMENT_GR(10), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_PR(1), {T1_LOC_CONST, 0, 0, 2}},
        {T1_ELEMENT_NAT(127), {T1_LOC_CONST, 0, 0, 2}},
        {T1_ELEMENT_GR(11), {T1_LOC_CONST, 0, 0, 2}},
        {T1_ELEMENT_NAT(1), {T1_LOC_REG_PLUS, 0, 0, 3}},
        {T1_ELEMENT_GR(12), {T1_LOC_CONST, 0, 0, 2}},
        {T1_ELEMENT_BR(7), {T1_LOC_HOME, 0, 0, 0}},
        {T1_ELEMENT_FR(2), {T1_LOC_CONST, 0, 0, 0}},
    };
    enum { HOMES = sizeof(frame) / sizeof(frame[0]) };
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    ir->npoints = 1;
    ir->nuses = ir->point[0].frame_count = HOMES;
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->nframe = HOMES;
    m->point[0] = (struct t1_mir_point){0, T1_MIR_NONE, 0, HOMES, T1_PT_EXIT};
    for (unsigned k = 0; k < HOMES; ++k) {
        m->frame[k].element = frame[k].element;
        m->frame_loc[k] = frame[k].loc;
    }
    uint8_t *code = hal_alloc_pages(1, HAL_PAGE_EXEC);
    CHECK(code != NULL);
    struct t1_emit_point ep = {0};
    struct t1_emitted e = {.buf = code, .size = 1, .cap = 4096, .pt = &ep};
    t1_restore_emit(m, &e);
    CHECK(ep.restore_offset);
    hal_code_set_executable(code, 1);
    static struct t1_exec x, saved;
    static struct vcpu v, want;
    static const uint64_t bases[] = {0, 1, UINT64_MAX};
    for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
        for (unsigned b = 0; b < sizeof(bases) / sizeof(bases[0]); ++b) {
            memset(&v, 0xa5, sizeof(v));
            for (unsigned k = 0; k < 128; ++k)
                v.s.nat[k] = (k & 1) != 0;
            x.entry_bof = bof;
            x.regs[0] = bases[b];
            x.regs[1] = rnd64();
            x.spill[0] = ~bases[b];
            saved = x;
            want = v;
            want.s.gr[1] = want.s.gr[2] = UINT64_MAX;
            want.s.nat[t1t_phys(bof, 32)] = true;
            want.s.pr = (want.s.pr | (UINT64_C(1) << 63)) & ~UINT64_C(2);
            want.s.gr[3] = 1;
            want.s.fr[2].exponent = 0x1ffff;
            want.s.gr[4] = UINT64_MAX;
            want.s.fr[2].sign = true;
            want.s.gr[5] = want.s.gr[7] = want.s.gr[9] = want.s.gr[10] = 0;
            want.s.gr[6] = bases[b] - 1;
            want.s.gr[8] = ~bases[b] - 1;
            want.s.fr[127].exponent = x.regs[1] & 0x1ffff;
            want.s.fr[127].sign = (x.regs[1] >> 17) & 1;
            want.s.nat[t1t_phys(bof, 127)] = false;
            want.s.gr[11] = want.s.gr[12] = 2;
            want.s.nat[1] = ((bases[b] + 3) & 1) != 0;
            want.s.fr[2].significand = 0;
            typedef void (*restore_fn)(struct t1_exec *, struct vcpu *);
            ((restore_fn)(uintptr_t)(code + ep.restore_offset))(&x, &v);
            CHECK(!memcmp(&v, &want, sizeof(v)) && !memcmp(&x, &saved, sizeof(x)));
        }
    hal_free_pages(code, 1);
    t1_mir_destroy(m);
    free(ir);
    printf(
        "unit_runtime: native restore constants: 288 exact mixed-home states, modular recipes and constant normalization\n");
}

/* Eight 31-home exits, each clearing nine NaTs, must fit the bounded
 * restore area. Every leaf writes exactly its named architectural homes. */
static void t_native_restore_density(void)
{
    static const struct { uint16_t element; struct t1_loc loc; } frame[] = {
        {T1_ELEMENT_GR(14), {T1_LOC_REG, 3, 0, 0}},
        {T1_ELEMENT_GR(15), {T1_LOC_REG, 5, 0, 0}},
        {T1_ELEMENT_GR(16), {T1_LOC_REG, 10, 0, 0}},
        {T1_ELEMENT_GR(17), {T1_LOC_REG, 6, 0, 0}},
        {T1_ELEMENT_GR(18), {T1_LOC_SPILL, 0, 2, 0}},
        {T1_ELEMENT_GR(19), {T1_LOC_REG, 7, 0, 0}},
        {T1_ELEMENT_GR(20), {T1_LOC_REG, 8, 0, 0}},
        {T1_ELEMENT_GR(22), {T1_LOC_SPILL, 0, 4, 0}},
        {T1_ELEMENT_GR(23), {T1_LOC_SPILL, 0, 5, 0}},
        {T1_ELEMENT_GR(25), {T1_LOC_SPILL, 0, 6, 0}},
        {T1_ELEMENT_GR(34), {T1_LOC_SPILL, 0, 7, 0}},
        {T1_ELEMENT_NAT(14), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_NAT(15), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_NAT(17), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_NAT(18), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_NAT(19), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_NAT(20), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_NAT(22), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_NAT(23), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_NAT(34), {T1_LOC_CONST, 0, 0, 0}},
        {T1_ELEMENT_PR(6), {T1_LOC_SPILL, 0, 46, 0}},
        {T1_ELEMENT_PR(7), {T1_LOC_SPILL, 0, 45, 0}},
        {T1_ELEMENT_PR(8), {T1_LOC_SPILL, 0, 8, 0}},
        {T1_ELEMENT_PR(9), {T1_LOC_SPILL, 0, 9, 0}},
        {T1_ELEMENT_LC, {T1_LOC_SPILL, 0, 10, 0}},
        {T1_ELEMENT_LSB, {T1_LOC_CONST, 0, 0, 0x111e20}},
        {T1_ELEMENT_FR(7), {T1_LOC_SPILL, 0, 11, 0}},
        {T1_ELEMENT_FR(8), {T1_LOC_SPILL, 0, 12, 0}},
        {T1_ELEMENT_PSR_MF, {T1_LOC_REG, 15, 0, 0}},
        {T1_ELEMENT_FR_META(7), {T1_LOC_SPILL, 0, 13, 0}},
        {T1_ELEMENT_FR_META(8), {T1_LOC_CONST, 0, 0, 65598}},
    };
    enum { POINTS = 8, HOMES = sizeof(frame) / sizeof(frame[0]) };
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    ir->npoints = POINTS;
    ir->nuses = POINTS * HOMES;
    for (unsigned p = 0; p < POINTS; ++p)
        ir->point[p].frame_count = HOMES;
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->nframe = POINTS * HOMES;
    for (unsigned p = 0; p < POINTS; ++p) {
        m->point[p] = (struct t1_mir_point){p, T1_MIR_NONE, p * HOMES, HOMES, T1_PT_EXIT};
        for (unsigned k = 0; k < HOMES; ++k) {
            m->frame[p * HOMES + k].element = frame[k].element;
            m->frame_loc[p * HOMES + k] = frame[k].loc;
            if (frame[k].element == T1_ELEMENT_LSB)
                m->frame_loc[p * HOMES + k].value += p * 16;
        }
    }
    uint8_t *code = hal_alloc_pages(2, HAL_PAGE_EXEC);
    uint8_t *moved = hal_alloc_pages(2, HAL_PAGE_EXEC);
    CHECK(code != NULL && moved != NULL);
    struct t1_emit_point ep[POINTS] = {0};
    struct t1_emitted e = {.buf = code,
                           .size = 1,
                           .cap = 8192,
                           .pt = ep,
                           .nframe_in = 2,
                           .frame_in = {T1_ELEMENT_GR(33), T1_ELEMENT_GR(34)}};
    t1_restore_emit(m, &e);
    CHECK(e.frame_in_offset && e.size <= 4097);
    for (unsigned p = 0; p < POINTS; ++p)
        CHECK(ep[p].restore_offset != 0);
    for (size_t at = 1; at < e.size;) {
        struct t1_x86_decoded d;
        CHECK(t1_x86_decode(code + at, e.size - at, &d));
        CHECK(!t1_x86_required_isa(d.op));
        at += d.length;
    }
    memcpy(moved, code, e.size);
    hal_code_set_executable(code, 2);
    hal_code_set_executable(moved, 2);
    static struct t1_exec x, saved;
    static struct vcpu v, want;
    for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
        for (unsigned p = 0; p < POINTS; ++p) {
            memset(&v, 0xa5, sizeof(v));
            for (unsigned k = 0; k < 128; ++k)
                v.s.nat[k] = (k & 1) != 0;
            x.entry_bof = bof;
            v.s.rse_bof = (bof + 17) % IA64_N_STACKED_PHYS;
            for (unsigned k = 0; k < 16; ++k)
                x.regs[k] = rnd64();
            for (unsigned k = 0; k < T1_SPILL_SLOTS; ++k)
                x.spill[k] = rnd64();
            saved = x;
            want = v;
            static const unsigned gr[] = {14, 15, 16, 17, 19, 20};
            static const unsigned reg[] = {3, 5, 10, 6, 7, 8};
            for (unsigned k = 0; k < sizeof(gr) / sizeof(gr[0]); ++k)
                want.s.gr[gr[k]] = x.regs[reg[k]];
            want.s.gr[18] = x.spill[2];
            want.s.gr[22] = x.spill[4];
            want.s.gr[23] = x.spill[5];
            want.s.gr[25] = x.spill[6];
            want.s.gr[t1t_phys(bof, 34)] = x.spill[7];
            static const unsigned nat[] = {14, 15, 17, 18, 19, 20, 22, 23, 34};
            for (unsigned k = 0; k < sizeof(nat) / sizeof(nat[0]); ++k)
                want.s.nat[t1t_phys(bof, nat[k])] = false;
            static const unsigned pr_slot[] = {46, 45, 8, 9};
            for (unsigned k = 0; k < 4; ++k) {
                uint64_t mask = UINT64_C(1) << (6 + k);
                want.s.pr = (want.s.pr & ~mask) | ((x.spill[pr_slot[k]] & 1) ? mask : 0);
            }
            want.s.ar[AR_LC] = x.spill[10];
            want.last_successful_bundle = 0x111e20 + p * 16;
            want.s.fr[7].significand = x.spill[11];
            want.s.fr[8].significand = x.spill[12];
            want.s.psr |= x.regs[15] & T1_PSR_MF_BITS;
            want.s.fr[7].exponent = x.spill[13] & 0x1ffff;
            want.s.fr[7].sign = (x.spill[13] >> 17) & 1;
            want.s.fr[8].exponent = 65598;
            want.s.fr[8].sign = false;
            typedef void (*restore_fn)(struct t1_exec *, struct vcpu *);
            restore_fn restore =
                (restore_fn)(uintptr_t)((bof & 1 ? moved : code) + ep[p].restore_offset);
            restore(&x, &v);
            CHECK(!memcmp(&v, &want, sizeof(v)) && !memcmp(&x, &saved, sizeof(x)));
        }
    hal_free_pages(code, 2);
    hal_free_pages(moved, 2);
    t1_mir_destroy(m);
    free(ir);
    printf(
        "unit_runtime: native restore density: eight 31-home exits in %zu bytes, all BOFs, relocated code, exact state\n",
        e.size - 1);
}

/* A byte-cap failure may discard only its own uncommitted tail. Earlier
 * restores retain their offsets, and a later smaller restore can reuse
 * the tail. Each resulting point still obeys the architectural home rules. */
static void t_native_restore_tail(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    ir->npoints = 3;
    ir->nuses = 34;
    for (unsigned p = 0; p < 3; ++p) {
        ir->point[p].kind = T1_PT_EXIT;
        ir->point[p].frame_count = p == 1 ? 32 : 1;
        ir->point[p].ip = UM_CODE + 16 * p;
        ir->point[p].slot = p;
        ir->point[p].retired_offset = p + 1;
    }
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->nframe = 34;
    for (unsigned p = 0; p < 3; ++p)
        m->point[p] = (struct t1_mir_point){p, T1_MIR_NONE,
                                            p == 0   ? 0
                                            : p == 1 ? 1
                                                     : 33,
                                            p == 1 ? 32 : 1, T1_PT_EXIT};
    const uint64_t first_value = UINT64_C(0x123456789abcdef0);
    const uint64_t last_value = UINT64_C(0xfedcba9876543210);
    m->frame[0].element = T1_ELEMENT_GR(1);
    m->frame_loc[0] = (struct t1_loc){.kind = T1_LOC_CONST, .value = first_value};
    for (unsigned k = 0; k < 32; ++k) {
        m->frame[k + 1].element = T1_ELEMENT_FR_META(k + 2);
        m->frame_loc[k + 1] = (struct t1_loc){.kind = T1_LOC_CONST,
                                              .value = UINT64_C(0xabcdef0000000000) |
                                                       ((uint64_t)(k & 1) << 17) | (0x10000u + k)};
    }
    m->frame[33].element = T1_ELEMENT_GR(31);
    m->frame_loc[33] = (struct t1_loc){.kind = T1_LOC_CONST, .value = last_value};
    uint8_t *code = hal_alloc_pages(1, HAL_PAGE_EXEC);
    CHECK(code != NULL);
    code[0] = 0xcc;
    struct t1_emit_point ep[3] = {0};
    struct t1_emitted e = {.buf = code, .size = 1, .cap = 4096, .pt = ep};
    t1_restore_emit(m, &e);
    /* The middle point is eligible and fits the normal allocation. */
    CHECK(ep[0].restore_offset == 1 && ep[1].restore_offset > 1 && ep[2].restore_offset > 256);
    uint32_t first_end = ep[1].restore_offset;
    uint8_t first_bytes[256];
    CHECK(first_end < sizeof(first_bytes));
    memcpy(first_bytes, code, first_end);
    memset(ep, 0, sizeof(ep));
    e.size = 1;
    e.cap = sizeof(first_bytes);
    t1_restore_emit(m, &e);
    CHECK(ep[0].restore_offset == 1 && !ep[1].restore_offset && ep[2].restore_offset == first_end);
    CHECK(e.size > ep[2].restore_offset && e.size <= e.cap);
    CHECK(!memcmp(code, first_bytes, first_end));
    struct t1_region r = {.code = code, .code_size = e.size};
    CHECK(t1_frame_build(&r, m, &e));
    static struct t1_exec x;
    static struct vcpu v, want;
    memset(&v, 0xa5, sizeof(v));
    for (unsigned k = 0; k < 128; ++k)
        v.s.nat[k] = (k & 1) != 0;
    want = v;
    x.v = &v;
    x.region = &r;
    x.entry_bof = 95;
    x.budget_entry = 100;
    x.raw_r13 = 31;
    x.retired_entry = 123;
    hal_code_set_executable(code, 1);
    for (unsigned p = 0; p < 3; ++p) {
        if (p == 0)
            want.s.gr[1] = first_value;
        else if (p == 1)
            for (unsigned k = 0; k < 32; ++k) {
                want.s.fr[k + 2].exponent = 0x10000u + k;
                want.s.fr[k + 2].sign = (k & 1) != 0;
            }
        else
            want.s.gr[31] = last_value;
        want.s.ip = UM_CODE + 16 * p;
        want.s.slot = p;
        want.retired = 123 + 100 - 31 + p + 1;
        t1_materialize(&x, &r.point[p]);
        CHECK(!memcmp(&v, &want, sizeof(v)));
    }
    t1_memory_free(r.meta, r.meta_pages);
    hal_free_pages(code, 1);
    t1_mir_destroy(m);
    free(ir);
    printf("unit_runtime: native restore tail: earlier and later leaves exact after a byte-cap failure\n");
}

/* SDM Vol.1 register files and Vol.2 register-stack mapping: generated
 * restores must leave every other home and every FR padding byte intact.
 * Exercise every location kind, every BOF, and both predicate endpoints. */
static void t_native_restore(void)
{
    t_native_frame_in();
    t_native_restore_constants();
    t_native_restore_density();
    t_native_restore_tail();
    static const uint16_t element[] = {
        T1_ELEMENT_PR(63), T1_ELEMENT_FR_META(127), T1_ELEMENT_GR(127),
        T1_ELEMENT_NAT(32), T1_ELEMENT_LC, T1_ELEMENT_FR(2), T1_ELEMENT_GR(1),
        T1_ELEMENT_PR(1), T1_ELEMENT_NAT(1), T1_ELEMENT_PSR_MF, T1_ELEMENT_PFS,
        T1_ELEMENT_EC, T1_ELEMENT_LSB, T1_ELEMENT_BR(0), T1_ELEMENT_BR(7),
        T1_ELEMENT_GR(32), T1_ELEMENT_NAT(127), T1_ELEMENT_FR_META(2),
        T1_ELEMENT_FR(127), T1_ELEMENT_GR(31), T1_ELEMENT_NAT(31), T1_ELEMENT_PR(32)
    };
    unsigned count = sizeof(element) / sizeof(element[0]);
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    ir->npoints = 1;
    ir->nuses = ir->point[0].frame_count = 33;
    ir->point[0].kind = T1_PT_EXIT;
    ir->point[0].ip = UM_CODE + 32;
    ir->point[0].slot = 2;
    ir->point[0].retired_offset = 7;
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    ir->nuses = ir->point[0].frame_count = count;
    m->nframe = count;
    m->point[0] = (struct t1_mir_point){0, T1_MIR_NONE, 0, count, T1_PT_EXIT};
    static struct vcpu v, want;
    static struct t1_exec x;
    uint8_t *code = hal_alloc_pages(1, HAL_PAGE_EXEC);
    uint8_t *moved = hal_alloc_pages(1, HAL_PAGE_EXEC);
    CHECK(code != NULL && moved != NULL);
    unsigned executions = 0;
    for (unsigned shift = 0; shift < 8; ++shift) {
        for (unsigned k = 0; k < count; ++k) {
            m->frame[k].element = element[k];
            m->frame_loc[k] = (struct t1_loc){.kind = shift < 6 ? (k + shift) % 6 : T1_LOC_CONST,
                                              .reg = k % 16,
                                              .slot = k % T1_SPILL_SLOTS,
                                              .value = shift == 6   ? UINT64_MAX
                                                       : shift == 7 ? 0
                                                       : k & 1      ? UINT64_MAX
                                                                    : UINT64_C(0x80000000ffffffff)};
        }
        struct t1_emit_point ep = {0};
        struct t1_emitted e = {.buf = code, .size = 1, .cap = 4096, .pt = &ep};
        t1_restore_emit(m, &e);
        CHECK(ep.restore_offset == 1 && e.size > 1 && e.size <= 4096);
        for (size_t at = 1; at < e.size;) {
            struct t1_x86_decoded d;
            CHECK(t1_x86_decode(code + at, e.size - at, &d));
            CHECK(!t1_x86_required_isa(d.op));
            at += d.length;
        }
        struct t1_region r = {.code = code, .frames = true};
        CHECK(t1_frame_build(&r, m, &e));
        CHECK(r.point[0].restore_offset == ep.restore_offset);
        CHECK(!r.fast_points); /* A nonzero retired offset needs normal accounting. */
        ir->point[0].retired_offset = 0;
        struct t1_region fast = {0};
        CHECK(t1_frame_build(&fast, m, &e));
        CHECK(fast.fast_points == 1u); /* Native edge rechecks PSR after MF restoration. */
        t1_memory_free(fast.meta, fast.meta_pages);
        ir->point[0].retired_offset = 7;
        r.point[0].delta = shift % 3;
        r.point[0].rse = shift != 0;
        r.point[0].cfm = 0x123;
        x.v = &v;
        x.region = &r;
        x.retired_entry = 123;
        x.budget_entry = 100;
        x.raw_r13 = 31;
        x.entry_cfm = 0x456;
        x.entry_bsp = 0x1f8;
        x.entry_dirty = 7;
        x.bsp_called = shift & 1;
        memcpy(moved, code, e.size);
        hal_code_set_executable(code, 1);
        hal_code_set_executable(moved, 1);
        for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof) {
            r.code = bof & 1 ? moved : code;
            memset(&v, 0xa5, sizeof(v));
            for (unsigned k = 0; k < 128; ++k)
                v.s.nat[k] = (k & 1) != 0;
            x.entry_bof = bof;
            /* Restores use the entry frame even when the current one differs. */
            v.s.rse_bof = (bof + 17) % IA64_N_STACKED_PHYS;
            v.s.psr = rnd64();
            v.s.pr = rnd64();
            want = v;
            for (unsigned k = 0; k < 16; ++k)
                x.regs[k] = rnd64();
            for (unsigned k = 0; k < T1_SPILL_SLOTS; ++k)
                x.spill[k] = rnd64();
            for (unsigned k = 0; k < count; ++k) {
                const struct t1_loc *l = &m->frame_loc[k];
                unsigned el = element[k];
                if (l->kind == T1_LOC_HOME)
                    continue;
                uint64_t value = l->value;
                if (l->kind == T1_LOC_REG || l->kind == T1_LOC_REG_PLUS)
                    value = x.regs[l->reg];
                if (l->kind == T1_LOC_SPILL || l->kind == T1_LOC_SPILL_PLUS)
                    value = x.spill[l->slot];
                if (l->kind == T1_LOC_REG_PLUS || l->kind == T1_LOC_SPILL_PLUS)
                    value += l->value;
                if (el < 128)
                    want.s.gr[t1t_phys(bof, el)] = value;
                else if (el < 256)
                    want.s.nat[t1t_phys(bof, el - 128)] = (value & 1) != 0;
                else if (el < T1_ELEMENT_LC) {
                    uint64_t bit = UINT64_C(1) << (el - 256);
                    want.s.pr = (want.s.pr & ~bit) | ((value & 1) ? bit : 0);
                } else if (el == T1_ELEMENT_LC)
                    want.s.ar[AR_LC] = value;
                else if (el == T1_ELEMENT_LSB)
                    want.last_successful_bundle = value;
                else if (el <= T1_ELEMENT_FR(127))
                    want.s.fr[el - 323].significand = value;
                else if (el == T1_ELEMENT_PSR_MF)
                    want.s.psr |= value & T1_PSR_MF_BITS;
                else if (el <= T1_ELEMENT_FR_META(127)) {
                    want.s.fr[el - 452].exponent = value & 0x1ffff;
                    want.s.fr[el - 452].sign = (value >> 17) & 1;
                } else if (el == T1_ELEMENT_PFS)
                    want.s.ar[AR_PFS] = value;
                else if (el == T1_ELEMENT_EC)
                    want.s.ar[AR_EC] = value;
                else
                    want.s.br[el - T1_ELEMENT_BR(0)] = value;
            }
            want.s.ip = UM_CODE + 32;
            want.s.slot = 2;
            want.retired = 123 + 100 - 31 + 7;
            want.s.cfm = shift ? 0x123 : 0x456;
            want.s.rse_bof = (bof + shift % 3) % IA64_N_STACKED_PHYS;
            want.s.rse_dirty = 7 + shift % 3;
            want.s.ar[AR_BSP] = shift % 3 || (shift & 1) ? 0x200 + 8 * (shift % 3) : 0x1f8;
            t1_materialize(&x, &r.point[0]);
            CHECK(!memcmp(&v, &want, sizeof(v)));
            ++executions;
        }
        hal_code_set_writable(code, 1);
        hal_code_set_writable(moved, 1);
        t1_memory_free(r.meta, r.meta_pages);
        /* Capacity exhaustion leaves the original emitted code usable. */
        ep.restore_offset = 0;
        e.size = 1;
        e.cap = 2;
        t1_restore_emit(m, &e);
        CHECK(e.size == 1 && ep.restore_offset == 0);
        e.size = T1_CHUNK_BYTES + 1;
        e.cap = T1_CHUNK_BYTES + 4096;
        t1_restore_emit(m, &e);
        CHECK(e.size == T1_CHUNK_BYTES + 1 && ep.restore_offset == 0);
    }
    /* A larger valid frame keeps exact table restoration. */
    ir->nuses = ir->point[0].frame_count = m->nframe = m->point[0].frame_count = 33;
    for (unsigned k = 0; k < 33; ++k) {
        m->frame[k].element = T1_ELEMENT_GR(k + 1);
        m->frame_loc[k] = (struct t1_loc){.kind = T1_LOC_CONST, .value = 0x12340000u + k};
    }
    struct t1_emit_point ep = {0};
    struct t1_emitted e = {.buf = code, .size = 1, .cap = 4096, .pt = &ep};
    t1_restore_emit(m, &e);
    CHECK(e.size == 1 && ep.restore_offset == 0);
    struct t1_region r = {.code = code};
    CHECK(t1_frame_build(&r, m, &e));
    x.region = &r;
    want = v;
    for (unsigned k = 0; k < 33; ++k)
        want.s.gr[t1t_phys(x.entry_bof, k + 1)] = 0x12340000u + k;
    t1_materialize(&x, &r.point[0]);
    CHECK(!memcmp(&v, &want, sizeof(v)));
    t1_memory_free(r.meta, r.meta_pages);
    hal_free_pages(code, 1);
    hal_free_pages(moved, 1);
    t1_mir_destroy(m);
    free(ir);
    printf("unit_runtime: native restore: %u exact states, every location and BOF, baseline ISA, bounded fallback\n",
           executions);
}
