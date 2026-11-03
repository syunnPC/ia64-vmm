/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/pmu.h"

/* Intel 251110-003 §10.3, Table 11-67; 308065-001 §3.3, Table 4-77;
 * SDM Rev.2.3 Vol.2 §7.2. Counters below use zero threshold;
 * Montecito uses ISM=2 as required by Table 3-5. */
static void test_pmu_retirement(void)
{
    const uint64_t nop = UINT64_C(1) << 27, up = UINT64_C(1) << 2;
    const uint64_t count_mask = UINT64_C(0x7fffffffffff);
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        unsigned opc = model ? 8 : 32, ranges = model ? 14 : 38;
        CHECK(v->s.pmc[opc] == UINT64_MAX && v->s.pmc[opc + 1] == UINT64_MAX);
        CHECK(v->s.pmc[ranges] == 0xdb6);
        CHECK(v->s.pmc[model ? 15 : 36] == UINT64_C(0xfffffff0));
        CHECK(v->s.pmc[model ? 13 : 41] == UINT64_C(0x2078fefefefe));
        if (model) CHECK(v->s.pmc[4] & (UINT64_C(1) << 23));
        uint64_t control = model ? UINT64_C(1) << 23 : UINT64_C(2) << 24;
        v->s.pmc[4] = control | 0x801; v->s.psr = up;
        bundle_write(ENTRY, 0x11, nop | 1, nop, (UINT64_C(4) << 37) | 1);
        /* Predicated-off nonbranch, true nop, predicated-off branch. */
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        CHECK(v->retired == 3 && v->s.pmd[4] == (model ? 2u : 3u));
        bundle_write(ENTRY, 4, nop, 0, UINT64_C(6) << 37 | (8u << 6));
        v->s.ip = ENTRY; v->s.slot = 0;
        CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET);
        CHECK(v->retired == 5 && v->s.pmd[4] == (model ? 4u : 5u));
        bundle_write(ENTRY, 0, nop, nop, nop);
        /* All enabled counters observe the same retiring instruction. */
        unsigned last = model ? 7 : 15;
        v->s.pmc[last] = (model ? 0 : UINT64_C(2) << 24) | 0x801;
        v->s.pmd[4] = v->s.pmd[last] = count_mask;
        v->s.ip = ENTRY; v->s.slot = 0;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK((v->s.pmc[0] & ((UINT64_C(1) << 4) | (UINT64_C(1) << last))) ==
              ((UINT64_C(1) << 4) | (UINT64_C(1) << last)));
        CHECK(!(v->s.pmc[0] & 1) && !(v->s.pmd[4] & count_mask));
        if (model) {
            CHECK(v->s.pmd[4] & (UINT64_C(1) << 47));
            v->s.pmd[4] = UINT64_C(0xffffffffffff);
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == 0);
        }
        v->s.pmc[4] |= 0x20; v->s.pmd[4] = count_mask;
        v->s.cr[CR_PMV] = 0x96;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK((v->s.pmc[0] & 0x11) == 0x11 && ia64_ivr_read(v) == 0x96);
        ia64_eoi(v);
        uint64_t frozen = v->s.pmd[last];
        v->s.ip = ENTRY; v->s.slot = 0;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.pmd[last] == frozen);
        /* A masked PMV drops the edge but retains the freeze and status. */
        v->s.pmc[0] = 0; v->s.pmd[4] = count_mask;
        v->s.cr[CR_PMV] = (1u << 16) | 0x96;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK((v->s.pmc[0] & 0x11) == 0x11 && ia64_ivr_read(v) == 15);
        v->s.cr[CR_PMV] &= ~(UINT64_C(1) << 16);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && ia64_ivr_read(v) == 15);
        vcpu_destroy(v);
    }
}

static void test_pmu_controls(void)
{
    const uint64_t nop = UINT64_C(1) << 27;
    bundle_write(ENTRY, 0, nop, nop, nop);
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        uint64_t control = model ? UINT64_C(1) << 23 : UINT64_C(2) << 24;
        for (unsigned cpl = 0; cpl < 4; ++cpl)
            for (unsigned pm = 0; pm < 2; ++pm)
                for (unsigned mask = 0; mask < 4; ++mask) {
                    v->s.pmc[4] = control | 0x800 | (UINT64_C(1) << cpl) | (pm << 6);
                    v->s.psr = ((uint64_t)cpl << 32) | ((uint64_t)(mask & 1) << 2) |
                               ((uint64_t)(mask >> 1) << 21);
                    v->s.ip = ENTRY; v->s.slot = 0; v->s.pmd[4] = 0;
                    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
                    CHECK(v->s.pmd[4] == ((mask >> pm) & 1));
                }
        v->s.psr = UINT64_C(1) << 2;
        const uint64_t unsupported[] = {0x1201, 0x100801};
        for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(unsupported); ++k) {
            v->s.pmc[4] = control | unsupported[k];
            v->s.ip = ENTRY; v->s.slot = 0;
            uint64_t before = v->retired;
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_UNIMPLEMENTED && v->retired == before);
            v->s.pmc[0] = 1;
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
            v->s.pmc[0] = 0; v->s.psr = 0;
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
            v->s.psr = UINT64_C(1) << 2;
        }
        v->s.pmc[4] = control | 0x801 | (UINT64_C(1) << 24);
        uint64_t before = v->s.pmd[4];
        v->s.ip = ENTRY; v->s.slot = 0;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == before);
        v->s.pmc[4] = control | 0x801;
        unsigned opc = model ? 8 : 32, ranges = model ? 14 : 38;
        v->s.pmc[opc] &= ~(UINT64_C(1) << (model ? 3 : 0));
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        v->s.pmc[model ? 15 : 36] |= 1;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        v->s.ip = ENTRY; v->s.slot = 0;
        v->s.pmc[opc] &= ~(UINT64_C(1) << (model ? 0 : 57));
        v->s.pmc[ranges] &= ~UINT64_C(2);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_UNIMPLEMENTED);
        v->s.pmc[ranges] |= 2;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        if (model) {
            v->s.pmc[4] &= ~control; before = v->s.pmd[4];
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == before);
        } else {
            v->s.pmc[4] |= UINT64_C(1) << 26;
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_UNIMPLEMENTED);
        }
        vcpu_destroy(v);
    }
}

static void pmu_filter_step(struct vcpu *v, unsigned slot, enum vcpu_exit_reason reason,
                            uint64_t count)
{
    v->s.ip = ENTRY; v->s.slot = slot; v->s.pmd[4] = 0;
    CHECK(engine_run(v, 1).reason == reason);
    CHECK(v->s.pmd[4] == count);
}

static void pmu_filter_tags(struct vcpu *v, uint64_t address, bool supported, unsigned count)
{
    Ia64Instruction i = instruction(IA64_OP_NOP);
    i.raw = UINT64_C(1) << 27; i.address = address; i.unit = IA64_UNIT_M;
    struct ia64_pmu_retirement pending;
    struct ia64_result r = ia64_pmu_prepare(v, &i, &pending);
    CHECK(r.kind == (supported ? IA64_STEP_COMPLETE : IA64_STEP_EXIT));
    if (!supported) CHECK(r.exit == VCPU_EXIT_UNIMPLEMENTED);
    CHECK(pending.count == count);
}

/* Intel 251110-003 pp.186-187; 308065-001 p.162: ES 0x50 excludes
 * predicated-off nops; ES 0x51 includes false-predicate non-B syllables. */
static void test_pmu_nops_and_squashed(void)
{
    const uint64_t nop = UINT64_C(1) << 27, nop_b = UINT64_C(2) << 37;
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        uint64_t control = model ? UINT64_C(1) << 23 : UINT64_C(2) << 24;
        v->s.psr = UINT64_C(1) << 2;
        v->s.pmc[4] = control | 0x5001;
        v->s.pmc[5] = (model ? 0 : control) | 0x5101;
        bundle_write(ENTRY, 0x0c, nop, nop, nop);
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.pmd[4] == 3 && v->s.pmd[5] == 0);
        bundle_write(ENTRY, 0x11, nop, nop, nop_b);
        v->s.ip = ENTRY; v->s.slot = 0;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.pmd[4] == 6 && v->s.pmd[5] == 0);
        bundle_write(ENTRY, 0x0c, nop | 1, nop | 1, nop | 1);
        v->s.ip = ENTRY; v->s.slot = 0;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.pmd[4] == 6 && v->s.pmd[5] == 3);
        /* nop.b has a false qp but is still a B syllable. */
        bundle_write(ENTRY, 0x11, nop | 1, nop | 1, nop_b | 1);
        v->s.ip = ENTRY; v->s.slot = 0;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.pmd[4] == 6 && v->s.pmd[5] == 5);
        /* True hints are not nop instructions; a false add still squashes. */
        uint64_t hint = nop | (UINT64_C(1) << 26);
        uint64_t adds = (UINT64_C(8) << 37) | (UINT64_C(2) << 34) |
                        (UINT64_C(8) << 20) | (UINT64_C(1) << 13) | (UINT64_C(8) << 6) | 1;
        bundle_write(ENTRY, 0, hint, hint, adds);
        v->s.ip = ENTRY; v->s.slot = 0;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.pmd[4] == 6 && v->s.pmd[5] == 6);
        /* ES 0x50 does not specify nop.x; ES 0x51 covers non-B syllables. */
        bundle_write(ENTRY, 4, nop, 0, nop);
        v->s.ip = ENTRY; v->s.slot = 1;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_UNIMPLEMENTED);
        CHECK(v->s.pmd[4] == 6 && v->s.pmd[5] == 6);
        v->s.pmc[4] &= ~UINT64_C(1);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.pmd[4] == 6 && v->s.pmd[5] == 6);
        bundle_write(ENTRY, 4, nop, 0, nop | 1);
        v->s.ip = ENTRY; v->s.slot = 1;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.pmd[4] == 6 && v->s.pmd[5] == 7);
        /* SDM Vol.3 §4.7.3.1: brl.cond is an X3 branch, not a B syllable. */
        v->s.pmc[4] = control | 0x801;
        v->s.pmd[4] = v->s.pmd[5] = 0;
        bundle_write(ENTRY, 4, nop, 0, (UINT64_C(0xc) << 37) | 1);
        v->s.ip = ENTRY; v->s.slot = 1;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.pmd[4] == 1 && v->s.pmd[5] == 1);
        CHECK(v->s.ip == ENTRY + 16 && v->s.slot == 0);
        /* Both events use channel 0 address/opcode constraints. */
        v->s.pmc[4] = control | 0x5001;
        v->s.pmc[5] = 0;
        unsigned opc = model ? 8 : 32, ranges = model ? 14 : 38;
        v->s.pmc[opc] &= ~(UINT64_C(3) << (model ? 0 : 56));
        v->s.pmc[ranges] = 0xdb6 & ~UINT64_C(2);
        v->s.ibr[0] = ENTRY + 16;
        v->s.ibr[1] = UINT64_C(0x01ffffffffffffff);
        bundle_write(ENTRY, 0, nop, nop, nop);
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        v->s.ibr[0] = ENTRY;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
        v->s.pmc[opc] &= ~(UINT64_C(8) << (model ? 60 : 48));
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        v->s.pmc[4] = control | 0x5101;
        bundle_write(ENTRY, 0, nop | 1, nop, nop);
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        v->s.pmc[opc] |= UINT64_C(8) << (model ? 60 : 48);
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
        v->s.ibr[0] = ENTRY + 16;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        v->s.pmc[ranges] |= 2;
        v->s.pmc[4] = control | 0x15001;
        pmu_filter_step(v, 0, VCPU_EXIT_UNIMPLEMENTED, 0);
        /* Ignored encodings execute as no-ops but ES 0x50 is unspecified. */
        v->s.pmc[4] = control | 0x5001;
        bundle_write(ENTRY, 0x11, nop, nop, (UINT64_C(3) << 37) | 1);
        pmu_filter_step(v, 2, VCPU_EXIT_UNIMPLEMENTED, 0);
        v->s.pmc[4] = control | 0x5101;
        pmu_filter_step(v, 2, VCPU_EXIT_BUDGET, 0);
        vcpu_destroy(v);
    }
}

/* Intel 251110-003 Table 10-12 and §10.3.5.2: fine matches signal on
 * both paired channels; the upper pair replaces the lower only in coarse mode.
 * Intel 308065-001 Table 3-10 leaves fine channels 2/3 undefined. */
static void test_pmu_paired_fine_ranges(void)
{
    for (unsigned model = 0; model < 2; ++model)
        for (unsigned channel = 2; channel < 4; ++channel) {
            struct vcpu *v = model_vcpu(model);
            unsigned opc = model ? 8 : 32, ranges = model ? 14 : 38;
            unsigned lower = 2 * (channel & 1), upper = lower + 4;
            uint64_t ignore = UINT64_C(1) << (1 + 3 * channel);
            uint64_t control = (model ? UINT64_C(1) << 23 : UINT64_C(2) << 24) |
                               0x80f | ((uint64_t)channel << 16);
            v->s.pmc[4] = control; v->s.psr = UINT64_C(1) << 2;
            v->s.pmc[opc] &= ~(UINT64_C(3) << (model ? 0 : 56));
            v->s.pmc[ranges] = (0xdb6 | (UINT64_C(1) << 13)) & ~ignore;
            v->s.ibr[lower] = ENTRY + 16; v->s.ibr[upper] = ENTRY + 48;
            v->s.ibr[lower + 1] = UINT64_C(1) << 56;
            v->s.ibr[upper + 1] = UINT64_C(0x00ffffffffffffff);
            pmu_filter_tags(v, ENTRY + 32, model, model ? 1u << 4 : 0);
            if (!model) {
                v->s.pmc[ranges] |= ignore;
                pmu_filter_tags(v, ENTRY + 32, true, 1u << 4);
                vcpu_destroy(v);
                continue;
            }
            uint64_t lower_ignore = UINT64_C(1) << (1 + 3 * (channel & 1));
            v->s.pmc[ranges] &= ~lower_ignore;
            v->s.pmc[5] = 0x80f | ((uint64_t)(channel & 1) << 16);
            pmu_filter_tags(v, ENTRY + 32, true, (1u << 4) | (1u << 5));
            v->s.pmc[5] = 0;
            v->s.pmc[ranges] |= lower_ignore;
            pmu_filter_tags(v, ENTRY + 16, true, 0);
            pmu_filter_tags(v, ENTRY + 48, true, 0);
            /* The lower pair supplies the high-address mask and PLM. */
            pmu_filter_tags(v, ENTRY + 0x1020, true, 1u << 4);
            v->s.ibr[lower + 1] |= UINT64_C(0x00ffffffffffffff);
            pmu_filter_tags(v, ENTRY + 0x1020, true, 0);
            pmu_filter_tags(v, ENTRY + 32, true, 1u << 4);
            for (unsigned bit = 56; bit < 64; ++bit)
                pmu_filter_tags(v, (ENTRY + 32) ^ (UINT64_C(1) << bit), true, 0);
            for (unsigned cpl = 0; cpl < 4; ++cpl) {
                v->s.psr = (UINT64_C(1) << 2) | ((uint64_t)cpl << 32);
                v->s.ibr[lower + 1] = UINT64_C(0x00ffffffffffffff) |
                                      (UINT64_C(1) << (56 + cpl));
                pmu_filter_tags(v, ENTRY + 32, true, 1u << 4);
                v->s.ibr[lower + 1] &= ~(UINT64_C(1) << (56 + cpl));
                v->s.ibr[upper + 1] |= UINT64_C(15) << 56;
                pmu_filter_tags(v, ENTRY + 32, true, 0);
            }
            v->s.psr = UINT64_C(1) << 2;
            v->s.ibr[lower + 1] |= UINT64_C(1) << 56;
            /* Madison does not define these controls per tag channel. */
            v->s.pmc[8] |= 1;
            pmu_filter_tags(v, ENTRY + 32, false, 0);
            v->s.pmc[8] = (v->s.pmc[8] & ~UINT64_C(1)) | 2;
            pmu_filter_tags(v, ENTRY + 32, false, 0);
            v->s.pmc[8] &= ~UINT64_C(3);
            unsigned opcode = 8 + (channel & 1);
            v->s.pmc[opcode] &= ~(UINT64_C(8) << 60);
            pmu_filter_tags(v, ENTRY + 32, true, 0);
            v->s.pmc[15] |= UINT64_C(1) << channel;
            pmu_filter_tags(v, ENTRY + 32, true, 1u << 4);
            v->s.pmc[opcode] |= UINT64_C(8) << 60;
            v->s.ibr[upper] = ENTRY + 0x1030;
            pmu_filter_tags(v, ENTRY + 32, false, 0);
            v->s.ibr[upper] = ENTRY + 16;
            pmu_filter_tags(v, ENTRY + 32, false, 0);
            v->s.ibr[upper] = ENTRY + 48;
            for (unsigned pair = 0; pair < 4; ++pair) {
                v->s.ibr[2 * pair + 1] |= UINT64_C(1) << 63;
                pmu_filter_tags(v, ENTRY + 32, false, 0);
                v->s.ibr[2 * pair + 1] &= ~(UINT64_C(1) << 63);
            }
            v->s.psr |= IA64_PSR_DB;
            pmu_filter_tags(v, ENTRY + 32, false, 0);
            v->s.pmc[ranges] |= ignore;
            pmu_filter_tags(v, ENTRY + 32, true, 1u << 4);
            v->s.psr = (UINT64_C(1) << 2) | IA64_PSR_IS;
            v->s.pmc[4] |= UINT64_C(1) << 24;
            v->s.pmc[ranges] &= ~ignore;
            pmu_filter_tags(v, ENTRY + 32, true, 0);
            v->s.pmc[ranges] |= ignore;
            pmu_filter_tags(v, ENTRY + 32, true, 1u << 4);
            vcpu_destroy(v);
        }
}

/* Intel 251110-003 Table 10-9; 308065-001 Table 3-11: inv inverts
 * channel 0's address range. Its interaction with PLM/fine is not specified. */
static void test_pmu_inverted_ranges(void)
{
    const uint64_t nop = UINT64_C(1) << 27;
    bundle_write(ENTRY, 0, nop, nop, nop);
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        unsigned opc = model ? 8 : 32, ranges = model ? 14 : 38;
        uint64_t ig_ad = UINT64_C(1) << (model ? 0 : 57);
        uint64_t control = (model ? UINT64_C(1) << 23 : UINT64_C(2) << 24) | 0x80f;
        v->s.pmc[4] = control;
        v->s.pmc[opc] = UINT64_MAX & ~ig_ad;
        v->s.pmc[ranges] = 0xdb4;
        for (unsigned cpl = 0; cpl < 4; ++cpl) {
            v->s.psr = (UINT64_C(1) << 2) | ((uint64_t)cpl << 32);
            v->s.ibr[1] = UINT64_C(0x00ffffffffffffff) | (UINT64_C(1) << (56 + cpl));
            v->s.ibr[0] = ENTRY;
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
            v->s.ibr[0] = ENTRY + 16;
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
            v->s.ibr[1] &= ~(UINT64_C(1) << (56 + cpl));
            pmu_filter_step(v, 0, VCPU_EXIT_UNIMPLEMENTED, 0);
        }
        v->s.psr = UINT64_C(1) << 2;
        v->s.ibr[1] = UINT64_C(0x01ffffffffffffff);
        v->s.ibr[0] = ENTRY | 15;
        for (unsigned slot = 0; slot < 3; ++slot)
            pmu_filter_step(v, slot, VCPU_EXIT_BUDGET, 0);
        v->s.ibr[0] = ENTRY + 16;
        v->s.ibr[1] = UINT64_C(1) << 56;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        for (unsigned bit = 56; bit < 64; ++bit) {
            v->s.ibr[0] = ENTRY ^ (UINT64_C(1) << bit);
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
        }
        v->s.pmc[opc] &= ~(UINT64_C(8) << (model ? 60 : 48));
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        v->s.pmc[model ? 15 : 36] |= 1;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
        v->s.pmc[model ? 15 : 36] &= ~UINT64_C(1);
        v->s.pmc[opc] |= UINT64_C(8) << (model ? 60 : 48);
        /* Either ignore control overrides inversion and IBR settings. */
        v->s.ibr[1] = UINT64_C(1) << 63;
        v->s.pmc[ranges] |= 2;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
        v->s.pmc[ranges] &= ~UINT64_C(2);
        v->s.pmc[opc] |= ig_ad;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
        v->s.pmc[opc] &= ~ig_ad;
        pmu_filter_step(v, 0, VCPU_EXIT_UNIMPLEMENTED, 0);
        v->s.ibr[1] = UINT64_C(1) << 56;
        v->s.ibr[3] = UINT64_C(1) << 63;
        pmu_filter_step(v, 0, model ? VCPU_EXIT_UNIMPLEMENTED : VCPU_EXIT_BUDGET,
                        model ? 0 : 1);
        v->s.ibr[3] = 0;
        v->s.psr |= IA64_PSR_DB;
        pmu_filter_step(v, 0, model ? VCPU_EXIT_UNIMPLEMENTED : VCPU_EXIT_BUDGET,
                        model ? 0 : 1);
        v->s.psr &= ~IA64_PSR_DB;
        v->s.pmc[ranges] = (0xdb4 & ~UINT64_C(0x80)) | (UINT64_C(1) << 13);
        v->s.ibr[0] = ENTRY; v->s.ibr[4] = ENTRY + 32;
        pmu_filter_step(v, 0, VCPU_EXIT_UNIMPLEMENTED, 0);
        v->s.pmc[ranges] = 0xdb4;
        Ia64Instruction i = instruction(IA64_OP_NOP);
        i.raw = nop;
        struct ia64_pmu_retirement pending;
        v->s.psr |= IA64_PSR_IS;
        /* Select IA-32 events while checking the address tag directly. */
        v->s.pmc[4] = (v->s.pmc[4] & ~(UINT64_C(3) << 24)) | (UINT64_C(1) << 24);
        struct ia64_result r = ia64_pmu_prepare(v, &i, &pending);
        CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_UNIMPLEMENTED && !pending.count);
        v->s.pmc[ranges] |= 2;
        r = ia64_pmu_prepare(v, &i, &pending);
        CHECK(r.kind == IA64_STEP_COMPLETE && pending.count == (1u << 4));
        v->s.psr &= ~IA64_PSR_IS;
        /* The shared opcode matcher does not invert channels 1, 2 or 3. */
        for (unsigned channel = 1; channel < 4; ++channel) {
            unsigned pair = 2 * channel;
            v->s.pmc[4] = control | ((uint64_t)channel << 16);
            v->s.pmc[ranges] = 0xdb6 & ~(UINT64_C(1) << (1 + 3 * channel));
            v->s.ibr[pair] = ENTRY;
            v->s.ibr[pair + 1] = UINT64_C(0x01ffffffffffffff);
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
            v->s.ibr[pair] = ENTRY + 16;
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        }
        vcpu_destroy(v);
    }
}

/* Intel 251110-003 Tables 10-9..12, 11-68; 308065-001 Tables 3-10..13, 4-78. */
static void test_pmu_filters(void)
{
    const uint64_t nop = UINT64_C(1) << 27;
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        unsigned opc = model ? 8 : 32, ranges = model ? 14 : 38, ignore = model ? 15 : 36;
        unsigned type_shift = model ? 60 : 48, mask_shift = model ? 3 : 0;
        uint64_t control = (model ? UINT64_C(1) << 23 : UINT64_C(2) << 24) | 0x801;
        uint64_t opcode_mask = model ? UINT64_C(0x7ffffff) : UINT64_C(0x1ffffffffff);
        v->s.psr = UINT64_C(1) << 2;
        bundle_write(ENTRY, 0, nop, nop, nop);
        /* Each channel has an independent address ignore and opcode ignore. */
        for (unsigned channel = 0; channel < 4; ++channel) {
            v->s.pmc[4] = control | ((uint64_t)(channel | 12) << 16);
            v->s.pmc[ranges] = 0xdb6 & ~(UINT64_C(1) << (1 + 3 * channel));
            v->s.pmc[opc] &= ~(UINT64_C(3) << (model ? 0 : 56));
            v->s.ibr[2 * channel] = ENTRY;
            v->s.ibr[2 * channel + 1] = UINT64_C(0x01ffffffffffffff);
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
            v->s.ibr[2 * channel] = ENTRY + 16;
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
            v->s.ibr[2 * channel] = ENTRY | 15;
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
            v->s.ibr[2 * channel + 1] = UINT64_C(1) << 56;
            v->s.ibr[2 * channel] = ENTRY + 16;
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
            v->s.ibr[2 * channel] = ENTRY ^ (UINT64_C(1) << 56);
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
            v->s.ibr[2 * channel + 1] &= ~(UINT64_C(1) << 56);
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
            v->s.pmc[ranges] |= UINT64_C(1) << (1 + 3 * channel);
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
            unsigned matcher = opc + (channel & 1) * (model ? 1 : 2);
            v->s.pmc[matcher] = (opcode_mask << mask_shift) | (model ? 4 : 0);
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
            v->s.pmc[ignore] |= UINT64_C(1) << channel;
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
            v->s.pmc[ignore] &= ~(UINT64_C(1) << channel);
            v->s.pmc[matcher] |= UINT64_C(8) << type_shift;
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
            pmu_filter_step(v, 1, VCPU_EXIT_BUDGET, 0);
            v->s.pmc[matcher] |= UINT64_C(4) << type_shift;
            pmu_filter_step(v, 1, VCPU_EXIT_BUDGET, 1);
            v->s.pmc[matcher] = UINT64_MAX;
        }
        v->s.pmc[4] = control;
        v->s.pmc[ranges] = 0xdb6;
        v->s.pmc[opc] = (UINT64_C(3) << type_shift) | (opcode_mask << mask_shift) | (model ? 4 : 0);
        bundle_write(ENTRY, 0x1c, nop, nop | 1, (UINT64_C(4) << 37) | 1);
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        pmu_filter_step(v, 1, VCPU_EXIT_BUDGET, model ? 0 : 1);
        pmu_filter_step(v, 2, VCPU_EXIT_BUDGET, 1);
        bundle_write(ENTRY, 0, nop, nop, nop);
        /* Both manuals require M+I to select A-type instructions. */
        const uint64_t adds = (UINT64_C(8) << 37) | (UINT64_C(2) << 34) |
                              (UINT64_C(8) << 20) | (UINT64_C(1) << 13) | (UINT64_C(8) << 6);
        bundle_write(ENTRY, 0, adds, adds, nop);
        v->s.pmc[opc] = (UINT64_C(8) << type_shift) | (opcode_mask << mask_shift) | (model ? 4 : 0);
        pmu_filter_step(v, 0, VCPU_EXIT_UNIMPLEMENTED, 0);
        pmu_filter_step(v, 1, VCPU_EXIT_UNIMPLEMENTED, 0);
        v->s.pmc[opc] ^= UINT64_C(12) << type_shift;
        pmu_filter_step(v, 0, VCPU_EXIT_UNIMPLEMENTED, 0);
        v->s.pmc[opc] |= UINT64_C(12) << type_shift;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
        pmu_filter_step(v, 1, VCPU_EXIT_BUDGET, 1);
        v->s.pmc[opc] &= ~(UINT64_C(12) << type_shift);
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        bundle_write(ENTRY, 0, nop, nop, nop);
        /* Mask=1 ignores an encoding bit, unlike the IBR address mask. */
        unsigned major_bit = model ? 13 : 27;
        v->s.pmc[opc] = (UINT64_C(15) << type_shift) |
                        ((opcode_mask & ~(UINT64_C(1) << major_bit)) << mask_shift);
        if (model) v->s.pmc[opc] |= UINT64_C(5);
        else v->s.pmc[opc + 1] = 0;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        if (model) v->s.pmc[opc] |= UINT64_C(1) << (33 + major_bit);
        else v->s.pmc[opc + 1] = nop;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
        /* Madison omits encoding[26:13]; Montecito can test those bits. */
        if (!model) {
            v->s.pmc[opc] &= ~(UINT64_C(1) << 13);
            v->s.pmc[opc + 1] |= UINT64_C(1) << 13;
            pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        }
        bundle_write(ENTRY, 0, nop | (UINT64_C(1) << 13), nop, nop);
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
        bundle_write(ENTRY, 0, nop, nop, nop);
        v->s.pmc[opc] &= ~(UINT64_C(1) << (6 + mask_shift));
        if (model) v->s.pmc[opc] |= UINT64_C(1) << 39;
        else v->s.pmc[opc + 1] = nop | (UINT64_C(1) << 6);
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        bundle_write(ENTRY, 0, nop | (UINT64_C(1) << 6), nop, nop);
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 1);
        bundle_write(ENTRY, 0, nop, nop, nop);
        v->s.pmc[opc] = UINT64_MAX;
        /* The fine limits are exclusive and use different page widths. */
        unsigned bits = model ? 12 : 16;
        v->s.pmc[opc] &= ~(UINT64_C(3) << (model ? 0 : 56));
        v->s.pmc[ranges] = (0xdb6 | (UINT64_C(1) << 13)) & ~UINT64_C(0x82);
        v->s.ibr[0] = ENTRY; v->s.ibr[4] = ENTRY + 32;
        v->s.ibr[1] = UINT64_C(1) << 56;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        v->s.ibr[0] = ENTRY | 16; v->s.ibr[4] = ENTRY | 48;
        v->s.ip = ENTRY + 32; v->s.slot = 0; v->s.pmd[4] = 0;
        bundle_write(ENTRY + 32, 0, nop, nop, nop);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == 1);
        v->s.ibr[1] |= (UINT64_C(1) << bits) - 1;
        pmu_filter_step(v, 0, VCPU_EXIT_BUDGET, 0);
        v->s.ip = ENTRY + 32; v->s.slot = 0; v->s.pmd[4] = 0;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == 1);
        v->s.ip = ENTRY + 48; v->s.slot = 0; v->s.pmd[4] = 0;
        bundle_write(ENTRY + 48, 0, nop, nop, nop);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == 0);
        v->s.ibr[0] = ENTRY + 0xff0; v->s.ibr[4] = ENTRY + 0x1020;
        v->s.ip = ENTRY + 0x1000; v->s.slot = 0; v->s.pmd[4] = 0;
        bundle_write(ENTRY + 0x1000, 0, nop, nop, nop);
        CHECK(engine_run(v, 1).reason == (model ? VCPU_EXIT_UNIMPLEMENTED : VCPU_EXIT_BUDGET));
        CHECK(v->s.pmd[4] == (model ? 0u : 1u));
        v->s.pmc[4] = control | (UINT64_C(2) << 16);
        pmu_filter_step(v, 0, VCPU_EXIT_UNIMPLEMENTED, 0);
        v->s.pmc[4] = control;
        v->s.pmc[ranges] = 0xdb6;
        /* Long instructions count once with wildcard/ignored opcode checks.
         * Their opcode-matcher syllable and itype are not specified here. */
        bundle_write(ENTRY, 4, nop, 0x123, (UINT64_C(6) << 37) | (8u << 6));
        pmu_filter_step(v, 1, VCPU_EXIT_BUDGET, 1);
        v->s.pmc[opc] &= ~(UINT64_C(1) << mask_shift);
        pmu_filter_step(v, 1, VCPU_EXIT_UNIMPLEMENTED, 0);
        v->s.pmc[ignore] |= 1;
        pmu_filter_step(v, 1, VCPU_EXIT_BUDGET, 1);
        vcpu_destroy(v);
    }
    test_pmu_inverted_ranges();
    test_pmu_paired_fine_ranges();
}

static void test_pmu_transitions(void)
{
    const uint64_t nop = UINT64_C(1) << 27, up = UINT64_C(1) << 2;
    const uint64_t mov_pmc = (UINT64_C(1) << 37) | (UINT64_C(4) << 27) |
                             (UINT64_C(10) << 20) | (UINT64_C(8) << 13);
    const uint64_t mov_pmd = mov_pmc | (UINT64_C(1) << 27);
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        uint64_t control = model ? UINT64_C(1) << 23 : UINT64_C(2) << 24;
        /* Warm cached code while monitoring is disabled. */
        bundle_write(ENTRY, 0x11, nop, nop, UINT64_C(4) << 37);
        CHECK(engine_run(v, 96).reason == VCPU_EXIT_BUDGET);
        v->s.pmc[4] = control | 0x801; v->s.psr = up;
        CHECK(engine_run(v, 96).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == 96);
        /* A native PMC writer enables a previously ineligible monitor. */
        bundle_write(ENTRY, 0, mov_pmc, nop, nop);
        v->s.ip = ENTRY; v->s.slot = 0; v->s.pmd[4] = 0;
        v->s.pmc[4] = control | 0x800; v->s.gr[8] = control | 0x801; v->s.gr[10] = 4;
        CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == 1);
        v->s.ip = ENTRY; v->s.slot = 0;
        v->s.pmc[4] = control | 0x800; v->s.gr[8] = control | 0x100801;
        uint64_t before = v->retired;
        CHECK(engine_run(v, 2).reason == VCPU_EXIT_UNIMPLEMENTED && v->retired == before + 1);
        /* A native sum enables UP; the next cached instruction must count. */
        bundle_write(ENTRY, 0, (UINT64_C(4) << 27) | (UINT64_C(4) << 6), nop, nop);
        v->s.ip = ENTRY; v->s.slot = 0; v->s.psr = 0;
        v->s.pmc[4] = control | 0x801; v->s.pmd[4] = 0;
        CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == 1);
        /* Chosen deterministic behavior inside an active-write serialization
         * interval: old controls apply, and a monitored PMD replacement wins. */
        bundle_write(ENTRY, 0, mov_pmd, nop, nop);
        v->s.ip = ENTRY; v->s.slot = 0; v->s.gr[8] = 0x123; v->s.gr[10] = 4;
        v->s.pmc[7] = (model ? 0 : UINT64_C(2) << 24) | 0x801;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.pmd[4] == 0x123 && v->s.pmd[7] == 1);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == 0x124);
        bundle_write(ENTRY, 0, mov_pmc, nop, nop);
        v->s.ip = ENTRY; v->s.slot = 0; v->s.gr[8] = control | 0x800;
        CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == 0x125);
        v->s.pmc[4] = control | 0x801;
        bundle_write(ENTRY, 0, (UINT64_C(5) << 27) | (UINT64_C(4) << 6), nop, nop);
        v->s.ip = ENTRY; v->s.slot = 0; before = v->s.pmd[4];
        CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET && v->s.pmd[4] == before + 1);
        vcpu_destroy(v);
    }
}

static void test_pmu_faults(void)
{
    const uint64_t nop = UINT64_C(1) << 27;
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        uint64_t control = model ? UINT64_C(1) << 23 : UINT64_C(2) << 24;
        bundle_write(ENTRY, 0, 0, nop, nop);
        bundle_write(ENTRY + 0x2c00, 0, nop, nop, nop);
        v->s.pmc[4] = control | 0x841;
        v->s.psr = IA64_PSR_IC | (UINT64_C(1) << 21); v->s.cr[CR_IVA] = ENTRY;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->fault.kind == IA64_FAULT_BREAK && v->retired == 1 && !v->s.pmd[4]);
        /* Floating-point trap commits and retires before interruption. */
        uint64_t raw = (UINT64_C(8) << 37) | (UINT64_C(1) << 36) |
            (UINT64_C(9) << 27) | (UINT64_C(6) << 20) | (UINT64_C(6) << 6);
        bundle_write(ENTRY, 0x0c, nop, raw, nop);
        v->s.fr[6] = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS + 127, false};
        v->s.fr[9] = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS + 2, false};
        v->s.ar[AR_FPSR] = 0x327; v->s.ip = ENTRY; v->s.slot = 1;
        v->s.psr = IA64_PSR_IC | (UINT64_C(1) << 21);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->fault.kind == IA64_FAULT_FP_TRAP && v->retired == 2 && v->s.pmd[4] == 1);
        vcpu_destroy(v);
    }
}
