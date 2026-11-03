/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/interrupt.h"

/* SDM Vol.2 §§4.1, 4.4: instruction fetch uses the region ID, translation
 * permissions and protection key. A native edge must obey a changed
 * translation before any instruction of its target executes. */
static uint64_t native_virtual_pte(uint64_t pa)
{
    return pa | 1 | (UINT64_C(1) << 5) | (UINT64_C(1) << 6) | (UINT64_C(1) << 9);
}

static void native_virtual_map(struct vcpu *v, unsigned index, uint64_t va, uint64_t pa,
                               unsigned shift)
{
    v->s.tr[0][index] = (struct ia64_tlb_entry){.va = va & ((UINT64_C(1) << 61) - 1),
                                                .pte = native_virtual_pte(pa),
                                                .itir = (shift << 2) | (index ? 0x100u : 0),
                                                .rid = (v->s.rr[va >> 61] >> 8) & 0xffffff,
                                                .page_shift = shift,
                                                .valid = true};
    ia64_mmu_changed(v, IA64_MMU_TR, 0, index);
}

static void native_virtual_code(const uint64_t head[2], const uint64_t pa[2][2], unsigned step,
                                unsigned first)
{
    for (unsigned k = first; k < 2; ++k) {
        const struct ienc_bundle code[] = {
            {IENC_STOP(IENC_MII),
             {ienc_nop_m(), ienc_adds(0, 32 + k, k ? step : 1, 32 + k),
              ienc_adds(0, 14 + k, 1, 14 + k)}},
            {IENC_STOP(IENC_MIB),
             {ienc_nop_m(), ienc_nop_i(),
              ienc_br_cond(6, (int64_t)(head[1 - k] - (head[k] + 16)))}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -32)}}};
        for (unsigned b = 0; b < sizeof(code) / sizeof(code[0]); ++b) {
            uint64_t ip = head[k] + 16 * b;
            unsigned page = (ip & ~UINT64_C(4095)) != (head[k] & ~UINT64_C(4095));
            um_emit(pa[k][page] + (ip & 4095), &code[b], 1);
        }
    }
}

static void native_virtual_case(unsigned layout)
{
    unsigned pages = layout ? 2 : 1;
    const uint64_t head[2] = {
        layout ? (UINT64_C(2) << 61) - 16 : (UINT64_C(1) << 61) + 0x2000000,
        layout ? (UINT64_C(2) << 61) + 0x7ff0 : (UINT64_C(1) << 61) + 0x2008000};
    uint64_t pa[2][2] = {{UM_CODE + 0x60000, UM_CODE + 0x72000},
                         {UM_CODE + 0x68000, UM_CODE + 0x7a000}};
    if (layout == 2)
        pa[1][0] = pa[0][1]; /* Different VA pages share disjoint physical bundles. */
    if (layout == 3)
        pa[0][1] = pa[0][0]; /* Two dependencies of one region share a physical page. */
    unsigned target_page = pages == 1 ? 2 : 4;
    native_virtual_code(head, pa, 1, 0);
    struct vcpu *v = um_vcpu(head[0]), *ref = um_vcpu(head[0]);
    v->cfg.itc_icount = false;
    v->s.cr[CR_PTA] = 15u << 2;
    v->s.rr[1] = (UINT64_C(0x101) << 8) | (12u << 2);
    ia64_mmu_changed(v, IA64_MMU_RR, 0, 1);
    native_virtual_map(v, 0, UM_IVT, UM_IVT, 15);
    v->s.rr[2] = (UINT64_C(0x202) << 8) | (12u << 2);
    ia64_mmu_changed(v, IA64_MMU_RR, 0, 2);
    for (unsigned k = 0; k < 2; ++k)
        for (unsigned p = 0; p < pages; ++p)
            native_virtual_map(v, 1 + k * pages + p,
                (head[k] & ~UINT64_C(4095)) + 4096 * p, pa[k][p], 12);
    v->s.pkr[0] = 1;
    v->s.pkr[1] = 0x101;
    ia64_mmu_changed(v, IA64_MMU_PKR, 0, 0);
    ia64_mmu_changed(v, IA64_MMU_PKR, 0, 1);
    v->s.psr |= IA64_PSR_IT | IA64_PSR_PK;
    um_pr(v, 6, true);
    struct hostwin_config cfg = {40, 1, false};
    struct hostwin_vcpu *hw = hostwin_create(&cfg, hostwin_linux_backend(), hostwin_linux_ctx());
    CHECK(hw != NULL);
    struct t1_vcpu *tv = t1_vcpu_create(v, hw);
    CHECK(tv != NULL);
    v->upper = tv;
    t1_test_set_compile_options(tv, &(struct t1_compile_options){0});
    t1_test_set_restore_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
    uint64_t link[2];
    uint32_t point[2] = {UINT32_MAX, UINT32_MAX};
    for (unsigned k = 0; k < 2; ++k) {
        v->s.ip = head[k];
        struct t1_compile_report report;
        CHECK(t1_compile(tv, &(struct t1_compile_options){0}, &report) == T1_COMPILED);
        const struct t1_region *r = t1_region_get(report.region_id);
        CHECK(r && r->npages == pages && r->fast_points && !r->nhelper);
        link[k] = r->link;
        for (unsigned p = 0; p < r->npoints; ++p)
            if (r->point[p].kind == T1_PT_EXIT && r->point[p].ip == head[1 - k])
                point[k] = p;
        CHECK(point[k] != UINT32_MAX);
    }
    for (unsigned k = 0; k < 2; ++k)
        tv->successors[t1_successor_index(link[k], point[k])] =
            (struct t1_successor){link[k], link[1 - k], point[k]};
    v->s.ip = head[0];
    const uint64_t budget = 1200;
    code_register_vcpu(v);
    struct vcpu_exit out = native_pages_run(v, head, link, budget);
    if (out.reason != VCPU_EXIT_BUDGET || tv->fast.completed <= 32)
        fprintf(stderr, "virtual layout %u: exit %u ip %#llx hops %llu\n", layout,
                out.reason, (unsigned long long)v->s.ip, (unsigned long long)tv->fast.completed);
    CHECK(out.reason == VCPU_EXIT_BUDGET && tv->fast.completed > 32);
    CHECK(v->s.ip == head[0] && !v->s.slot && v->s.gr[14] == 100 && v->s.gr[15] == 100);
    CHECK(ia64_gr_read(&v->s, 32) == 100 && ia64_gr_read(&v->s, 33) == 100);
    for (unsigned k = 0; k < tv->fast.nnodes; ++k) {
        const struct t1_fast_node *n = &tv->fast.node[k];
        unsigned target = n->region->c.ip == head[1];
        CHECK(n->region->c.ip == head[target]);
        CHECK(n->witness.fetch.rr == v->s.rr[head[target] >> 61] &&
              n->witness.fetch.pte == native_virtual_pte(pa[target][0]));
        CHECK((n->second != NULL) == (pages == 2));
        if (pages == 2) {
            CHECK(n->second->page == n->region->dep[1].page &&
                  !memcmp(&n->witness.fetch, &n->second->fetch, sizeof(n->witness.fetch)));
            struct code_stay_witness_group group = {
                .n = 2, .page = {n->witness, *n->second}};
            CHECK(code_stay_witness_group_reenter(v, &group, &n->witness.fetch));
            CHECK((n->witness.page == n->second->page) == (layout == 3 && !target));
        }
    }

    /* Vol.1 §3.1.7: logical stacked names follow BOF, not physical homes
     * cached by a previous entry. Every BOF runs the same compiled pair. */
    for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof) {
        v->s.rse_bof = bof;
        um_gr(v, 32, 700 + bof, false);
        um_gr(v, 33, 900 + bof, false);
        uint64_t before = tv->fast.completed;
        out = native_pages_run(v, head, link, budget);
        CHECK(out.reason == VCPU_EXIT_BUDGET && tv->fast.completed > before + 32);
        CHECK(ia64_gr_read(&v->s, 32) == 800 + bof && ia64_gr_read(&v->s, 33) == 1000 + bof);
    }
    code_unregister_vcpu(v);

    /* Vol.2 §3.3.4.2: a future ITM must leave the interrupt pending state
     * unchanged while virtual native entries continue under the host clock. */
    v->s.cr[CR_ITV] = 0xef;
    v->s.cr[CR_ITM] = ia64_itc_read(v) + v->cfg.itc_hz * 3600;
    ia64_timer_program(v);
    ia64_timer_poll(v);
    CHECK(v->timer_armed && !v->timer_fired && v->timer_cache.valid && !v->clock_hook);
    struct um_snapshot timer_snap;
    um_snapshot_take(&timer_snap, v);
    uint64_t timer_before = tv->fast.completed;
    code_register_vcpu(v);
    out = native_pages_run(v, head, link, budget);
    code_unregister_vcpu(v);
    CHECK(out.reason == VCPU_EXIT_BUDGET && tv->fast.completed > timer_before + 32);
    CHECK((v->s.psr & IA64_PSR_IT) && v->timer_armed && !v->timer_fired);
    CHECK(v->s.ip == head[0] && !v->s.slot && v->retired == timer_snap.v.retired + budget);
    struct um_run timer_observed = {0};
    CHECK(!um_compare(&timer_snap, ref, &timer_observed, v, &out, budget,
                      "virtual native armed host timer"));
    um_snapshot_free(&timer_snap);
    v->timer_armed = false;
    v->timer_cache.valid = false;

    /* Each changed context starts after native execution has warmed both
     * fetch proofs and witnesses. Reference runs use the same change. */
    static struct vcpu base;
    base = *v;
    for (unsigned test = 0; test < 12; ++test) {
        *v = base;
        t1_vcpu_state_restored(tv);
        code_register_vcpu(v);
        uint64_t warmed = tv->fast.completed;
        out = native_pages_run(v, head, link, budget);
        if (out.reason != VCPU_EXIT_BUDGET || tv->fast.completed <= warmed + 32)
            fprintf(stderr,
                    "virtual warm case %u: exit %u ip %#llx links %llu/%llu hops %llu nodes %u\n",
                    test, out.reason, (unsigned long long)v->s.ip, (unsigned long long)link[0],
                    (unsigned long long)link[1], (unsigned long long)(tv->fast.completed - warmed),
                    tv->fast.nnodes);
        CHECK(out.reason == VCPU_EXIT_BUDGET && tv->fast.completed > warmed + 32);
        uint64_t before = tv->fast.completed;
        switch (test) {
        case 0: /* PTA changes the validation fetch context without an MMU-log entry. */
            v->s.cr[CR_PTA] ^= UINT64_C(1) << 16;
            break;
        case 1: /* The same VA now names a distinct physical page. */
            native_virtual_map(v, target_page,
                (head[1] & ~UINT64_C(4095)) + (pages - 1) * 4096, UM_IVT, 12);
            break;
        case 2: /* AR=0 is read-only, without execute permission. */
            v->s.tr[0][target_page].pte &= ~(UINT64_C(7) << 9);
            ia64_mmu_changed(v, IA64_MMU_TR, 0, target_page);
            break;
        case 3: /* A new region ID cannot reuse the old translation. */
            v->s.rr[head[1] >> 61] ^= UINT64_C(1) << 8;
            ia64_mmu_changed(v, IA64_MMU_RR, 0, head[1] >> 61);
            break;
        case 4: /* PL=0 translation does not grant CPL=3 execution. */
            v->s.psr |= UINT64_C(3) << 32;
            break;
        case 5: /* Removing the matching protection key faults. */
            v->s.pkr[1] = 0;
            ia64_mmu_changed(v, IA64_MMU_PKR, 0, 1);
            break;
        case 6: /* State restoration clears proofs even when sequence values repeat. */
            t1_vcpu_state_restored(tv);
            CHECK(!tv->fast.nnodes);
            break;
        case 7: /* A missing instruction translation must reach the lower fetch. */
            v->s.tr[0][target_page].valid = false;
            ia64_mmu_changed(v, IA64_MMU_TR, 0, target_page);
            break;
        case 8: /* An absent cached proof is not permission to prefetch a target. */
            tv->fetch_ok[t1_region_from_link(link[1])->id % T1_FETCH_CACHE].link = 0;
            tv->fast.nnodes = 0;
            break;
        case 9: /* The matching PKR disables execute access. */
            v->s.pkr[1] |= 8;
            ia64_mmu_changed(v, IA64_MMU_PKR, 0, 1);
            break;
        case 10: /* A clear accessed bit must reach the architectural fetch fault. */
            v->s.tr[0][target_page].pte &= ~(UINT64_C(1) << 5);
            ia64_mmu_changed(v, IA64_MMU_TR, 0, target_page);
            break;
        case 11: /* Source replacement changes the target's architectural increment. */
            native_virtual_code(head, pa, 2, 1);
            CHECK(!t1_region_from_link(link[1]));
            break;
        }
        /* Planning a future target is a read-only operation on the guest,
         * including a missing translation or denied execute permission. */
        static struct vcpu unchanged;
        unchanged = *v;
        t1_fast_prepare(tv, t1_region_from_link(link[0]));
        CHECK(!memcmp(v, &unchanged, sizeof(*v)));
        CHECK(!tv->x.fast);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        out = native_pages_run(v, head, link, budget);
        if (test == 0 || test == 6 || test == 8)
            CHECK(tv->fast.completed > before + 32);
        else
            CHECK(tv->fast.completed == before);
        if (test == 0)
            for (unsigned k = 0; k < tv->fast.nnodes; ++k)
                CHECK(tv->fast.node[k].witness.fetch.pta == v->s.cr[CR_PTA]);
        code_unregister_vcpu(v);
        struct um_run observed = {0};
        CHECK(!um_compare(&snap, ref, &observed, v, &out, budget, "virtual native context"));
        um_snapshot_free(&snap);
    }
    v->upper = NULL;
    t1_vcpu_destroy(tv);
    hostwin_destroy(hw);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

void native_virtual_test(void);
void native_virtual_test(void)
{
    umt_init();
    for (unsigned layout = 0; layout < 4; ++layout)
        native_virtual_case(layout);
    puts("unit_runtime: virtual native entries: one/two pages, distinct RR/PA, shared-page aliases, every BOF, armed host timer, fetch faults, restore and source changes");
}
