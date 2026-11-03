/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/tier1/memory.h"

/* The frame plan is stable within each restore class. HOME entries
 * follow every restored entry, including predicates and both FR halves. */
static void t_frame_plan(void)
{
    static const uint16_t element[] = {
        T1_ELEMENT_PR(63), T1_ELEMENT_GR(127), T1_ELEMENT_LC, T1_ELEMENT_NAT(31),
        T1_ELEMENT_GR(1), T1_ELEMENT_LSB, T1_ELEMENT_NAT(127), T1_ELEMENT_PR(1),
        T1_ELEMENT_GR(32), T1_ELEMENT_NAT(1), T1_ELEMENT_GR(31), T1_ELEMENT_NAT(32),
        T1_ELEMENT_FR(2), T1_ELEMENT_FR_META(2), T1_ELEMENT_PR(2), T1_ELEMENT_GR(2)
    };
    static const unsigned order[] = {4, 10, 1, 8, 3, 9, 6, 11, 0, 7, 2, 5, 12, 13, 14, 15};
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    ir->npoints = 1;
    ir->nuses = sizeof(element) / sizeof(element[0]);
    ir->point[0].kind = T1_PT_CHECKPOINT;
    ir->point[0].frame_count = ir->nuses;
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->nframe = ir->nuses;
    m->point[0] = (struct t1_mir_point){0, T1_MIR_NONE, 0, ir->nuses, T1_PT_CHECKPOINT};
    for (unsigned k = 0; k < ir->nuses; ++k) {
        m->frame[k].element = element[k];
        m->frame_loc[k] = (struct t1_loc){.kind = k < 14 ? T1_LOC_CONST : T1_LOC_HOME, .value = 0x1000 + k};
    }
    m->frame_loc[9] = (struct t1_loc){.kind = T1_LOC_REG, .reg = T1_RAX};
    m->frame_loc[11] = (struct t1_loc){.kind = T1_LOC_SPILL_PLUS, .slot = 3, .value = UINT64_MAX};
    m->frame_loc[2] = (struct t1_loc){.kind = T1_LOC_REG_PLUS, .reg = T1_RCX, .value = 0xff7};
    m->frame_loc[5] = (struct t1_loc){.kind = T1_LOC_SPILL, .slot = 4};
    struct t1_emit_point ep = {0};
    struct t1_emitted e = {.pt = &ep};
    struct t1_region r = {0};
    CHECK(t1_frame_build(&r, m, &e));
    CHECK(r.nframe == ir->nuses && r.npoints == 1 && r.point[0].frame_count == ir->nuses);
    CHECK(r.point[0].frame_restore == 14 && r.point[0].frame_first == 0);
    CHECK(r.point[0].pr_mask == ((UINT64_C(1) << 63) | (UINT64_C(1) << 1)));
    for (unsigned k = 0; k < 5; ++k) CHECK(r.point[0].plan[k] == 2 * (k + 1));
    CHECK(r.point[0].plan[5] == 11 && r.point[0].plan[6] == 12);
    for (unsigned k = 0; k < r.nframe; ++k) {
        CHECK(r.frame[k].element == element[order[k]]);
        CHECK(r.frame[k].kind == m->frame_loc[order[k]].kind);
        CHECK(r.frame[k].value == m->frame_loc[order[k]].value);
    }
    static struct vcpu v, want;
    static struct t1_exec x;
    x.v = &v;
    x.region = &r;
    x.regs[T1_RAX] = UINT64_MAX;
    x.spill[3] = 2;
    x.regs[T1_RCX] = 11;
    x.spill[4] = 0x1005;
    for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof) {
        memset(&v, 0xa5, sizeof(v));
        for (unsigned k = 0; k < 128; ++k) v.s.nat[k] = (k & 1) != 0;
        x.entry_bof = bof;
        want = v;
        want.s.gr[1] = 0x1004;
        want.s.gr[31] = 0x100a;
        want.s.gr[t1t_phys(bof, 127)] = 0x1001;
        want.s.gr[t1t_phys(bof, 32)] = 0x1008;
        want.s.nat[31] = true;
        want.s.nat[1] = true;
        want.s.nat[t1t_phys(bof, 127)] = false;
        want.s.nat[t1t_phys(bof, 32)] = true;
        want.s.pr = (want.s.pr & ~(UINT64_C(1) << 63)) | (UINT64_C(1) << 1);
        want.s.ar[AR_LC] = 0x1002;
        want.last_successful_bundle = 0x1005;
        want.s.fr[2].significand = 0x100c;
        want.s.fr[2].exponent = 0x100d;
        want.s.fr[2].sign = 0;
        want.s.ip = 0;
        want.s.slot = 0;
        want.retired = 0;
        t1_materialize(&x, &r.point[0]);
        CHECK(!memcmp(&v, &want, sizeof(v)));
    }
    t1_memory_free(r.meta, r.meta_pages);
    t1_mir_destroy(m);
    free(ir);
    printf("unit_runtime: frame plan: all restore classes, stable order, HOME suffix and predicate mask at every BOF\n");
}

/* Immutable frames retain every location field, including unused fields.
 * Page counts include the point tables and their allocation rounding. */
static void t_frame_compact(void)
{
    static const struct { unsigned points, homes, pages; } shape[] = {
        {8, 64, 3}, {16, 64, 5}, {32, 127, 17}
    };
    static const uint64_t values[] = {0, 1, UINT64_MAX, UINT64_MAX - 1,
        UINT64_C(1) << 32, UINT64_C(1) << 63, (UINT64_C(1) << 63) - 1,
        UINT64_C(0x123456789abcdef0)};
    static const uint16_t slots[] = {0, 255, 256, UINT16_MAX};
    static const uint8_t regs[] = {0, 15, 16, UINT8_MAX};
    static struct t1_exec x;
    static struct vcpu v, want;
    for (unsigned test = 0; test < IA64VMM_ARRAY_SIZE(shape); ++test) {
        unsigned points = shape[test].points, homes = shape[test].homes;
        struct t1_ir *ir = calloc(1, sizeof(*ir));
        CHECK(ir != NULL);
        ir->npoints = points;
        ir->nuses = points * homes;
        for (unsigned p = 0; p < points; ++p) {
            ir->point[p].kind = T1_PT_CHECKPOINT;
            ir->point[p].frame_count = homes;
            ir->point[p].ip = UM_CODE + 16 * p;
        }
        struct t1_mir *m = t1_mir_create(ir);
        CHECK(m != NULL);
        m->nframe = ir->nuses;
        for (unsigned p = 0; p < points; ++p) {
            m->point[p] = (struct t1_mir_point){p, T1_MIR_NONE, p * homes, homes, T1_PT_CHECKPOINT};
            for (unsigned k = 0; k < homes; ++k) {
                unsigned at = p * homes + k;
                m->frame[at].element = k + 1;
                m->frame_loc[at] = (struct t1_loc){at % 6, regs[(at / 6) % 4], slots[(at / 24) % 4],
                                                   values[(at / 96) % 8]};
                if (!k || k + 1 == homes) {
                    m->frame[at].element = k ? UINT16_MAX : 0;
                    m->frame_loc[at].kind = T1_LOC_HOME;
                }
            }
        }
        struct t1_emit_point ep[T1_MAX_POINTS] = {{0}};
        struct t1_emitted out = {.pt = ep};
        struct t1_region r = {0};
        struct t1_memory_stats before, after;
        t1_memory_get_stats(&before);
        CHECK(t1_frame_build(&r, m, &out));
        t1_memory_get_stats(&after);
        CHECK(r.nframe == points * homes && r.npoints == points);
        for (unsigned p = 0; p < points; ++p) {
            const struct t1_point *point = &r.point[p];
            CHECK(point->frame_first == p * homes && point->frame_count == homes);
            for (unsigned j = 0; j < homes; ++j) {
                const struct t1_frame_entry *f = &r.frame[point->frame_first + j];
                unsigned k = !f->element ? 0 : f->element == UINT16_MAX ? homes - 1 : f->element - 1u;
                CHECK(k < homes);
                const struct t1_loc *a = &m->frame_loc[p * homes + k];
                struct t1_loc b = t1_frame_loc(f);
                CHECK(f->element == m->frame[p * homes + k].element);
                CHECK(a->kind == b.kind && a->reg == b.reg && a->slot == b.slot && a->value == b.value);
                CHECK((j >= point->frame_restore) == (b.kind == T1_LOC_HOME));
            }
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof) {
                memset(&v, 0, sizeof(v));
                memset(&x, 0, sizeof(x));
                for (unsigned k = 0; k < 16; ++k) x.regs[k] = UINT64_MAX - k;
                for (unsigned k = 0; k < T1_SPILL_SLOTS; ++k) x.spill[k] = UINT64_C(0x80000000ffffffff) + k;
                x.region = &r;
                x.v = &v;
                x.entry_bof = bof;
                want = v;
                for (unsigned k = 0; k < homes; ++k) {
                    const struct t1_loc *l = &m->frame_loc[p * homes + k];
                    if (l->kind == T1_LOC_HOME)
                        continue;
                    uint64_t value = l->value;
                    if (l->kind == T1_LOC_REG || l->kind == T1_LOC_REG_PLUS)
                        value = x.regs[l->reg & 15];
                    if (l->kind == T1_LOC_SPILL || l->kind == T1_LOC_SPILL_PLUS)
                        value = x.spill[l->slot % T1_SPILL_SLOTS];
                    if (l->kind == T1_LOC_REG_PLUS || l->kind == T1_LOC_SPILL_PLUS)
                        value += l->value;
                    want.s.gr[t1t_phys(bof, k + 1)] = value;
                }
                want.s.ip = point->ip;
                t1_materialize(&x, point);
                CHECK(!memcmp(&v, &want, sizeof(v)));
            }
        }
        CHECK(r.meta_pages == shape[test].pages);
        CHECK(t1_frame_pages(m, &out) == r.meta_pages);
        CHECK(after.used_bytes - before.used_bytes == r.meta_pages * 4096);
        t1_memory_free(r.meta, r.meta_pages);
        t1_mir_destroy(m);
        free(ir);
    }
    CHECK(sizeof(struct t1_frame_entry) == 16 && _Alignof(struct t1_frame_entry) == 8);
    CHECK(sizeof(struct t1_loc) == 16);
    puts("unit_runtime: compact frames: exact fields/all locations/all BOFs; 512/1024/4064 entries in 3/5/17 pages");
}
