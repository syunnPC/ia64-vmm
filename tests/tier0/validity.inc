/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/interrupt.h"
static uint64_t validator_branch(gpa_t from, gpa_t to)
{
    uint64_t displacement = (to - from) >> 4;
    return (UINT64_C(4) << 37) | ((displacement & 0xfffff) << 13) |
        (((displacement >> 20) & 1) << 36);
}
/* Emulated device DMA: deliberately bypass gm_write/code_notify_*. */
static void dma_bundle(gpa_t ip, unsigned t, uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t low = t | (a << 5) | (b << 46), high = (b >> 18) | (c << 23);
    uint8_t *dst = ram + (ip - BASE);
    memcpy(dst, &low, 8); memcpy(dst + 8, &high, 8);
}
static void validator_run(struct vcpu *v, uint64_t count)
{ CHECK(engine_run(v, count).reason == VCPU_EXIT_BUDGET); }
static void validator_reuse(struct vcpu *v, enum engine_kind engine,
                            enum code_validity_mode mode, bool bump, bool cpu, bool vmm)
{
    CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
    gpa_t other = BASE + 4096;
    bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(BASE, other));
    bundle(other, 0x11, nop, nop, validator_branch(other, BASE));
    restart(v, engine, BASE); v->s.gr[9] = 11; v->s.gr[10] = 22;
    validator_run(v, 3); CHECK(v->s.gr[8] == 11 && v->s.ip == other);
    struct code_validity_stats before, after;
    code_validity_get_stats(&before);
    if (cpu) code_notify_cpu_write(BASE, 16);
    if (vmm) code_notify_vmm_write(BASE, 16);
    dma_bundle(BASE, 0x11, nop, add(8,10,0), validator_branch(BASE, other));
    if (bump) code_epoch_bump(CODE_DEVICE_STORAGE);
    validator_run(v, 3); CHECK(v->s.ip == BASE);
    validator_run(v, 2);
    bool detected = cpu || vmm || mode == CODE_VALIDITY_COMPARE_ALWAYS ||
        mode == CODE_VALIDITY_HW_DIRTY || (mode == CODE_VALIDITY_EPOCH_COMPARE && bump);
    CHECK(v->s.gr[8] == (detected ? 22 : 11));
    code_validity_get_stats(&after);
    CHECK(after.epoch_bumps == before.epoch_bumps + bump);
    if (mode == CODE_VALIDITY_NONE) CHECK(after.snapshot_bytes == 0);
    else if (detected && !cpu && !vmm) {
        CHECK(after.mismatches == before.mismatches + 1);
        CHECK(after.invalidations > before.invalidations);
    } else if (!detected) CHECK(after.comparisons == before.comparisons);
}
static enum code_test_stage race_stage;
static bool race_bump;
static unsigned race_calls;
static void validator_dma_race(gpa_t page, enum code_test_stage stage)
{
    if (page != BASE || stage != race_stage || race_calls) return;
    ++race_calls;
    dma_bundle(BASE, 0x11, nop, add(8,10,0), validator_branch(BASE, BASE + 4096));
    if (race_bump) code_epoch_bump(CODE_DEVICE_STORAGE);
}
static void validator_translation_race(struct vcpu *v, enum engine_kind engine,
                                       enum code_validity_mode mode, enum code_test_stage stage)
{
    CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(BASE, BASE + 4096));
    restart(v, engine, BASE); v->s.gr[9] = 11; v->s.gr[10] = 22;
    race_stage = stage; race_calls = 0; race_bump = mode == CODE_VALIDITY_EPOCH_COMPARE;
    struct code_validity_stats before, after; code_validity_get_stats(&before);
    code_validity_set_test_hook(validator_dma_race);
    validator_run(v, 2);
    code_validity_set_test_hook(NULL);
    CHECK(race_calls == 1 && v->s.gr[8] == 22 && v->retired == 2);
    code_validity_get_stats(&after);
    CHECK(after.mismatches == before.mismatches + 1);
}
static void validator_multiple_pages(struct vcpu *v, enum engine_kind engine,
                                    enum code_validity_mode mode)
{
    CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
    /* Each region stops at the page boundary. An unchanged entry page
     * must not let the stale translation on the next page survive. */
    gpa_t edge = BASE + 4096 - 16, second = BASE + 4096, away = BASE + 8192;
    bundle(edge, 1, nop, add(8,9,0), nop);
    bundle(second, 0x11, nop, add(11,9,0), validator_branch(second, away));
    bundle(away, 0x11, nop, nop, validator_branch(away, edge));
    restart(v, engine, edge); v->s.gr[9] = 11; v->s.gr[10] = 22;
    validator_run(v, 6); CHECK(v->s.ip == away && v->s.gr[11] == 11);
    dma_bundle(second, 0x11, nop, add(11,10,0), validator_branch(second, away));
    if (mode == CODE_VALIDITY_EPOCH_COMPARE) code_epoch_bump(CODE_DEVICE_STORAGE);
    validator_run(v, 3); CHECK(v->s.ip == edge);
    validator_run(v, 6); CHECK(v->s.gr[8] == 11 && v->s.gr[11] == 22);
    if (engine != ENGINE_INTERP) CHECK(tier0_page_has_code(BASE) && tier0_page_has_code(second));
}
static void validator_current_page(struct vcpu *v, enum engine_kind engine)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(BASE, 1, nop, add(8,9,0), nop);
    bundle(BASE + 4096, 0x11, nop, nop, validator_branch(BASE + 4096, BASE));
    restart(v, engine, BASE); v->s.gr[9] = 11; v->s.gr[10] = 22;
    validator_run(v, 1);
    dma_bundle(BASE, 1, nop, add(8,10,0), nop);
    validator_run(v, 1);
    /* Each engine_run call is a new stay: every engine
     * compares the bundle again, and Tier0 also publishes a new slot key. */
    CHECK(v->s.gr[8] == 22);
    v->s.ip = BASE + 4096; v->s.slot = 0;
    validator_run(v, 3); validator_run(v, 2); CHECK(v->s.gr[8] == 22);
}
static void validator_reclaims(struct vcpu *v, enum engine_kind engine)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, 12288));
    for (unsigned i = 0; i < 3; ++i)
        bundle(BASE + i * 4096, 0x11, nop, add(8,9,0), validator_branch(BASE + i * 4096, BASE + i * 4096));
    restart(v, engine, BASE); v->s.gr[9] = 11; v->s.gr[10] = 22;
    validator_run(v, 3);
    v->s.ip = BASE + 4096; validator_run(v, 3);
    v->s.ip = BASE; validator_run(v, 3); /* both pages have a recent entry tick */
    struct code_validity_stats before, after; code_validity_get_stats(&before);
    v->s.ip = BASE + 8192; validator_run(v, 3);
    code_validity_get_stats(&after);
    CHECK(after.evictions == before.evictions + 1 && after.snapshot_bytes == 8192);
    CHECK(code_page_has_code(BASE) != code_page_has_code(BASE + 4096)); /* clock victim */
    dma_bundle(BASE + 4096, 0x11, nop, add(8,10,0), validator_branch(BASE + 4096, BASE + 4096));
    v->s.ip = BASE + 4096; validator_run(v, 3); CHECK(v->s.gr[8] == 22);
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, 8192));
    restart(v, engine, BASE); validator_run(v, 1);
    v->s.ip = BASE + 4096; v->s.slot = 0; validator_run(v, 1);
    CHECK(!code_page_has_code(BASE) && code_page_has_code(BASE + 4096));
}
static void validator_interruption_entries(struct vcpu *v, enum engine_kind engine)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    gpa_t handler = BASE + 0x3000;
    bundle(BASE, 1, nop, add(8,9,0), nop);
    bundle(handler, 0x11, nop, add(11,9,0), UINT64_C(8) << 27); /* rfi */
    restart(v, engine, handler); v->s.gr[9] = 11; v->s.gr[10] = 22;
    validator_run(v, 2); CHECK(v->s.gr[11] == 11);
    v->s.ip = BASE; v->s.slot = 0; v->s.cr[CR_IVA] = BASE;
    v->s.psr = IA64_PSR_I | IA64_PSR_IC;
    validator_run(v, 1);
    dma_bundle(handler, 0x11, nop, add(11,10,0), UINT64_C(8) << 27);
    vcpu_raise_external(v, 150);
    validator_run(v, 2); CHECK(v->s.gr[11] == 22 && v->s.ip == handler && v->s.slot == 2);
    CHECK(ia64_ivr_read(v) == 150);
    /* The interrupted page was cached; rfi must validate it again. */
    dma_bundle(BASE, 1, nop, add(8,10,0), nop);
    validator_run(v, 1); CHECK(v->s.ip == BASE && v->s.slot == 1);
    validator_run(v, 1); CHECK(v->s.gr[8] == 22);
}
static void validator_other_executor(struct vcpu *v, struct vcpu *other, enum engine_kind engine)
{
    CHECK(code_validity_configure(CODE_VALIDITY_EPOCH_COMPARE, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(BASE, BASE));
    restart(v, engine, BASE); v->s.gr[9] = 11; v->s.gr[10] = 22; validator_run(v, 3);
    dma_bundle(BASE, 0x11, nop, add(8,10,0), validator_branch(BASE, BASE));
    code_epoch_bump(CODE_DEVICE_STORAGE);
    restart(other, engine, BASE); other->s.gr[9] = 11; other->s.gr[10] = 22;
    validator_run(other, 3); CHECK(other->s.gr[8] == 22);
    /* A warm old executor cannot keep using an invalidated decoded entry. */
    validator_run(v, 3); CHECK(v->s.gr[8] == 22);
}
static void validator_snapshot_store(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(BASE, 1, nop, add(8,11,0), nop);
    bundle(BASE + 0x40000, 1, nop, nop, nop);
    bundle(BASE + 0x60000, 1, nop, nop, nop);
    restart(v, ENGINE_TIER0, BASE); v->s.gr[11] = 11; v->s.gr[12] = 22;
    validator_run(v, 2); CHECK(v->s.gr[8] == 11);
    const struct ia64_bundle *decoded;
    CHECK(code_get_bundle(v, BASE + 0x40000, &decoded).kind == IA64_STEP_COMPLETE);
    /* Both decoded-cache ways must be occupied to evict BASE. */
    CHECK(code_get_bundle(v, BASE + 0x60000, &decoded).kind == IA64_STEP_COMPLETE);
    tier0_flush(); /* removes the remaining generated dependency of BASE */
    CHECK(code_page_has_snapshot(BASE) && !code_page_has_code(BASE));
    CHECK(!tier0_page_has_code(BASE));
    bundle(BASE + 4096, 0x11, store(3,9,10), nop, validator_branch(BASE + 4096, BASE));
    v->s.gr[9] = 1 | (nop << 5) | (add(8,12,0) << 46); v->s.gr[10] = BASE;
    v->s.ip = BASE + 4096; v->s.slot = 0;
    validator_run(v, 3); validator_run(v, 2); CHECK(v->s.gr[8] == 22);
}
static void validator_same_page_entries(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(BASE, 0x11, nop, nop, validator_branch(BASE, BASE));
    bundle(BASE + 4096, 0x11, nop, nop, validator_branch(BASE + 4096, BASE));
    restart(v, ENGINE_TIER0, BASE);
    validator_run(v, 3); validator_run(v, 3); /* seal and warm the key */
    struct code_validity_stats before, after;
    struct tier0_stats t_before, t_after;
    code_validity_get_stats(&before); tier0_get_stats(&t_before);
    /* One engine_run call, one stay: its first entry compares, the other 99
     * entries of the same page do not. */
    validator_run(v, 300);
    code_validity_get_stats(&after); tier0_get_stats(&t_after);
    CHECK(after.comparisons == before.comparisons + 1);
    /* Each boundary looked the region up or took it from the boundary cache. */
    CHECK(t_after.lookups + t_after.boundary_hits == t_before.lookups + t_before.boundary_hits + 100);
    CHECK(t_after.cross_page_validations == t_before.cross_page_validations + 1);
    v->s.ip = BASE + 4096; validator_run(v, 3);
    code_validity_get_stats(&before); tier0_get_stats(&t_before);
    validator_run(v, 3);
    code_validity_get_stats(&after); tier0_get_stats(&t_after);
    CHECK(after.comparisons == before.comparisons + 1);
    CHECK(t_after.cross_page_validations == t_before.cross_page_validations + 1);
}
static void validator_seal_race(struct vcpu *v, enum code_validity_mode mode)
{
    CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(BASE, BASE));
    restart(v, ENGINE_TIER0, BASE); v->s.gr[9] = 11; v->s.gr[10] = 22;
    validator_run(v, 3); CHECK(v->s.gr[8] == 11);
    race_stage = CODE_TEST_SEAL; race_calls = 0; race_bump = mode == CODE_VALIDITY_EPOCH_COMPARE;
    struct code_validity_stats before, after; code_validity_get_stats(&before);
    code_validity_set_test_hook(validator_dma_race);
    validator_run(v, 2);
    code_validity_set_test_hook(NULL);
    code_validity_get_stats(&after);
    CHECK(race_calls == 1 && v->s.gr[8] == 22 && v->retired == 5);
    CHECK(after.mismatches == before.mismatches + 1);
}
static void validator_vmm_resume(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(BASE, 0x11, load(3,8,10), nop, validator_branch(BASE, BASE));
    restart(v, ENGINE_TIER0, BASE); v->s.gr[10] = DEVICE;
    validator_run(v, 3); validator_run(v, 3);
    struct code_validity_stats before, after; code_validity_get_stats(&before);
    request_exit = true;
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_HALT);   /* a new stay: one comparison */
    request_exit = false;
    code_validity_get_stats(&after); CHECK(after.comparisons == before.comparisons + 1);
    CHECK(!v->code_entry_generation);
    validator_run(v, 2); /* same page, but a new stay after the HALT */
    code_validity_get_stats(&after); CHECK(after.comparisons > before.comparisons + 1);
}
static void validator_virtual_entry(struct vcpu *v, enum engine_kind engine)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    const uint64_t alias = UINT64_C(0x400000);
    bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(BASE, BASE + 4096));
    bundle(BASE + 4096, 0x11, nop, nop, validator_branch(BASE + 4096, BASE));
    restart(v, engine, alias); v->s.psr = IA64_PSR_IT;
    for (unsigned i = 0; i < 2; ++i)
        v->s.tr[0][i] = (struct ia64_tlb_entry){.va = alias + i * 4096,
            .pte = (BASE + i * 4096) | 0x7e1, .itir = 48, .page_shift = 12, .valid = true};
    v->s.gr[9] = 11; v->s.gr[10] = 22;
    validator_run(v, 3); CHECK(v->s.gr[8] == 11 && v->s.ip == alias + 4096);
    dma_bundle(BASE, 0x11, nop, add(8,10,0), validator_branch(BASE, BASE + 4096));
    validator_run(v, 3); validator_run(v, 2);
    CHECK(v->s.gr[8] == 22 && v->code_page == BASE);
}
static void test_validator(struct vcpu *v, struct vcpu *other)
{
    enum code_validity_mode parsed;
    CHECK(code_validity_parse_mode("compare-always", 14, &parsed) && parsed == CODE_VALIDITY_COMPARE_ALWAYS);
    CHECK(code_validity_parse_mode("epoch-compare", 13, &parsed) && parsed == CODE_VALIDITY_EPOCH_COMPARE);
    CHECK(code_validity_parse_mode("none", 4, &parsed) && parsed == CODE_VALIDITY_NONE);
    CHECK(code_validity_parse_mode("hw-dirty", 8, &parsed) && parsed == CODE_VALIDITY_HW_DIRTY);
    CHECK(!code_validity_parse_mode("epoch", 5, &parsed));
    CHECK(!code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, 0));
    CHECK(!code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, 4097));
    CHECK(!code_validity_configure((enum code_validity_mode)99, 4096));
    struct code_validity_stats before, after; code_validity_get_stats(&before);
    code_epoch_bump(CODE_DEVICE_OTHER); code_validity_get_stats(&after);
    CHECK(before.epoch_bumps == after.epoch_bumps);
    for (enum engine_kind engine = ENGINE_INTERP; engine <= ENGINE_LOCKSTEP; ++engine) {
        validator_reuse(v, engine, CODE_VALIDITY_COMPARE_ALWAYS, false, false, false);
        validator_reuse(v, engine, CODE_VALIDITY_EPOCH_COMPARE, false, false, false); /* miss */
        validator_reuse(v, engine, CODE_VALIDITY_EPOCH_COMPARE, true, false, false);
        validator_reuse(v, engine, CODE_VALIDITY_NONE, false, false, false);
        validator_reuse(v, engine, CODE_VALIDITY_HW_DIRTY, false, false, false);
        validator_reuse(v, engine, CODE_VALIDITY_EPOCH_COMPARE, false, true, false);
        validator_reuse(v, engine, CODE_VALIDITY_EPOCH_COMPARE, false, false, true);
        for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_EPOCH_COMPARE; ++mode) {
            validator_translation_race(v, engine, mode, CODE_TEST_SNAPSHOT);
            validator_translation_race(v, engine, mode, CODE_TEST_DECODE);
            if (engine != ENGINE_INTERP) validator_translation_race(v, engine, mode, CODE_TEST_PUBLISH);
            validator_multiple_pages(v, engine, mode);
        }
        validator_current_page(v, engine);
        validator_reclaims(v, engine);
        validator_interruption_entries(v, engine);
        validator_other_executor(v, other, engine);
        validator_virtual_entry(v, engine);
    }
    validator_snapshot_store(v);
    validator_same_page_entries(v);
    validator_seal_race(v, CODE_VALIDITY_COMPARE_ALWAYS);
    validator_seal_race(v, CODE_VALIDITY_EPOCH_COMPARE);
    validator_vmm_resume(v);
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    code_validity_get_stats(&after); CHECK(after.snapshot_bytes == 0);
}
