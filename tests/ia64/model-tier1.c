/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "engine/tier1/tier1.h"
#include "ia64/state.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/interrupts.h"
#define ENTRY UINT64_C(0x100000)
static unsigned formed;
static void formed_prefix(const struct vcpu *v, enum t1_compile_status status,
                          const struct t1_compile_report *r, void *arg)
{
    (void)v;
    (void)arg;
    fprintf(stderr, "model prefix: status=%u insns=%u bytes=%zu reject=%s end=%s\n", status,
            r->insns, r->code_bytes, r->reject_reason ? r->reject_reason : "-",
            r->end_reason ? r->end_reason : "-");
    assert(status == T1_COMPILED && r->insns == 3 && r->code_bytes);
    assert(r->end_ip == ENTRY + 128 && r->end_slot == 0);
    /* Removing form.c's model check changes this to "not in the v1
     * subset": the test must distinguish a profile boundary from coverage. */
    assert(r->end_reason && !strcmp(r->end_reason, "not in the CPU profile"));
    ++formed;
}
void test_model_tier1(struct vcpu *v)
{
    v->s.ip = ENTRY + 112;
    v->s.slot = 0;
    v->s.gr[8] = 123;
    v->s.gr[10] = ENTRY + 0x8000;
    assert(engine_vcpu_attach(v));
    t1_test_set_compile_observer(formed_prefix, NULL);
    assert(t1_upper_hot(v, v->s.ip, false) == T1_HOT_LINKED && formed == 1);
    t1_test_set_compile_observer(NULL, NULL);
    uint64_t link = t1_upper_probe(v, v->s.ip);
    struct vcpu_exit out;
    assert(link);
    /* Existing native entries must decline once the PMU is enabled. The
     * standalone runner must count through its interpreter lower tier. */
    struct ia64_state saved = v->s;
    v->s.pmc[4] =
        (v->cfg.cpu_model == IA64_CPU_MADISON ? UINT64_C(1) << 23 : UINT64_C(2) << 24) | 0x801;
    v->s.psr |= UINT64_C(1) << 2;
    assert(t1_upper_enter(v, &link, 32, &out) == T1_UPPER_DECLINED && !v->retired);
    assert(t1_run(v->upper, 1).reason == VCPU_EXIT_BUDGET);
    assert(v->retired == 1 && v->s.pmd[4] == 1);
    v->s = saved;
    v->retired = 0;
    link = t1_upper_probe(v, v->s.ip);
    assert(link && t1_upper_enter(v, &link, 32, &out) == T1_UPPER_RAN);
    assert(v->retired == 3 && v->s.gr[8] == 124 && v->s.ip == ENTRY + 128 && !v->s.slot);
    engine_run(v, 1);
    assert(v->fault.kind == IA64_FAULT_ILLEGAL && v->fault.ip == ENTRY + 128 && v->retired == 3);
    assert(v->s.gr[8] == 124 && v->s.gr[10] == ENTRY + 0x8000);
    engine_vcpu_detach(v);
}

/* An INIT that reaches the vCPU between slices while
 * a Tier1 loop is hot enters PALE_INIT at the next boundary; Tier1 leaves
 * the delivery to the lower tier, so no further iteration runs. The loop
 * is `adds r8 = 1, r8` and a branch to itself (MIB); PALE_INIT holds
 * break.m 0x1ffffe. */
static bool pal_stop(struct vcpu *v, uint64_t imm, uint64_t ip)
{
    (void)ip;
    if (imm != 0x1ffffe)
        return false;
    vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    return true;
}
static void bundle(uint64_t gpa, unsigned template_code, uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t words[2] = {template_code | (a << 5) | (b << 46), (b >> 18) | (c << 23)};
    assert(gm_write(gpa, words, sizeof(words)));
}
void test_pal_event_tier1(struct vcpu *v)
{
    const uint64_t loop = ENTRY + 0x9000, pale = ENTRY + 0x9100, nop = UINT64_C(1) << 27;
    const uint64_t adds = (UINT64_C(8) << 37) | (UINT64_C(2) << 34) | (UINT64_C(8) << 20) |
                          (UINT64_C(1) << 13) | (UINT64_C(8) << 6);
    bundle(loop, 0x11, adds, nop, UINT64_C(4) << 37);
    bundle(pale, 0, (UINT64_C(1) << 36) | (UINT64_C(0xffffe) << 6), nop, nop);
    v->cfg.pale_init = pale;
    v->s.ip = loop;
    v->s.slot = 0;
    v->s.psr = IA64_PSR_IC | IA64_PSR_BN;
    v->s.gr[8] = 0;
    assert(engine_vcpu_attach(v));
    engine_set_break_hook(pal_stop);
    struct t1_stats before, after;
    t1_get_stats(&before);
    struct vcpu_exit out = engine_run(v, 20000);
    t1_get_stats(&after);
    assert(out.reason == VCPU_EXIT_BUDGET && v->s.ip == loop && v->s.gr[8] > 1000);
    assert(after.tier1_retired > before.tier1_retired);
    uint64_t count = v->s.gr[8];
    assert(vcpu_accept_pal_interrupt(v, IA64_IRQ_INIT, 0));
    out = engine_run(v, 20000);
    assert(out.reason == VCPU_EXIT_DEBUG && out.ip == pale && v->s.gr[8] == count);
    assert(v->s.cr[CR_IIP] == loop && (v->s.psr & IA64_PSR_MC));
    engine_set_break_hook(NULL);
    engine_vcpu_detach(v);
}
