/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/decode/decode.h"

/* gather.inc: slot 2 of the first loop bundle is nop.i 0. SDM Vol.3
 * nop (3:193), I18: changing its ignored immediate preserves every
 * architectural effect while changing an instruction's source bytes. */
#define NATIVE_MUTATION_BUNDLE (T1H_CODE + 0x60)
static struct {
    struct t1_vcpu *tv;
    uint64_t before, after, old_link, hops;
    bool stop, changed;
} native_mutation;

static bool native_mutation_holds(const struct t1_region *r)
{
    unsigned bit = (unsigned)(NATIVE_MUTATION_BUNDLE & 4095) / 16;
    for (unsigned p = 0; p < r->npages; ++p)
        if (r->page[p].pa == (NATIVE_MUTATION_BUNDLE & ~UINT64_C(4095)) &&
            (r->dep[p].mask[bit / 64] & (UINT64_C(1) << (bit % 64))))
            return true;
    return false;
}

static void *native_mutation_peer(void *unused)
{
    (void)unused;
    struct t1_vcpu *tv = native_mutation.tv;
    while (!__atomic_load_n(&native_mutation.stop, __ATOMIC_ACQUIRE)) {
        uint64_t hops = atomic_load_explicit(&tv->fast.completed, memory_order_acquire);
        if (hops < 32) {
            sched_yield();
            continue;
        }
        /* The lock protects the sampled record from reclamation; the
         * executor remains free to run and change its published region. */
        code_lock();
        const struct t1_region *r = atomic_load_explicit(&tv->running, memory_order_seq_cst);
        if (!r || !r->valid || !native_mutation_holds(r)) {
            code_unlock();
            sched_yield();
            continue;
        }
        native_mutation.old_link = r->link;
        uint64_t *word = gm_direct_ptr(NATIVE_MUTATION_BUNDLE + 8, 8);
        T1H_CHECK(word && __atomic_load_n(word, __ATOMIC_ACQUIRE) == native_mutation.before);
        /* Keep invalidation and the atomic write in the same source
         * preparation bracket. No guest wait occurs inside this bracket. */
        bool acquired = code_vmm_write_begin(NATIVE_MUTATION_BUNDLE + 8, 8);
        __atomic_store_n(word, native_mutation.after, __ATOMIC_RELEASE);
        code_cpu_write_end(acquired);
        T1H_CHECK(!r->valid && !t1_region_from_link(native_mutation.old_link));
        native_mutation.hops = atomic_load_explicit(&tv->fast.completed, memory_order_acquire);
        native_mutation.changed = true;
        code_unlock();
        return NULL;
    }
    return NULL;
}

static void native_mutation_test(void)
{
    struct guest_params q;
    const struct guest_program *p = open_gather(200, true, &q);
    run_reference(NULL);
    memset(&native_mutation, 0, sizeof(native_mutation));
    uint64_t low = image_word(NATIVE_MUTATION_BUNDLE);
    native_mutation.before = image_word(NATIVE_MUTATION_BUNDLE + 8);
    /* Slot 2 starts at bundle bit 87; I18 immediate bit 0 is slot bit 6. */
    native_mutation.after = native_mutation.before ^ (UINT64_C(1) << 29);
    const IA64TemplateInfo *tmpl = ia64_template_info(ia64_bundle_template_code(low));
    T1H_CHECK(tmpl->defined && tmpl->units[2] == IA64_UNIT_I);
    for (unsigned k = 0; k < 2; ++k) {
        uint64_t high = k ? native_mutation.after : native_mutation.before;
        Ia64Instruction insn = ia64_decode_insn(IA64_UNIT_I, ia64_bundle_slot(low, high, 2),
                                               NATIVE_MUTATION_BUNDLE, 2);
        T1H_CHECK(insn.valid && insn.opcode == IA64_OP_NOP && !insn.qp &&
                  insn.operands.system.immediate == k);
    }
    /* The only expected memory difference is the nop's ignored marker. */
    memcpy(M.reference.ram + NATIVE_MUTATION_BUNDLE + 8 - T1H_RAM_BASE,
           &native_mutation.after, 8);
    struct t1h_engine_vcpu ev;
    bool old_check = t1h_tier1_set_hostwin_check(false);
    engine_open(&ev);
    t1h_tier1_set_hostwin_check(old_check);
    t1_test_set_restore_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
    native_mutation.tv = t1h_tier1_vcpu(ev.ctx);
    t1_test_set_compile_options(native_mutation.tv, &(struct t1_compile_options){0});
    code_register_vcpu(M.v);
    pthread_t peer;
    T1H_CHECK(!pthread_create(&peer, NULL, native_mutation_peer, NULL));
    struct t1_stats before = stats_now();
    struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
    struct t1_stats after = stats_now();
    __atomic_store_n(&native_mutation.stop, true, __ATOMIC_RELEASE);
    T1H_CHECK(!pthread_join(peer, NULL));
    T1H_CHECK(native_mutation.changed && native_mutation.old_link && native_mutation.hops >= 32);
    T1H_CHECK(image_word(NATIVE_MUTATION_BUNDLE) == low &&
              image_word(NATIVE_MUTATION_BUNDLE + 8) == native_mutation.after);
    T1H_CHECK(DELTA(before, after, chain_native_links) > native_mutation.hops);
    /* Retirement must be followed by an entered replacement translation
     * whose source dependency covers the changed instruction. */
    bool replaced = false;
    code_lock();
    T1H_CHECK(!t1_region_from_link(native_mutation.old_link));
    for (uint32_t id = 1; id <= t1_cfg.max_regions; ++id) {
        const struct t1_region *r = t1_region_get(id);
        if (r && r->valid && r->link != native_mutation.old_link && r->entries &&
            native_mutation_holds(r))
            replaced = true;
    }
    code_unlock();
    T1H_CHECK(replaced);
    code_unregister_vcpu(M.v);
    char why[256];
    T1H_CHECK(guest_check(p, &q, M.v, &e, &M.logs, why, sizeof(why)));
    compare_or_fail(&e, "native edge: concurrent instruction-byte write");
    engine_close(&ev);
    close_vcpus();
    printf("chain: native code mutation: real nop immediate write, active translation retired, replacement entered, reference results\n");
}
