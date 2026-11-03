/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Self-test of the comparison relation: two
 * interpreter runs of tests/tier1/programs/selftest.S from one snapshot -- one
 * whole, one split into small budgets -- compare equal; every compared
 * field that is perturbed is reported, every excluded field is not; the
 * outer exit and the in-place page-hash form are compared. The exit-outcome
 * tests across engines (HALT included) are in dispatch_tests.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/compare.h"
#include "tests/tier1/harness.h"
#include "tests/tier1/results.h"
#include "tests/tier1/snapshot.h"
#include "engine/interp/interp.h"

#define DATA (T1H_RAM_BASE + 0x8000)
#define CHECK T1H_CHECK

/* Runs v to the end marker in slices of `slice` instructions (0 = whole);
 * returns the final (outermost) exit. */
static struct vcpu_exit run(struct vcpu *v, uint64_t slice)
{
    for (unsigned n = 0; n < 100000; ++n) {
        struct vcpu_exit e = interp_run(v, slice ? slice : UINT64_C(1000000));
        if (e.reason == VCPU_EXIT_DEBUG)
            return e;
        CHECK(e.reason == VCPU_EXIT_BUDGET);
        slice = slice ? slice % 7 + 1 : 0; /* budgets 1..7, then cycle */
    }
    CHECK(!"selftest program did not reach its end marker");
    return (struct vcpu_exit){0};
}

struct expect {
    unsigned count;
    char what[64];
    uint64_t index;
};
static void note(const char *what, uint64_t index, uint64_t a, uint64_t b, void *arg)
{
    (void)a;
    (void)b;
    struct expect *e = arg;
    if (e->count++ == 0) {
        snprintf(e->what, sizeof(e->what), "%s", what);
        e->index = index;
    }
}
static struct expect compare(const struct compare_run *a, const struct compare_run *b)
{
    struct expect e = {0};
    unsigned n = compare_runs(a, b, note, &e);
    CHECK(n == e.count);
    return e;
}

/* Exit outcomes of interp vs interp from one snapshot:
 * budget 0, a requested exit, an access outside every region. */
static void outcomes(const uint8_t *ram_image)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *x = vcpu_create(&cfg), *y = vcpu_create(&cfg);
    CHECK(x && y);
    struct vcpu_exit ex, ey;
    struct compare_run a = {&ex, x, ram_image, T1H_RAM_SIZE, NULL, 0, NULL, 0, NULL, 0};
    struct compare_run b = {&ey, y, ram_image, T1H_RAM_SIZE, NULL, 0, NULL, 0, NULL, 0};
    vcpu_reset(x, &(struct ia64_entry_regs){.ip = T1H_CODE, .rsc = 3});

    *y = *x;
    ex = interp_run(x, 0);
    ey = interp_run(y, 0);
    CHECK(ex.reason == VCPU_EXIT_BUDGET && compare(&a, &b).count == 0);

    vcpu_request_exit(x, VCPU_EXIT_HALT);
    *y = *x;
    ex = interp_run(x, 100);
    ey = interp_run(y, 100);
    CHECK(ex.reason == VCPU_EXIT_HALT && compare(&a, &b).count == 0);
    *y = *x; /* request consumed on x */
    vcpu_request_exit(y, VCPU_EXIT_HALT);
    ex = interp_run(x, 0);
    ey = interp_run(y, 100);
    struct expect e = compare(&a, &b);
    CHECK(e.count >= 1 && !strcmp(e.what, "exit.reason"));

    x->s.ip = T1H_UNMAPPED;
    x->s.slot = 0;
    *y = *x; /* no region there */
    ex = interp_run(x, 10);
    ey = interp_run(y, 10);
    CHECK(ex.reason == VCPU_EXIT_BAD_ACCESS && compare(&a, &b).count == 0);
    vcpu_destroy(x);
    vcpu_destroy(y);
}

/* The in-place form: page hashes instead of bytes. */
static void page_hashes(const uint8_t *ram_a, const struct vcpu *v)
{
    static uint64_t ha[T1H_RAM_PAGES], hb[T1H_RAM_PAGES];
    for (unsigned i = 0; i < T1H_RAM_PAGES; ++i)
        ha[i] = hb[i] = t1h_page_hash(ram_a + 4096 * i);
    struct compare_run a = {NULL, v, NULL, 0, NULL, 0, NULL, 0, ha, T1H_RAM_PAGES};
    struct compare_run b = {NULL, v, NULL, 0, NULL, 0, NULL, 0, hb, T1H_RAM_PAGES};
    CHECK(compare(&a, &b).count == 0);
    static uint8_t page[4096];
    memcpy(page, ram_a + 4096 * 8, 4096);
    page[123] ^= 4;
    hb[8] = t1h_page_hash(page);
    struct expect e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "ram_page") && e.index == 8);
    /* Moving a word within a page changes the hash. */
    memcpy(page, ram_a + 4096 * 8, 4096);
    uint64_t w0, w1;
    memcpy(&w0, page, 8);
    memcpy(&w1, page + 8, 8);
    w0 ^= 0x5a;
    memcpy(page, &w1, 8);
    memcpy(page + 8, &w0, 8);
    CHECK(t1h_page_hash(page) != t1h_page_hash(ram_a + 4096 * 8) || w0 == w1);
    hb[8] = ha[8];
    struct compare_run c = {NULL, v, ram_a, T1H_RAM_SIZE, NULL, 0, NULL, 0, NULL, 0};
    e = compare(&a, &c);
    CHECK(e.count == 1 && !strcmp(e.what, "ram_form"));
}

void compare_test(void);
void compare_test(void)
{
    CHECK(compare_lists_complete());
    t1h_load_image("selftest.bin");
    uint8_t *ram = t1h_ram();
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t x = (i * UINT64_C(0x9e3779b97f4a7c15)) >> 57; /* 0..127 */
        memcpy(ram + (DATA - T1H_RAM_BASE) + 8 * i, &x, 8);
    }
    code_notify_vmm_write(DATA, 512);

    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *ref = vcpu_create(&cfg), *got = vcpu_create(&cfg);
    CHECK(ref && got);
    static struct t1h_logs ref_logs, got_logs;
    t1h_bind(ref, &ref_logs);
    t1h_bind(got, &got_logs);
    vcpu_reset(ref, &(struct ia64_entry_regs){.ip = T1H_CODE, .rsc = 3});
    static struct snapshot snap;
    snapshot_init(&snap);
    for (unsigned i = 0; i < 62; ++i)
        ref->pal_min_state[i] = 0x1000 + i;
    ref->pal_state_address = DATA;
    ref->pal_state_pending = 2;
    ref->pal_state_valid = true;
    snapshot_take(&snap, ref);
    snapshot_restore(&snap, got);
    CHECK(!memcmp(got->pal_min_state, ref->pal_min_state, sizeof(ref->pal_min_state)));
    CHECK(got->pal_state_address == DATA && got->pal_state_pending == 2 && got->pal_state_valid);
    ref->pal_state_pending = got->pal_state_pending = snap.v.pal_state_pending = 0;
    ref->pal_state_valid = got->pal_state_valid = snap.v.pal_state_valid = false;

    struct run_result a_res, b_res;
    result_init(&a_res);
    result_init(&b_res);
    struct vcpu_exit ref_exit = run(ref, 0);
    result_take(&a_res, &ref_exit, ref);
    snapshot_restore(&snap, got);
    struct vcpu_exit got_exit = run(got, 1);
    result_take(&b_res, &got_exit, got);
    const uint8_t *ref_ram = a_res.ram;
    uint8_t *got_ram = b_res.ram;

    /* The program did its work: 64 iterations, stores, both device accesses. */
    CHECK(ref->retired > 64 * 10 && ref_logs.device_n == 2 && ref_logs.device[0].write &&
          !ref_logs.device[1].write);
    CHECK(memcmp(ref_ram, snap.ram, T1H_RAM_SIZE) != 0);

    struct compare_run a = {&ref_exit,         ref,  ref_ram, T1H_RAM_SIZE, ref_logs.device,
                            ref_logs.device_n, NULL, 0,       NULL,         0};
    struct compare_run b = {&got_exit,         got,  got_ram, T1H_RAM_SIZE, got_logs.device,
                            got_logs.device_n, NULL, 0,       NULL,         0};
    struct expect e = compare(&a, &b);
    if (e.count)
        fprintf(stderr, "first difference: %s[%llu]\n", e.what, (unsigned long long)e.index);
    CHECK(e.count == 0);
    CHECK(result_compare(&a_res, &b_res, false, "selftest") == 0);

    /* Compared fields are reported ... */
    static struct vcpu copy;
    b.v = &copy;
#define PERTURB(stmt, name, idx)                                                                   \
    do {                                                                                           \
        copy = *got;                                                                               \
        stmt;                                                                                      \
        e = compare(&a, &b);                                                                       \
        CHECK(e.count == 1 && !strcmp(e.what, name) && e.index == (idx));                          \
    } while (0)
    PERTURB(copy.s.gr[5] ^= 1, "s.gr", 5);
    PERTURB(copy.s.nat[40] ^= 1, "s.nat", 40);
    PERTURB(copy.s.fr[7].exponent ^= 1, "s.fr[].exponent", 7);
    PERTURB(copy.s.tc[1][3].rid ^= 1, "s.tc[][].rid", 131);
    PERTURB(copy.s.tc[1][127].pte ^= 1, "s.tc[][].pte", 255);
    PERTURB(copy.s.tr[1][63].va ^= 1, "s.tr[][].va", 127);
    PERTURB(copy.s.cpu_model ^= 1, "s.cpu_model", 0);
    PERTURB(copy.s.pal.copy_valid ^= 1, "s.pal.copy_valid", 0);
    PERTURB(copy.s.slot ^= 1, "s.slot", 0);
    PERTURB(copy.retired += 1, "retired", 0);
    PERTURB(copy.last_successful_bundle += 16, "last_successful_bundle", 0);
    PERTURB(copy.in_service[2] ^= 1, "in_service", 2);
    PERTURB(copy.requested_exit = 1, "requested_exit", 0);
    PERTURB(copy.pal_min_state[61] ^= 1, "pal_min_state", 61);
    PERTURB(copy.pal_state_address ^= 512, "pal_state_address", 0);
    PERTURB(copy.pal_state_pending = 2, "pal_state_pending", 0);
    PERTURB(copy.pal_state_valid = true, "pal_state_valid", 0);
    /* ... excluded ones are not. */
#define IGNORED(stmt)                                                                              \
    do {                                                                                           \
        copy = *got;                                                                               \
        stmt;                                                                                      \
        CHECK(compare(&a, &b).count == 0);                                                         \
    } while (0)
    IGNORED(copy.fault.kind = IA64_FAULT_ILLEGAL);
    IGNORED(copy.tlb_index.ready ^= 1);
    IGNORED(copy.timer_last += 99);
    IGNORED(copy.physical_page ^= 0x1000);
    IGNORED(copy.code_page ^= 0x1000);
    IGNORED(copy.code_entry_generation += 1);
    IGNORED(copy.clock_origin += 1);
    IGNORED(copy.cfg.itc_hz += 1);
    b.v = got;

    /* The outer vcpu_exit: same state, other reason. */
    struct vcpu_exit other = got_exit;
    b.exit = &other;
    other.reason = VCPU_EXIT_BUDGET;
    e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "exit.reason"));
    other = got_exit;
    other.slot ^= 1;
    e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "exit.slot"));
    other = got_exit;
    other.detail = "another diagnostic";
    CHECK(compare(&a, &b).count == 0);
    b.exit = NULL;
    e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "exit_present"));
    b.exit = &got_exit;

    got_ram[0x8010] ^= 0x80;
    e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "ram") && e.index == 0x8010);
    got_ram[0x8010] ^= 0x80;
    got_ram[T1H_RAM_SIZE - 1] ^= 1; /* last byte of the last page */
    e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "ram") && e.index == T1H_RAM_SIZE - 1);
    got_ram[T1H_RAM_SIZE - 1] ^= 1;
    got_logs.device[1].value ^= 1;
    e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "device[].value") && e.index == 1);
    got_logs.device[1].value ^= 1;
    b.device_count = 1;
    e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "device_count"));
    b.device_count = got_logs.device_n;

    struct compare_store_record s1[2] = {{DATA, 1, 8}, {DATA + 8, 2, 8}},
                                s2[2] = {{DATA, 1, 8}, {DATA + 8, 2, 8}};
    a.stores = s1;
    a.store_count = 2;
    e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "store_log_present"));
    b.stores = s2;
    b.store_count = 2;
    CHECK(compare(&a, &b).count == 0);
    s2[1].gva += 8;
    e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "store[].gva") && e.index == 1);
    s2[1].gva -= 8;
    s1[0].width = s2[0].width = 2;
    s2[0].value |= UINT64_C(0xffff0000); /* beyond the width */
    CHECK(compare(&a, &b).count == 0);
    s2[0].value ^= 0x100;
    e = compare(&a, &b);
    CHECK(e.count == 1 && !strcmp(e.what, "store[].value") && e.index == 0);
    a.stores = b.stores = NULL;

    page_hashes(ref_ram, got);
    outcomes(ref_ram);

    printf("compare: interp vs split interp equal after %llu instructions; "
           "compared and excluded fields classified\n",
           (unsigned long long)ref->retired);
    t1h_unbind(ref);
    t1h_unbind(got);
    vcpu_destroy(ref);
    vcpu_destroy(got);
    result_free(&a_res);
    result_free(&b_res);
    snapshot_free(&snap);
}
