/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmpxchg1/2/4 .acq/.rel whose ar.ccv does not fit the operand width
 * can never compare equal (SDM Vol.3 cmpxchg: the memory value is zero
 * extended and compared with all 64 bits of AR[CCV]). The instruction
 * returns the zero-extended value and leaves memory unchanged, even when
 * the low bytes of ar.ccv equal memory (a truncating compare would write).
 * Every engine executes the real encodings. The ordering half of the fix
 * (MFENCE before that read) is checked on the generated code by
 * tests/review/check_codegen.py.
 */
#include <stdbool.h>
#include <string.h>
#include "tests/review/review.h"
#include "ia64/state.h"
#include "ia64vmm/guest_mem.h"

#define CODE UINT64_C(0x100000)
#define DATA UINT64_C(0x108000)
static uint8_t ram[0x10000] __attribute__((aligned(4096)));

/* cmpxchg<size>.<sem> r8 = [r9], r10, ar.ccv ; nop.i ; nop.i ;;  (GNU as) */
static const uint64_t bundles[4][2][2] = {
    {{UINT64_C(0x0000100112284001), UINT64_C(0x0004000000000200)},  /* cmpxchg1.acq */
     {UINT64_C(0x0000102112284001), UINT64_C(0x0004000000000200)}}, /* cmpxchg1.rel */
    {{UINT64_C(0x0000100912284001), UINT64_C(0x0004000000000200)},  /* cmpxchg2.acq */
     {UINT64_C(0x0000102912284001), UINT64_C(0x0004000000000200)}}, /* cmpxchg2.rel */
    {{UINT64_C(0x0000101112284001), UINT64_C(0x0004000000000200)},  /* cmpxchg4.acq */
     {UINT64_C(0x0000103112284001), UINT64_C(0x0004000000000200)}}, /* cmpxchg4.rel */
    {{UINT64_C(0x0000101912284001), UINT64_C(0x0004000000000200)},  /* cmpxchg8.acq */
     {UINT64_C(0x0000103912284001), UINT64_C(0x0004000000000200)}}, /* cmpxchg8.rel */
};

/* Executes one cmpxchg and checks r8, memory and the retirement. */
static void run(enum engine_kind engine, unsigned log2, unsigned rel, uint64_t memory, uint64_t ccv,
                uint64_t value, bool swaps)
{
    unsigned size = 1u << log2;
    uint64_t mask = size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
    CHECK(gm_write(CODE, bundles[log2][rel], 16));
    uint8_t before[64];
    for (unsigned i = 0; i < sizeof(before); ++i)
        before[i] = (uint8_t)(0xa5 ^ (i * 37));
    uint64_t at = DATA + 16;
    memcpy(before + 16, &memory, size);
    CHECK(gm_write(DATA, before, sizeof(before)));
    struct vcpu_config cfg = {.engine = engine, .itc_hz = 400000000};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    struct ia64_entry_regs regs = {.ip = CODE, .psr = UINT64_C(1) << 44};
    vcpu_reset(v, &regs);
    ia64_gr_write(&v->s, 8, UINT64_C(0xdeadbeefdeadbeef), true);
    ia64_gr_write(&v->s, 9, at, false);
    ia64_gr_write(&v->s, 10, value, false);
    v->s.ar[AR_CCV] = ccv;
    struct vcpu_exit e = engine_run(v, 1);
    CHECK(e.reason == VCPU_EXIT_BUDGET && v->retired == 1);
    CHECK(v->s.ip == CODE && v->s.slot == 1);
    CHECK(ia64_gr_read(&v->s, 8) == (memory & mask) && !ia64_nat_read(&v->s, 8));
    uint8_t after[64];
    CHECK(gm_read(DATA, after, sizeof(after)));
    if (swaps)
        memcpy(before + 16, &value, size);
    if (memcmp(before, after, sizeof(after)))
        fprintf(stderr, "review: engine %u cmpxchg%u.%s memory=%llx ccv=%llx changed memory\n",
                (unsigned)engine, size, rel ? "rel" : "acq", (unsigned long long)memory,
                (unsigned long long)ccv);
    CHECK(!memcmp(before, after, sizeof(after)));
    vcpu_destroy(v);
}

static void review_semaphore(void)
{
    CHECK(gm_add_region(&(struct gm_region){
        .base = CODE, .size = sizeof(ram), .kind = GM_RAM, .name = "review-ram", .hva = ram}));
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    for (unsigned engine = ENGINE_INTERP; engine <= ENGINE_LOCKSTEP; ++engine)
        for (unsigned log2 = 0; log2 < 3; ++log2)
            for (unsigned rel = 0; rel < 2; ++rel) {
                unsigned bits = 8u << log2;
                uint64_t top = UINT64_C(1) << bits, low = 0x5a & (top - 1);
                /* Out-of-range ccv whose low bytes equal memory: never equal. */
                run(engine, log2, rel, low, top | low, 0x77, false);
                run(engine, log2, rel, low, UINT64_MAX, 0x77, false);
                run(engine, log2, rel, top - 1, ((top - 1) << 1) | 1, 0x77, false);
                run(engine, log2, rel, 0, top, 0x77, false);
                /* In range: equal swaps the low bytes of r10, unequal does not. */
                run(engine, log2, rel, low, low, UINT64_C(0x1122334455667788), true);
                run(engine, log2, rel, low, low ^ 1, UINT64_C(0x1122334455667788), false);
            }
    /* cmpxchg8: every ccv is in range. */
    for (unsigned engine = ENGINE_INTERP; engine <= ENGINE_LOCKSTEP; ++engine)
        for (unsigned rel = 0; rel < 2; ++rel) {
            run(engine, 3, rel, UINT64_C(0x8000000000000001), UINT64_C(0x8000000000000001), 9,
                true);
            run(engine, 3, rel, UINT64_C(0x8000000000000001), 1, 9, false);
        }
}
REVIEW_SUITE("semaphore", review_semaphore)
