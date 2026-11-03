/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "tests/review/review.h"
#include "ia64/mmu.h"
#include "ia64/tlb.h"
#include "engine/tier0/internal.h"
#include "ia64vmm/guest_mem.h"

#define PA UINT64_C(0x700000)
#define VA UINT64_C(0x800000)
static uint8_t ram[4096] __attribute__((aligned(4096)));
static struct vcpu *inject_deferred;
/* Scalar Tier0 references currently pass speculative=false, so a deferred
 * success cannot occur naturally. Link-wrap only this review binary to
 * exercise the defensive result guard without altering product semantics. */
struct ia64_result __real_ia64_translate(struct vcpu *, uint64_t, enum ia64_reference, unsigned,
                                         unsigned, bool, struct ia64_translation *);
struct ia64_result __wrap_ia64_translate(struct vcpu *, uint64_t, enum ia64_reference, unsigned,
                                         unsigned, bool, struct ia64_translation *);
struct ia64_result __wrap_ia64_translate(struct vcpu *v, uint64_t address, enum ia64_reference ref,
                                         unsigned rights, unsigned level, bool speculative,
                                         struct ia64_translation *out)
{
    struct ia64_result r = __real_ia64_translate(v, address, ref, rights, level, speculative, out);
    if (v == inject_deferred && r.kind == IA64_STEP_COMPLETE)
        out->deferred = true;
    return r;
}
static struct vcpu *fresh(void)
{
    struct vcpu_config cfg = {.itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v);
    v->s.psr = IA64_PSR_DT | IA64_PSR_IC;
    v->s.ip = 0x900000;
    v->s.gr[8] = VA + 16;
    v->s.gr[9] = 0xaabbccdd;
    v->s.gr[10] = 0xdeadbeef;
    return v;
}
static void map(struct vcpu *v, uint64_t pte)
{
    Ia64Instruction i = {.opcode = IA64_OP_ITC_D};
    i.operands.system.source = 2;
    v->s.gr[2] = PA | pte;
    v->s.cr[CR_IFA] = VA;
    v->s.cr[CR_ITIR] = 12 << 2;
    CHECK(ia64_tlb(v, &i).kind == IA64_STEP_COMPLETE);
}
static void track(struct vcpu *v)
{
    const struct ia64_bundle *bundle;
    CHECK(code_get_bundle(v, PA + 16, &bundle).kind == IA64_STEP_COMPLETE);
    CHECK(code_page_has_code(PA));
}
static void compare(struct vcpu *v, bool store, bool fast, enum ia64_fault_kind fault, bool code)
{
    v->s.slot = 0;
    Ia64Instruction i = {.opcode = store ? IA64_OP_ST8 : IA64_OP_LD8,
                         .valid = true,
                         .unit = IA64_UNIT_M,
                         .address = v->s.ip,
                         .slot = 0};
    i.operands.memory.base = 8;
    i.operands.memory.source = 9;
    i.operands.memory.destination = 10;
    struct vcpu *ref = fresh();
    *ref = *v;
    memset(ram, 0, sizeof(ram));
    uint64_t value = UINT64_C(0x123456789abcdef0);
    memcpy(ram + 16, &value, 8);
    if (code)
        track(v);
    static struct tier0_dtc_entry dtc[TIER0_DTC_SLOTS];
    struct tier0_execution e = {.dtc = dtc};
    CHECK((tier0_data_pointer(v, &i, &e) != NULL) == fast);
    uint8_t original[sizeof(ram)], expected[sizeof(ram)];
    memcpy(original, ram, sizeof(ram));
    struct ia64_result a = ia64_execute(ref, &i);
    CHECK(a.fault == fault);
    memcpy(expected, ram, sizeof(ram));
    memcpy(ram, original, sizeof(ram));
    code_cache_flush();
    if (code)
        track(v);
    struct code_cache_stats before, after;
    code_cache_get_stats(&before);
    struct ia64_result b = tier0_test_instruction(v, &i);
    code_cache_get_stats(&after);
    if (a.kind == IA64_STEP_COMPLETE) {
        ++ref->retired;
        ref->s.slot = 1;
    }
    CHECK(a.kind == b.kind && a.fault == b.fault && a.exit == b.exit && a.info == b.info);
    CHECK(a.translation_fault == b.translation_fault && a.fault_isr == b.fault_isr);
    CHECK(!memcmp(&v->s, &ref->s, sizeof(v->s)));
    CHECK(v->retired == ref->retired);
    CHECK(!memcmp(ram, expected, sizeof(ram)));
    if (code)
        CHECK(after.invalidated_pages > before.invalidated_pages);
    code_cache_flush();
    vcpu_destroy(ref);
}
static void virtual_tests(void)
{
    CHECK(gm_add_region(
        &(struct gm_region){.base = PA, .size = sizeof(ram), .hva = ram, .kind = GM_RAM}));
    /* Cold and warm translations, reads and writes; key faults are rechecked. */
    for (unsigned store = 0; store < 2; ++store)
        for (unsigned key = 0; key < 3; ++key) {
            struct vcpu *v = fresh();
            map(v, 0x661);
            if (key)
                v->s.psr |= IA64_PSR_PK;
            if (key == 2)
                v->s.pkr[0] = 1 | (store ? 2 : 4);
            enum ia64_fault_kind f = key == 1   ? IA64_FAULT_KEY_MISS
                                     : key == 2 ? IA64_FAULT_KEY_PERMISSION
                                                : 0;
            compare(v, store, !key, f, false);
            compare(v, store, !key, f, false);
            vcpu_destroy(v);
        }
    /* Dirty before Access, including first write; fix via real itc and retry. */
    for (unsigned bits = 0; bits < 4; ++bits) {
        struct vcpu *v = fresh();
        map(v, 0x601 | bits << 5);
        enum ia64_fault_kind f = !(bits & 2)   ? IA64_FAULT_DIRTY_BIT
                                 : !(bits & 1) ? IA64_FAULT_ACCESS_BIT
                                               : 0;
        compare(v, true, !f, f, false);
        compare(v, false, (bits & 1) != 0, bits & 1 ? 0 : IA64_FAULT_ACCESS_BIT, false);
        map(v, 0x661);
        compare(v, true, true, 0, false);
        vcpu_destroy(v);
    }
    /* All Table 4-3 AR/PL/CPL combinations, both load and store. */
    for (unsigned ar = 0; ar < 8; ++ar)
        for (unsigned pl = 0; pl < 4; ++pl)
            for (unsigned cpl = 0; cpl < 4; ++cpl)
                for (unsigned store = 0; store < 2; ++store) {
                    struct vcpu *v = fresh();
                    uint64_t pte = 0x61 | pl << 7 | ar << 9;
                    map(v, pte);
                    v->s.psr |= (uint64_t)cpl << 32;
                    bool allowed = (ia64_tlb_permissions(pte, cpl) & (store ? 2 : 1)) != 0;
                    compare(v, store, allowed, allowed ? 0 : IA64_FAULT_ACCESS_RIGHTS, false);
                    vcpu_destroy(v);
                }
    for (unsigned store = 0; store < 2; ++store)
        for (unsigned source = 0; source < 2; ++source) {
            if (!store && source)
                continue;
            struct vcpu *v = fresh();
            map(v, 0x661);
            v->s.nat[source ? 9 : 8] = true;
            compare(v, store, false, IA64_FAULT_NAT, false);
            vcpu_destroy(v);
        }
    for (unsigned store = 0; store < 2; ++store) {
        struct vcpu *v = fresh();
        map(v, 0x661);
        ++v->s.gr[8];
        compare(v, store, false, IA64_FAULT_UNALIGNED, false);
        vcpu_destroy(v);
        v = fresh();
        map(v, 0x661 | IA64_MA_UC << 2);
        compare(v, store, false, 0, false);
        vcpu_destroy(v);
        v = fresh();
        map(v, 0x661 | IA64_MA_NAT << 2);
        compare(v, store, false, IA64_FAULT_DATA_NAT, false);
        vcpu_destroy(v);
    }
    struct vcpu *v = fresh();
    map(v, 0x661);
    compare(v, true, false, 0, true);
    /* Translator-result contract test, distinct from architectural cases. */
    Ia64Instruction i = {.opcode = IA64_OP_LD8};
    i.operands.memory.base = 8;
    static struct tier0_dtc_entry dtc[TIER0_DTC_SLOTS];
    struct tier0_execution e = {.dtc = dtc};
    inject_deferred = v;
    CHECK(tier0_data_pointer(v, &i, &e) == NULL);
    inject_deferred = NULL;
    vcpu_destroy(v);
    tier0_destroy();
}
REVIEW_SUITE("tier0-data-pointer", virtual_tests)
