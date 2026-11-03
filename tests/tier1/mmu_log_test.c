/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * build/tier1-mmu-log-test: MMU change-log writers, replay and completeness.
 * Execute real MMU semantics and check exact changed TR/TC/RR/PKR elements
 * (IA64_MMU_ALL covers all). Muting each writer must expose an unlogged change.
 * Compare hostwin log replay against full resync, including overflow, rights
 * changes, identical reinsertion and re-entry after a non-executing interlude.
 * Check mode must panic on unlogged writes of every element kind and on an
 * mmu_seq restore without hostwin_resync; muted writers must likewise panic.
 * Also checks statistics and the fast path.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "tests/tier1/hostwin_linux.h"
#include "ia64/mmu.h"
#include "ia64/mmu_log.h"
#include "ia64/tlb.h"
#include "ia64/smp.h"
#include "ia64/semantics.h"
#include "ia64/decode/decode.h"
#include "ia64vmm/guest_mem.h"
#include "core/log_internal.h"

#define PAGE 4096u
#define RAM_BYTES (UINT64_C(64) << 20)
#define R1 (UINT64_C(1) << 61)
#define R2 (UINT64_C(2) << 61)

static unsigned checks, tests;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "mmu_log_test: %s:%d: check failed: %s\n", __FILE__, __LINE__, #c);    \
            exit(2);                                                                               \
        }                                                                                          \
    } while (0)

static uint8_t *ram;
static struct vcpu *v;

/* ---- instructions through the real semantics --------------------------- */

/* M-unit system format (SDM Vol.3 §4.4): major opcode 1, x3 = 0, x6 at
 * bits 27..32, r3 at 20..26, r2 at 13..19, r1 at 6..12, qp 0. The decoder
 * is the oracle: run() checks the opcode it decodes. */
static uint64_t m_sys(unsigned x6, unsigned r3, unsigned r2)
{
    return UINT64_C(1) << 37 | (uint64_t)x6 << 27 | (uint64_t)r3 << 20 | (uint64_t)r2 << 13;
}
static void run(uint64_t raw, Ia64Opcode opcode)
{
    Ia64Instruction insn = ia64_decode_insn(IA64_UNIT_M, raw, v->s.ip, 0);
    CHECK(insn.valid && insn.opcode == opcode);
    struct ia64_result r = ia64_execute(v, &insn);
    CHECK(r.kind == IA64_STEP_COMPLETE);
}
static void gr(unsigned reg, uint64_t value)
{
    ia64_gr_write(&v->s, reg, value, false);
}
static uint64_t pte(gpa_t pa, unsigned ar, bool a, bool d)
{
    return 1 | (uint64_t)IA64_MA_WB << 2 | (uint64_t)a << 5 | (uint64_t)d << 6 | (uint64_t)ar << 9 |
           (pa & ((UINT64_C(1) << 50) - PAGE));
}
static uint64_t rw(gpa_t pa)
{
    return pte(pa, 2, true, true);
}
/* SDM Vol.3 itc, itr: an insertion with PSR.ic = 1 is an Illegal Operation
 * fault (ia64/ since f6c9f55); a guest inserts with interruption
 * collection off (rsm psr.ic ... ssm psr.ic), so the insertions below do. */
static void run_ic_off(uint64_t raw, Ia64Opcode opcode)
{
    uint64_t psr = v->s.psr;
    v->s.psr &= ~IA64_PSR_IC;
    run(raw, opcode);
    v->s.psr = psr;
}
/* itc.i/itc.d r3 with ifa = va, itir = ps, key. */
static void op_itc(unsigned side, uint64_t va, uint64_t p, unsigned ps, uint32_t key)
{
    v->s.cr[CR_IFA] = va;
    v->s.cr[CR_ITIR] = (uint64_t)ps << 2 | (uint64_t)key << 8;
    gr(3, p);
    run_ic_off(m_sys(side ? 0x2e : 0x2f, 0, 3), side ? IA64_OP_ITC_D : IA64_OP_ITC_I);
}
/* itr.i/itr.d tr[r2] = r3. */
static void op_itr(unsigned side, unsigned slot, uint64_t va, uint64_t p, unsigned ps, uint32_t key)
{
    v->s.cr[CR_IFA] = va;
    v->s.cr[CR_ITIR] = (uint64_t)ps << 2 | (uint64_t)key << 8;
    gr(2, slot);
    gr(3, p);
    run_ic_off(m_sys(side ? 0x0e : 0x0f, 2, 3), side ? IA64_OP_ITR_D : IA64_OP_ITR_I);
}
/* ptc.l/ptc.g/ptc.ga/ptr.d/ptr.i r2, r3 (address, ps << 2). */
static void op_purge(Ia64Opcode op, uint64_t va, unsigned ps)
{
    unsigned x6 = op == IA64_OP_PTC_L    ? 0x09
                  : op == IA64_OP_PTC_G  ? 0x0a
                  : op == IA64_OP_PTC_GA ? 0x0b
                  : op == IA64_OP_PTR_D  ? 0x0c
                                         : 0x0d;
    gr(2, va);
    gr(3, (uint64_t)ps << 2);
    run(m_sys(x6, 2, 3), op);
}
static void op_ptc_e(uint64_t va)
{
    gr(2, va);
    run(m_sys(0x34, 2, 0), IA64_OP_PTC_E);
}
/* mov rr[r2] = r3; mov pkr[r2] = r3. */
static void op_rr(unsigned vrn, uint64_t value)
{
    gr(2, (uint64_t)vrn << 61);
    gr(3, value);
    run(m_sys(0x00, 2, 3), IA64_OP_MOV_GRRR);
}
static void op_pkr(unsigned index, uint64_t value)
{
    gr(2, index);
    gr(3, value);
    run(m_sys(0x03, 2, 3), IA64_OP_MOV_GRPKR_INDEXED);
}
static uint64_t rr_value(unsigned rid, unsigned ps, bool ve)
{
    return (uint64_t)rid << 8 | ps << 2 | ve;
}

/* A fresh architectural state: virtual data, RIDs 0x100 + n with 4 KiB
 * pages, no translation; mmu_seq keeps counting (vcpu_reset). */
static void fresh_state(void)
{
    ia64_mmu_test_mute = 0;
    vcpu_reset(
        v, &(struct ia64_entry_regs){.ip = 0x100000, .psr = IA64_PSR_IC | IA64_PSR_DT, .rsc = 3});
    for (unsigned r = 0; r < 8; ++r)
        op_rr(r, rr_value(0x100 + r, 12, false));
}
/* Page k of region 1 (64 KiB apart, so a 64 KiB TR covers exactly one). */
static uint64_t page_va(unsigned k)
{
    return R1 | UINT64_C(0x10000000) | (uint64_t)k << 16;
}
static gpa_t page_pa(unsigned k)
{
    return 0x1000000 + (gpa_t)k * PAGE;
}
static unsigned slot_of(const struct ia64_tlb_entry *e, unsigned side)
{
    for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i)
        if (e == &v->s.tc[side][i])
            return i;
    for (unsigned i = 0; i < IA64_DTR_COUNT; ++i)
        if (e == &v->s.tr[side][i])
            return i;
    CHECK(!"entry outside the vCPU's TLB");
    return 0;
}

/* ---- A. exact log contents ---------------------------------------------- */

struct image {
    uint64_t rr[8], pkr[16];
    struct ia64_tlb_entry tr[2][IA64_DTR_COUNT], tc[2][IA64_TC_ENTRIES];
};
static void take_image(struct image *m)
{
    memcpy(m->rr, v->s.rr, sizeof(m->rr));
    memcpy(m->pkr, v->s.pkr, sizeof(m->pkr));
    memcpy(m->tr, v->s.tr, sizeof(m->tr));
    memcpy(m->tc, v->s.tc, sizeof(m->tc));
}
static bool same_entry(const struct ia64_tlb_entry *a, const struct ia64_tlb_entry *b)
{
    return a->va == b->va && a->pte == b->pte && a->itir == b->itir && a->rid == b->rid &&
           a->page_shift == b->page_shift && a->valid == b->valid;
}
static bool element_changed(const struct image *m, unsigned kind, unsigned side, unsigned i)
{
    switch (kind) {
    case IA64_MMU_RR:
        return m->rr[i] != v->s.rr[i];
    case IA64_MMU_PKR:
        return m->pkr[i] != v->s.pkr[i];
    case IA64_MMU_TR:
        return !same_entry(&m->tr[side][i], &v->s.tr[side][i]);
    default:
        return !same_entry(&m->tc[side][i], &v->s.tc[side][i]);
    }
}
static bool logged_since(uint64_t seq0, unsigned kind, unsigned side, unsigned i)
{
    for (uint64_t q = seq0; q != v->mmu_seq; ++q) {
        const struct ia64_mmu_change *c = &v->mmu_log[q % IA64_MMU_LOG];
        if (c->kind == IA64_MMU_ALL)
            return true;
        if (c->kind == kind && c->index == i &&
            (c->side == side || kind == IA64_MMU_RR || kind == IA64_MMU_PKR))
            return true;
    }
    return false;
}
struct tally {
    unsigned changed, missing, logged, stale;
};
/* Changed elements, those without a log entry since seq0, the log entries,
 * and log entries naming an element that did not change. */
static struct tally tally_since(const struct image *m, uint64_t seq0)
{
    CHECK(v->mmu_seq - seq0 <= IA64_MMU_LOG);
    struct tally t = {0};
    static const unsigned counts[4] = {IA64_TC_ENTRIES, IA64_DTR_COUNT, 8, 16};
    for (unsigned kind = IA64_MMU_TC; kind <= IA64_MMU_PKR; ++kind)
        for (unsigned side = 0; side < (kind <= IA64_MMU_TR ? 2u : 1u); ++side)
            for (unsigned i = 0; i < counts[kind]; ++i)
                if (element_changed(m, kind, side, i)) {
                    ++t.changed;
                    if (!logged_since(seq0, kind, side, i))
                        ++t.missing;
                }
    for (uint64_t q = seq0; q != v->mmu_seq; ++q) {
        const struct ia64_mmu_change *c = &v->mmu_log[q % IA64_MMU_LOG];
        ++t.logged;
        CHECK(c->kind <= IA64_MMU_ALL && c->side <= 1 &&
              (c->kind == IA64_MMU_ALL || c->index < counts[c->kind]));
        CHECK((c->kind == IA64_MMU_TC || c->kind == IA64_MMU_TR) || c->side == 0);
        if (c->kind != IA64_MMU_ALL && !element_changed(m, c->kind, c->side, c->index))
            ++t.stale;
    }
    return t;
}

/* The writer scenarios, shared by A and D. prepare() runs unlogged-safe
 * (every writer on); act() runs the family under test and returns the
 * element the completeness check must name when the family is muted. */
struct scenario {
    enum ia64_mmu_writer family;
    const char *name;
    void (*prepare)(void);
    const char *(*act)(void);
    unsigned changes;      /* elements act() changes (0: IA64_MMU_ALL) */
    unsigned data_changes; /* changes affecting a resident data translation */
};
static char element[64];
static const char *name_tlb(unsigned kind, unsigned side, unsigned i)
{
    snprintf(element, sizeof(element), "%s-side %s %u", side ? "D" : "I",
             kind == IA64_MMU_TR ? "TR" : "TC", i);
    return element;
}

static void prep_none(void) {}
static void prep_d_page0(void)
{
    op_itc(1, page_va(0), rw(page_pa(0)), 12, 0);
}
static void prep_i_page0(void)
{
    op_itc(0, page_va(0), rw(page_pa(0)), 12, 0);
}
static void prep_tr5(void)
{
    op_itr(1, 5, page_va(1), rw(page_pa(1)), 12, 0);
}
static void prep_itr2(void)
{
    op_itr(0, 2, page_va(2), rw(page_pa(2)), 12, 0);
}
static void prep_pkr(void)
{
    op_pkr(1, 1 | (uint64_t)0x77 << 8);
}
/* The VHPT walker (SDM Vol.2 §4.1.5): region 2 with 64 KiB pages and
 * ve = 1, a short-format table at R2 | 0x40000000 mapped by DTR 0, holding
 * the PTE of WALK_VA's page. OLD_VA: a 4 KiB entry in the same 64 KiB page
 * that does not match WALK_VA; the walker's insertion purges it. */
#define WALK_VA (R2 | UINT64_C(0x12343000))
#define OLD_VA (R2 | UINT64_C(0x12340000))
static void prep_walker(void)
{
    op_rr(2, rr_value(0x222, 16, true));
    v->s.cr[CR_PTA] = (R2 | 0x40000000) | 15 << 2 | 1;
    uint64_t h = ia64_thash(&v->s, WALK_VA);
    op_itr(1, 0, h, rw(0xa00000 + (h & 0x7000)), 12, 0);
    uint64_t p = rw(0x910000);
    memcpy(ram + 0xa00000 + (h & 0x7fff), &p, 8);
}
static void prep_walker_old(void)
{
    prep_walker();
    op_itc(1, OLD_VA, rw(0x900000), 12, 0);
}

static const char *act_tc_insert(void)
{
    unsigned index = v->s.tc_next[1] % IA64_TC_ENTRIES;
    ia64_tc_insert(v, page_va(3), 1, rw(page_pa(3)), 12 << 2);
    return name_tlb(IA64_MMU_TC, 1, index);
}
static const char *act_tc_insert_purge(void)
{
    unsigned old = slot_of(ia64_tlb_find(&v->s, page_va(0), 1), 1);
    ia64_tc_insert(v, page_va(0), 1, rw(page_pa(7)), 12 << 2);
    return name_tlb(IA64_MMU_TC, 1, old);
}
static const char *act_walk(void)
{
    unsigned index = v->s.tc_next[1] % IA64_TC_ENTRIES;
    struct ia64_translation t;
    CHECK(!ia64_tlb_find(&v->s, WALK_VA, 1));
    CHECK(ia64_translate(v, WALK_VA, IA64_REF_DATA, IA64_ACCESS_READ, 0, false, &t).kind ==
          IA64_STEP_COMPLETE);
    CHECK(t.pa == 0x913000 && t.entry == &v->s.tc[1][index]);
    return name_tlb(IA64_MMU_TC, 1, index);
}
static const char *act_walk_purge(void)
{
    unsigned old = slot_of(ia64_tlb_find(&v->s, OLD_VA, 1), 1);
    act_walk();
    return name_tlb(IA64_MMU_TC, 1, old);
}
static const char *act_itc_d(void)
{
    unsigned index = v->s.tc_next[1] % IA64_TC_ENTRIES;
    op_itc(1, page_va(3), rw(page_pa(3)), 12, 0);
    return name_tlb(IA64_MMU_TC, 1, index);
}
static const char *act_itc_i(void)
{
    unsigned index = v->s.tc_next[0] % IA64_TC_ENTRIES;
    op_itc(0, page_va(3), rw(page_pa(3)), 12, 0);
    return name_tlb(IA64_MMU_TC, 0, index);
}
static const char *act_itc_overlap(void)
{
    unsigned old = slot_of(ia64_tlb_find(&v->s, page_va(0), 1), 1);
    op_itc(1, page_va(0), rw(page_pa(6)), 12, 0);
    return name_tlb(IA64_MMU_TC, 1, old);
}
static const char *act_itr_overlap(void)
{
    unsigned old = slot_of(ia64_tlb_find(&v->s, page_va(0), 1), 1);
    op_itr(1, 6, page_va(0), rw(page_pa(6)), 16, 0);
    return name_tlb(IA64_MMU_TC, 1, old);
}
static const char *act_itr_d(void)
{
    op_itr(1, 3, page_va(4), rw(page_pa(4)), 12, 0);
    return name_tlb(IA64_MMU_TR, 1, 3);
}
static const char *act_itr_i(void)
{
    op_itr(0, 2, page_va(4), rw(page_pa(4)), 12, 0);
    return name_tlb(IA64_MMU_TR, 0, 2);
}
static const char *purge_page0(Ia64Opcode op, unsigned side)
{
    unsigned old = slot_of(ia64_tlb_find(&v->s, page_va(0), side), side);
    op_purge(op, page_va(0), 12);
    return name_tlb(IA64_MMU_TC, side, old);
}
static const char *act_ptc_l(void)
{
    return purge_page0(IA64_OP_PTC_L, 1);
}
static const char *act_ptc_l_i(void)
{
    return purge_page0(IA64_OP_PTC_L, 0);
}
static const char *act_ptc_g(void)
{
    return purge_page0(IA64_OP_PTC_G, 1);
}
static const char *act_remote(void)
{
    unsigned old = slot_of(ia64_tlb_find(&v->s, page_va(0), 1), 1);
    ia64_tlb_remote_purge(v, (v->s.rr[page_va(0) >> 61] >> 8) & 0xffffff, page_va(0), 12, false);
    return name_tlb(IA64_MMU_TC, 1, old);
}
static const char *act_remote_all(void)
{
    unsigned old = slot_of(ia64_tlb_find(&v->s, page_va(0), 1), 1);
    ia64_tlb_remote_purge(v, 0, 0, 0, true);
    return name_tlb(IA64_MMU_TC, 1, old);
}
static const char *act_ptc_ga(void)
{
    return purge_page0(IA64_OP_PTC_GA, 1);
}
static const char *act_ptr_d_tc(void)
{
    return purge_page0(IA64_OP_PTR_D, 1);
}
static const char *act_ptr_i_tc(void)
{
    return purge_page0(IA64_OP_PTR_I, 0);
}
static const char *act_ptc_e(void)
{
    unsigned old = slot_of(ia64_tlb_find(&v->s, page_va(0), 1), 1);
    op_ptc_e(page_va(0));
    return name_tlb(IA64_MMU_TC, 1, old);
}
static const char *act_ptr_d_tr(void)
{
    op_purge(IA64_OP_PTR_D, page_va(1), 12);
    return name_tlb(IA64_MMU_TR, 1, 5);
}
static const char *act_ptr_i_tr(void)
{
    op_purge(IA64_OP_PTR_I, page_va(2), 12);
    return name_tlb(IA64_MMU_TR, 0, 2);
}
static const char *act_rr(void)
{
    op_rr(5, rr_value(0x555, 14, false));
    return "RR 5";
}
static const char *act_pkr(void)
{
    op_pkr(4, 1 | (uint64_t)0x44 << 8);
    return "PKR 4";
}
static const char *act_pkr_dup(void)
{
    op_pkr(6, 1 | (uint64_t)0x77 << 8);
    return "PKR 1";
}
static const char *act_reset(void)
{
    vcpu_reset(v, NULL);
    return "RR 0";
}

static const struct scenario scenarios[] = {
    {IA64_MMU_W_TC_INSERT, "ia64_tc_insert", prep_none, act_tc_insert, 1, 1},
    {IA64_MMU_W_TC_INSERT_PURGE, "ia64_tc_insert overlap purge", prep_d_page0, act_tc_insert_purge,
     2, 2},
    {IA64_MMU_W_TC_INSERT, "VHPT walker insertion", prep_walker, act_walk, 1, 1},
    {IA64_MMU_W_TC_INSERT_PURGE, "VHPT walker overlap purge", prep_walker_old, act_walk_purge, 2,
     2},
    {IA64_MMU_W_ITC, "itc.d", prep_none, act_itc_d, 1, 1},
    {IA64_MMU_W_ITC, "itc.i", prep_none, act_itc_i, 1, 0},
    {IA64_MMU_W_INSERT_PURGE, "itc.d overlap purge", prep_d_page0, act_itc_overlap, 2, 2},
    {IA64_MMU_W_INSERT_PURGE, "itr.d overlap purge", prep_d_page0, act_itr_overlap, 2, 2},
    {IA64_MMU_W_ITR, "itr.d", prep_none, act_itr_d, 1, 1},
    {IA64_MMU_W_ITR, "itr.i", prep_none, act_itr_i, 1, 0},
    {IA64_MMU_W_PTC_L, "ptc.l (D)", prep_d_page0, act_ptc_l, 1, 1},
    {IA64_MMU_W_PTC_L, "ptc.l (I)", prep_i_page0, act_ptc_l_i, 1, 0},
    {IA64_MMU_W_PTC_G, "ptc.g", prep_d_page0, act_ptc_g, 1, 1},
    {IA64_MMU_W_PTC_GA, "ptc.ga", prep_d_page0, act_ptc_ga, 1, 1},
    {IA64_MMU_W_PTC_G_REMOTE, "remote ptc.g", prep_d_page0, act_remote, 1, 1},
    {IA64_MMU_W_PTC_G_REMOTE, "remote overrun", prep_d_page0, act_remote_all, 1, 1},
    {IA64_MMU_W_PTC_E, "ptc.e", prep_d_page0, act_ptc_e, 1, 1},
    {IA64_MMU_W_PTR_TC, "ptr.d (TC)", prep_d_page0, act_ptr_d_tc, 1, 1},
    {IA64_MMU_W_PTR_TC, "ptr.i (TC)", prep_i_page0, act_ptr_i_tc, 1, 0},
    {IA64_MMU_W_PTR_TR, "ptr.d (TR)", prep_tr5, act_ptr_d_tr, 1, 1},
    {IA64_MMU_W_PTR_TR, "ptr.i (TR)", prep_itr2, act_ptr_i_tr, 1, 0},
    {IA64_MMU_W_RR, "mov rr", prep_none, act_rr, 1, 1},
    {IA64_MMU_W_PKR, "mov pkr", prep_none, act_pkr, 1, 1},
    {IA64_MMU_W_PKR_DUP, "mov pkr duplicate key", prep_pkr, act_pkr_dup, 2, 2},
    {IA64_MMU_W_RESET, "vcpu_reset", prep_d_page0, act_reset, 0, 1},
};
#define NSCENARIOS (sizeof(scenarios) / sizeof(scenarios[0]))

static void test_writers(void)
{
    bool family_seen[IA64_MMU_WRITERS] = {false};
    for (unsigned i = 0; i < NSCENARIOS; ++i) {
        const struct scenario *sc = &scenarios[i];
        family_seen[sc->family] = true;
        for (int muted = 0; muted < 2; ++muted) {
            fresh_state();
            sc->prepare();
            struct image m;
            take_image(&m);
            uint64_t seq0 = v->mmu_seq, data0 = v->mmu_dseq;
            ia64_mmu_test_mute = muted ? UINT32_C(1) << sc->family : 0;
            sc->act();
            ia64_mmu_test_mute = 0;
            struct tally t = tally_since(&m, seq0);
            if (!muted) {
                /* Exactly the changed elements, nothing stale; ALL alone
                 * for a reset (mmu_seq moves by one, never backwards). */
                if (sc->changes)
                    CHECK(t.changed == sc->changes && t.missing == 0 && t.logged == sc->changes);
                else
                    CHECK(t.changed > 0 && t.missing == 0 && t.logged == 1 &&
                          v->mmu_log[seq0 % IA64_MMU_LOG].kind == IA64_MMU_ALL &&
                          v->mmu_seq == seq0 + 1);
                CHECK(t.stale == 0);
                CHECK(v->mmu_dseq - data0 == sc->data_changes);
            } else {
                /* The writer completeness check of this test fails. */
                CHECK(t.missing > 0 && t.changed == (sc->changes ? sc->changes : t.changed));
            }
        }
    }
    for (unsigned f = 0; f < IA64_MMU_WRITERS; ++f)
        CHECK(family_seen[f]);
    /* Purges log only valid -> invalid transitions: nothing matches. */
    fresh_state();
    prep_d_page0();
    uint64_t seq = v->mmu_seq;
    op_purge(IA64_OP_PTC_L, page_va(9), 12);
    op_purge(IA64_OP_PTR_D, page_va(9), 12);
    op_ptc_e(0);
    CHECK(v->mmu_seq == seq + 1); /* ptc.e purged page 0's entry only */
    seq = v->mmu_seq;
    op_ptc_e(0);
    CHECK(v->mmu_seq == seq);
    /* ptc.e of both sides: every valid entry once, nothing else. */
    fresh_state();
    for (unsigned k = 0; k < 3; ++k)
        op_itc(1, page_va(k), rw(page_pa(k)), 12, 0);
    for (unsigned k = 0; k < 2; ++k)
        op_itc(0, page_va(k), rw(page_pa(k)), 12, 0);
    struct image m;
    take_image(&m);
    seq = v->mmu_seq;
    op_ptc_e(0);
    struct tally t = tally_since(&m, seq);
    CHECK(t.changed == 5 && t.missing == 0 && t.logged == 5 && t.stale == 0);
    /* Same-value writes are writes: mov rr and mov pkr log them. */
    seq = v->mmu_seq;
    op_rr(3, v->s.rr[3]);
    op_pkr(2, v->s.pkr[2]);
    CHECK(v->mmu_seq == seq + 2 && v->mmu_log[seq % IA64_MMU_LOG].kind == IA64_MMU_RR &&
          v->mmu_log[seq % IA64_MMU_LOG].index == 3 &&
          v->mmu_log[(seq + 1) % IA64_MMU_LOG].kind == IA64_MMU_PKR);
    /* mov pkr clears only valid duplicates. */
    op_pkr(0, 1 | (uint64_t)0x99 << 8);
    op_pkr(1, (uint64_t)0x99 << 8); /* invalid, same key */
    seq = v->mmu_seq;
    op_pkr(2, 1 | (uint64_t)0x99 << 8);
    CHECK(v->mmu_seq == seq + 2 && v->s.pkr[0] == ((uint64_t)0x99 << 8));
    printf(
        "mmu_log_test: writers: %u scenarios over %u families, each exact and each detected when muted\n",
        (unsigned)NSCENARIOS, (unsigned)IA64_MMU_WRITERS);
    ++tests;
}

/* ---- hostwin instances ---------------------------------------------------- */

static struct hostwin_vcpu *make(bool check)
{
    struct hostwin_config c = {.w = 40, .slots = 8, .check = check};
    struct hostwin_vcpu *hw = hostwin_create(&c, hostwin_linux_backend(), hostwin_linux_ctx());
    CHECK(hw != NULL);
    return hw;
}
static struct hostwin_stats stats(const struct hostwin_vcpu *hw)
{
    struct hostwin_stats s;
    hostwin_get_stats(hw, &s);
    return s;
}
static enum hostwin_fill fill(struct hostwin_vcpu *hw, uint64_t gva, bool write)
{
    struct hostwin_tag t;
    CHECK(hostwin_bind(hw, v, gva, &t) == HOSTWIN_OK);
    enum hostwin_exit_reason why;
    return hostwin_resolve(hw, v, gva, 8, write, &why);
}

struct shadow_rec {
    uint64_t gva;
    gpa_t pa;
    bool writable;
};
struct shadow_set {
    struct shadow_rec r[512];
    unsigned n;
};
static void collect(uint64_t gva, gpa_t pa, bool writable, void *arg)
{
    struct shadow_set *s = arg;
    CHECK(s->n < 512);
    s->r[s->n++] = (struct shadow_rec){gva, pa, writable};
}
static int by_gva(const void *a, const void *b)
{
    const struct shadow_rec *x = a, *y = b;
    return x->gva < y->gva ? -1 : x->gva > y->gva;
}
static void shadows(const struct hostwin_vcpu *hw, struct shadow_set *s)
{
    s->n = 0;
    hostwin_test_shadows(hw, collect, s);
    qsort(s->r, s->n, sizeof(s->r[0]), by_gva);
}
static bool same_shadows(const struct shadow_set *a, const struct shadow_set *b)
{
    if (a->n != b->n)
        return false;
    for (unsigned i = 0; i < a->n; ++i)
        if (a->r[i].gva != b->r[i].gva || a->r[i].pa != b->r[i].pa ||
            a->r[i].writable != b->r[i].writable)
            return false;
    return true;
}

/* ---- B. replay = full diff ------------------------------------------------ */

/* Shadowed pages: k = 0..5 through D-TC entries, 6..7 through DTR 0..1;
 * page 8 only in the I-TC; region 3 is never mapped. */
#define NPAGES 8
static void shadowed_state(void)
{
    fresh_state();
    for (unsigned k = 0; k < 6; ++k)
        op_itc(1, page_va(k), rw(page_pa(k)), 12, 0);
    for (unsigned k = 6; k < NPAGES; ++k)
        op_itr(1, k - 6, page_va(k), rw(page_pa(k)), 12, 0);
    op_itc(0, page_va(8), rw(page_pa(8)), 12, 0);
}

enum path { PATH_REPLAY, PATH_FULL, PATH_UNCHANGED };
struct change {
    const char *name;
    void (*apply)(void);
    enum path path; /* A's update */
    int drops;      /* shadows dropped (-1: all of them) */
};

static void ch_dtc_insert(void)
{
    op_itc(1, page_va(0), rw(page_pa(10)), 12, 0);
}
static void ch_dtc_purge(void)
{
    op_purge(IA64_OP_PTC_L, page_va(1), 12);
}
static void ch_itc(void)
{
    op_itc(0, page_va(9), rw(page_pa(9)), 12, 0);
    op_purge(IA64_OP_PTR_I, page_va(8), 12);
}
static void ch_dtr_insert(void)
{
    op_itr(1, 4, page_va(2), rw(page_pa(2)), 16, 0);
}
static void ch_dtr_purge(void)
{
    op_purge(IA64_OP_PTR_D, page_va(6), 12);
}
static void ch_itr(void)
{
    op_itr(0, 3, page_va(3), rw(page_pa(3)), 16, 0);
    op_purge(IA64_OP_PTR_I, page_va(3), 16);
}
static void ch_rr(void)
{
    op_rr(1, rr_value(0x301, 12, false));
}
static void ch_rr_unused(void)
{
    op_rr(3, rr_value(0x303, 13, false));
}
static void ch_rr_same(void)
{
    op_rr(1, v->s.rr[1]);
}
static void ch_pkr(void)
{
    op_pkr(9, 1 | (uint64_t)0x123 << 8);
}
static void ch_pkr_same(void)
{
    op_pkr(9, v->s.pkr[9]);
}
static void ch_reset(void)
{
    vcpu_reset(
        v, &(struct ia64_entry_regs){.ip = 0x100000, .psr = IA64_PSR_IC | IA64_PSR_DT, .rsc = 3});
}
/* 16 logged changes (the replay limit) and 17 (overflow): insertions of
 * new pages, the last of which re-maps page 4. */
static void ch_16(void)
{
    for (unsigned k = 0; k < 15; ++k)
        op_itc(1, page_va(20 + k), rw(page_pa(20 + k)), 12, 0);
    op_purge(IA64_OP_PTC_L, page_va(4), 12);
}
static void ch_17(void)
{
    for (unsigned k = 0; k < 16; ++k)
        op_itc(1, page_va(20 + k), rw(page_pa(20 + k)), 12, 0);
    op_purge(IA64_OP_PTC_L, page_va(4), 12);
}
/* A purge followed by the identical re-insertion (same slot). */
static void ch_purge_reinsert_tr(void)
{
    struct ia64_tlb_entry saved = v->s.tr[1][1];
    op_purge(IA64_OP_PTR_D, page_va(7), 12);
    op_itr(1, 1, page_va(7), saved.pte, 12, 0);
    CHECK(same_entry(&saved, &v->s.tr[1][1]));
}
static void ch_purge_reinsert_tc(void)
{
    unsigned slot = slot_of(ia64_tlb_find(&v->s, page_va(5), 1), 1);
    struct ia64_tlb_entry saved = v->s.tc[1][slot];
    op_purge(IA64_OP_PTC_L, page_va(5), 12);
    v->s.tc_next[1] = slot; /* tc_next is not logged state */
    op_itc(1, page_va(5), saved.pte, 12, 0);
    CHECK(same_entry(&saved, &v->s.tc[1][slot]));
}
/* Rights-only changes (PTE.ar 2 -> 0: read only). */
static void ch_rights_tr(void)
{
    /* SDM Vol.2 Table 4-1 requires PTR before a same-slot rights update. */
    op_purge(IA64_OP_PTR_D, page_va(6), 12);
    op_itr(1, 0, page_va(6), pte(page_pa(6), 0, true, true), 12, 0);
}
static void ch_rights_tc(void)
{
    op_itc(1, page_va(3), pte(page_pa(3), 0, true, true), 12, 0);
}
/* Re-entry after a non-executing interlude: the vCPU leaves virtual
 * data addressing (no region entry), MMU changes happen, PSR comes back. */
static void ch_interlude(void)
{
    uint64_t psr = v->s.psr;
    v->s.psr &= ~IA64_PSR_DT;
    op_purge(IA64_OP_PTC_L, page_va(2), 12);
    op_itc(1, page_va(30), rw(page_pa(30)), 12, 0);
    op_rr(4, v->s.rr[4]);
    v->s.psr = psr;
}
static void ch_cpl_interlude(void)
{
    v->s.psr |= UINT64_C(3) << 32;
    v->s.psr &= ~(UINT64_C(3) << 32);
}
static void ch_cpl(void)
{
    v->s.psr |= UINT64_C(3) << 32;
}

static const struct change changes[] = {
    {"D-side TC insertion over a shadowed page", ch_dtc_insert, PATH_REPLAY, 1},
    {"D-side TC purge", ch_dtc_purge, PATH_REPLAY, 1},
    {"I-side TC insertion and purge", ch_itc, PATH_REPLAY, 0},
    {"D-side TR insertion over a shadowed page", ch_dtr_insert, PATH_REPLAY, 1},
    {"D-side TR purge", ch_dtr_purge, PATH_REPLAY, 1},
    {"I-side TR insertion and purge", ch_itr, PATH_REPLAY, 0},
    {"RR change", ch_rr, PATH_REPLAY, -1},
    {"RR change of an unmapped region", ch_rr_unused, PATH_REPLAY, -1},
    {"RR written with its value", ch_rr_same, PATH_REPLAY, 0},
    {"PKR change", ch_pkr, PATH_REPLAY, -1},
    {"PKR written with its value", ch_pkr_same, PATH_REPLAY, 0},
    {"ALL (vcpu_reset)", ch_reset, PATH_FULL, -1},
    {"16 changes (replay limit)", ch_16, PATH_REPLAY, 1},
    {"17 changes (overflow)", ch_17, PATH_FULL, 1},
    {"TR purge and identical re-insertion", ch_purge_reinsert_tr, PATH_REPLAY, 0},
    {"TC purge and identical re-insertion", ch_purge_reinsert_tc, PATH_REPLAY, 0},
    {"TR rights-only change", ch_rights_tr, PATH_REPLAY, 1},
    {"TC rights-only change", ch_rights_tc, PATH_REPLAY, 1},
    {"re-entry after a physical-mode interlude", ch_interlude, PATH_REPLAY, 1},
    {"re-entry after a CPL interlude", ch_cpl_interlude, PATH_UNCHANGED, 0},
    {"CPL change at entry", ch_cpl, PATH_REPLAY, -1},
};

/* A runs the product configuration (check off: only the shadow comparison
 * with B judges its replay) and then the check mode (the check also
 * compares the replay's actions with the full diff's). */
static void test_replay_equals_full_diff(void)
{
    static struct shadow_set a, b, before;
    for (unsigned n = 0; n < 2 * sizeof(changes) / sizeof(changes[0]); ++n) {
        const struct change *c = &changes[n % (sizeof(changes) / sizeof(changes[0]))];
        bool check = n >= sizeof(changes) / sizeof(changes[0]);
        shadowed_state();
        struct hostwin_vcpu *A = make(check), *B = make(true);
        hostwin_enter(A, v);
        hostwin_enter(B, v);
        for (unsigned k = 0; k < NPAGES; ++k) {
            CHECK(fill(A, page_va(k) + 8, false) == HOSTWIN_MAPPED);
            CHECK(fill(B, page_va(k) + 8, false) == HOSTWIN_MAPPED);
        }
        hostwin_enter(A, v);
        hostwin_resync(B);
        hostwin_enter(B, v);
        shadows(A, &before);
        shadows(B, &b);
        CHECK(before.n == NPAGES && same_shadows(&before, &b));
        struct hostwin_stats a0 = stats(A), b0 = stats(B);
        c->apply();
        hostwin_enter(A, v);
        hostwin_resync(B);
        hostwin_enter(B, v);
        struct hostwin_stats a1 = stats(A), b1 = stats(B);
        shadows(A, &a);
        shadows(B, &b);
        if (!same_shadows(&a, &b))
            fprintf(stderr, "mmu_log_test: %s: replay keeps %u shadows, full diff %u\n", c->name,
                    a.n, b.n);
        CHECK(same_shadows(&a, &b));
        CHECK(a1.shadow_drops - a0.shadow_drops == b1.shadow_drops - b0.shadow_drops);
        CHECK(a1.entry_diffs - a0.entry_diffs == b1.entry_diffs - b0.entry_diffs);
        CHECK(a1.context_full_drops - a0.context_full_drops ==
              b1.context_full_drops - b0.context_full_drops);
        CHECK(b1.context_full_diffs == b0.context_full_diffs + 1);
        switch (c->path) {
        case PATH_REPLAY:
            CHECK(a1.context_replays == a0.context_replays + 1 &&
                  a1.context_full_diffs == a0.context_full_diffs);
            break;
        case PATH_FULL:
            CHECK(a1.context_full_diffs == a0.context_full_diffs + 1 &&
                  a1.context_replays == a0.context_replays);
            break;
        case PATH_UNCHANGED:
            CHECK(a1.context_unchanged == a0.context_unchanged + 1);
            break;
        }
        CHECK(a1.check_diffs == a0.check_diffs + (check && c->path != PATH_FULL));
        unsigned dropped = before.n - a.n;
        CHECK(c->drops < 0 ? a.n == 0 : dropped == (unsigned)c->drops);
        /* Nothing stale: every remaining shadow still matches the TLB. */
        for (unsigned k = 0; k < a.n; ++k) {
            const struct ia64_tlb_entry *e = ia64_tlb_find(&v->s, a.r[k].gva, 1);
            CHECK(e && e->valid &&
                  ((e->pte & ((UINT64_C(1) << 50) - PAGE) & ~ia64_low_mask(e->page_shift)) |
                   (a.r[k].gva & ia64_low_mask(e->page_shift) & ~(uint64_t)(PAGE - 1))) ==
                      a.r[k].pa);
        }
        hostwin_destroy(A);
        hostwin_destroy(B);
    }
    printf("mmu_log_test: replay: %u change kinds, shadows and drops equal to the full diff, "
           "with and without the check\n",
           (unsigned)(sizeof(changes) / sizeof(changes[0])));
    ++tests;
}

/* ---- forked children -------------------------------------------------------- */

static char child_out[1 << 15];
static int run_child(void (*body)(const void *), const void *arg)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        alarm(60);
        body(arg);
        _exit(0);
    }
    close(fds[1]);
    size_t used = 0;
    for (ssize_t n; used + 1 < sizeof(child_out) &&
                    (n = read(fds[0], child_out + used, sizeof(child_out) - 1 - used)) > 0;)
        used += (size_t)n;
    child_out[used] = 0;
    close(fds[0]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    return status;
}
#define CHILD_EXITED(s, code) (WIFEXITED(s) && WEXITSTATUS(s) == (code))
static bool panicked_naming(const char *what)
{
    char want[160];
    snprintf(want, sizeof(want), "[panic] hostwin: MMU log check: %s changed without a log entry",
             what);
    return strstr(child_out, want) != NULL;
}

/* ---- C. the check finds unlogged writes ------------------------------------ */

struct direct {
    const char *element;
    void (*write)(void);
};
static void dw_dtc(void)
{
    v->s.tc[1][7].valid = true;
}
static void dw_itc(void)
{
    CHECK(ia64_tlb_find(&v->s, page_va(8), 0) == &v->s.tc[0][0]);
    v->s.tc[0][0].pte ^= 1u << 9;
}
static void dw_dtr(void)
{
    v->s.tr[1][1].itir ^= 1u << 8;
}
static void dw_itr(void)
{
    v->s.tr[0][4].valid = true;
}
static void dw_rr(void)
{
    v->s.rr[6] ^= 1u << 8;
}
static void dw_pkr(void)
{
    v->s.pkr[11] = 1;
}
static const struct direct directs[] = {
    {"D-side TC 7", dw_dtc}, {"I-side TC 0", dw_itc}, {"D-side TR 1", dw_dtr},
    {"I-side TR 4", dw_itr}, {"RR 6", dw_rr},         {"PKR 11", dw_pkr},
};
static void direct_body(const void *arg)
{
    const struct direct *d = arg;
    shadowed_state();
    struct hostwin_vcpu *hw = make(true);
    hostwin_enter(hw, v);
    CHECK(fill(hw, page_va(1) + 8, false) == HOSTWIN_MAPPED);
    hostwin_enter(hw, v);
    if (d)
        d->write();
    hostwin_enter(hw, v);
    _exit(0);
}
/* A restored state (struct copy, mmu_seq included) can move the count to a
 * value the instance has seen with other contents (snapshot restore). restore_mode 0: the restored count equals the one
 * seen, no hostwin_resync; 1: the same with hostwin_resync; 2: the count
 * moved back below the one seen, no resync (unsigned distance: full diff). */
static int restore_mode;
static void restore_body(const void *arg)
{
    (void)arg;
    shadowed_state();
    struct hostwin_vcpu *hw = make(true);
    static struct vcpu saved;
    saved = *v;
    hostwin_enter(hw, v);
    CHECK(fill(hw, page_va(1) + 8, false) == HOSTWIN_MAPPED);
    op_purge(IA64_OP_PTC_L, page_va(0), 12);
    op_itc(1, page_va(1), rw(page_pa(11)), 12, 0);
    hostwin_enter(hw, v);
    CHECK(fill(hw, page_va(1) + 8, false) == HOSTWIN_MAPPED);
    struct shadow_set s;
    shadows(hw, &s);
    CHECK(s.n == 1 && s.r[0].pa == page_pa(11));
    uint64_t seen = v->mmu_seq;
    *v = saved; /* page 1 maps page_pa(1) again */
    CHECK(v->mmu_seq < seen);
    if (restore_mode != 2)
        v->mmu_seq = seen;
    if (restore_mode == 1)
        hostwin_resync(hw);
    hostwin_enter(hw, v);
    /* Right: the shadow of page_pa(11) is gone. */
    shadows(hw, &s);
    CHECK(s.n == 0);
    _exit(0);
}

static void test_check_finds_unlogged_writes(void)
{
    for (unsigned i = 0; i < sizeof(directs) / sizeof(directs[0]); ++i) {
        int s = run_child(direct_body, &directs[i]);
        const char *what = directs[i].element;
        if (!CHILD_EXITED(s, 1) || !panicked_naming(what))
            fprintf(stderr, "mmu_log_test: direct write of %s: status %#x\n%s", what, (unsigned)s,
                    child_out);
        CHECK(CHILD_EXITED(s, 1) && panicked_naming(what));
    }
    /* Control: no write, no panic. */
    CHECK(CHILD_EXITED(run_child(direct_body, NULL), 0));
    /* Restore: with hostwin_resync, or with a count that is not seen, the
     * full diff runs and the result is right; with the count moved back to
     * the one seen and no resync, the check panics. */
    restore_mode = 1;
    CHECK(CHILD_EXITED(run_child(restore_body, NULL), 0));
    restore_mode = 2;
    CHECK(CHILD_EXITED(run_child(restore_body, NULL), 0));
    restore_mode = 0;
    int s = run_child(restore_body, NULL);
    CHECK(CHILD_EXITED(s, 1) && panicked_naming("D-side TC 0"));
    printf(
        "mmu_log_test: check: unlogged writes of D/I TC, D/I TR, RR and PKR named; a restore without "
        "resync to a seen count panics, with resync or an unseen count the full diff runs\n");
    ++tests;
}

/* ---- D. mutation test --------------------------------------------------------- */

static void mutation_body(const void *arg)
{
    const struct scenario *sc = arg;
    fresh_state();
    struct hostwin_vcpu *hw = make(true);
    hostwin_enter(hw, v);
    sc->prepare();
    hostwin_enter(hw, v);
    ia64_mmu_test_mute = getenv("MMU_LOG_TEST_MUTE") ? UINT32_C(1) << sc->family : 0;
    const char *what = sc->act();
    fprintf(stderr, "expects: %s\n", what);
    hostwin_enter(hw, v);
    _exit(0);
}
static void test_mutations(void)
{
    unsigned detected = 0;
    for (unsigned i = 0; i < NSCENARIOS; ++i) {
        const struct scenario *sc = &scenarios[i];
        unsetenv("MMU_LOG_TEST_MUTE");
        int s = run_child(mutation_body, sc);
        if (!CHILD_EXITED(s, 0))
            fprintf(stderr, "mmu_log_test: %s unmuted: status %#x\n%s", sc->name, (unsigned)s,
                    child_out);
        CHECK(CHILD_EXITED(s, 0));
        setenv("MMU_LOG_TEST_MUTE", "1", 1);
        s = run_child(mutation_body, sc);
        unsetenv("MMU_LOG_TEST_MUTE");
        const char *e = strstr(child_out, "expects: ");
        CHECK(e != NULL);
        char what[64];
        CHECK(sscanf(e, "expects: %63[^\n]", what) == 1);
        if (!CHILD_EXITED(s, 1) || !panicked_naming(what))
            fprintf(stderr, "mmu_log_test: %s muted: status %#x\n%s", sc->name, (unsigned)s,
                    child_out);
        CHECK(CHILD_EXITED(s, 1) && panicked_naming(what));
        const char *p = strstr(child_out, "[panic]");
        printf("mmu_log_test: mutation %-28s -> %.*s\n", sc->name, (int)strcspn(p, "\n"), p);
        ++detected;
    }
    printf(
        "mmu_log_test: mutation: %u of %u muted writers detected by the check, each control silent\n",
        detected, (unsigned)NSCENARIOS);
    ++tests;
}

/* ---- E. statistics and the fast path ------------------------------------------ */

static void test_fast_path(void)
{
    shadowed_state();
    struct hostwin_vcpu *hw = make(false);
    hostwin_enter(hw, v); /* first: the full diff */
    struct hostwin_stats s = stats(hw);
    CHECK(s.context_full_diffs == 1 && s.context_unchanged == 0 && s.check_diffs == 0);
    for (unsigned k = 0; k < NPAGES; ++k)
        CHECK(fill(hw, page_va(k), true) == HOSTWIN_MAPPED);
    s = stats(hw);
    for (unsigned n = 0; n < 1000; ++n)
        hostwin_enter(hw, v);
    struct hostwin_stats t = stats(hw);
    CHECK(t.context_unchanged == s.context_unchanged + 1000 &&
          t.context_replays == s.context_replays && t.context_full_diffs == s.context_full_diffs &&
          t.check_diffs == 0 && t.live_mappings == NPAGES);
    /* A PSR bit outside the context (and PSR.da, single-translation) stays on the fast
     * path; a logged change replays; a resync forces the full diff. */
    v->s.psr ^= IA64_PSR_I | IA64_PSR_DA;
    hostwin_enter(hw, v);
    op_purge(IA64_OP_PTC_L, page_va(0), 12);
    hostwin_enter(hw, v);
    hostwin_resync(hw);
    hostwin_enter(hw, v);
    struct hostwin_stats u = stats(hw);
    CHECK(u.context_unchanged == t.context_unchanged + 1 &&
          u.context_replays == t.context_replays + 1 &&
          u.context_full_diffs == t.context_full_diffs + 1 && u.live_mappings == NPAGES - 1);
    /* A context PSR field (cpl) without any logged change: every shadow
     * goes, on the replay path (no full diff). */
    v->s.psr |= UINT64_C(3) << 32;
    hostwin_enter(hw, v);
    v->s.psr &= ~(UINT64_C(3) << 32);
    t = stats(hw);
    CHECK(t.live_mappings == 0 && t.context_replays == u.context_replays + 1 &&
          t.context_full_drops == u.context_full_drops + 1 &&
          t.context_full_diffs == u.context_full_diffs);
    hostwin_enter(hw, v);
    /* The update inside hostwin_resolve: a walker insertion is replayed. */
    prep_walker_old();
    hostwin_enter(hw, v);
    s = stats(hw);
    CHECK(fill(hw, WALK_VA, false) == HOSTWIN_MAPPED);
    t = stats(hw);
    CHECK(t.context_replays == s.context_replays + 1 &&
          t.context_full_diffs == s.context_full_diffs &&
          t.translate_calls == s.translate_calls + 1 && t.probe_calls == 0);
    /* Statistics builds only: the update's clock reads. */
    if (IA64VMM_TIER1_STATS)
        CHECK(t.snapshot_ticks > 0);
    else
        CHECK(t.snapshot_ticks == 0);
    hostwin_destroy(hw);
    printf(
        "mmu_log_test: fast path: 1000 unchanged entries without an update; replay, full diff and "
        "resolve counted (TIER1_STATS=%d)\n",
        IA64VMM_TIER1_STATS);
    ++tests;
}

/* F. Measurement (only when named): steady-state hostwin_enter of a
 * product instance (check off) with 8 live shadows, MMU_LOG_BENCH_CALLS
 * calls (default 10^8, 0 for the baseline of `perf stat -e cycles:u`). */
static void bench_enter(void)
{
    const char *env = getenv("MMU_LOG_BENCH_CALLS");
    uint64_t calls = env ? strtoull(env, NULL, 0) : UINT64_C(100000000);
    shadowed_state();
    struct hostwin_vcpu *hw = make(false);
    hostwin_enter(hw, v);
    for (unsigned k = 0; k < NPAGES; ++k)
        CHECK(fill(hw, page_va(k), false) == HOSTWIN_MAPPED);
    hostwin_enter(hw, v);
    struct hostwin_stats s = stats(hw);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint64_t n = 0; n < calls; ++n)
        hostwin_enter(hw, v);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    struct hostwin_stats t = stats(hw);
    CHECK(t.context_unchanged == s.context_unchanged + calls &&
          t.context_replays == s.context_replays && t.context_full_diffs == s.context_full_diffs &&
          t.live_mappings == NPAGES);
    double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    printf(
        "mmu_log_test: bench: %llu steady-state hostwin_enter calls, %.3f ns per call (TIER1_STATS=%d)\n",
        (unsigned long long)calls, calls ? ns / (double)calls : 0.0, IA64VMM_TIER1_STATS);
    hostwin_destroy(hw);
    ++tests;
}

/* Usage: tier1-mmu-log-test [writers|replay|check|mutation|fastpath|bench]:
 * the parts A..E, or the one named (to show a broken variant failing in a
 * part, or the measurement F). */
int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    log_set_level(LOG_ERROR);
    CHECK(hostwin_linux_init());
    ram = hostwin_linux_ram(0, RAM_BYTES);
    CHECK(ram != NULL);
    struct gm_region r = {.base = 0, .size = RAM_BYTES, .kind = GM_RAM, .name = "ram", .hva = ram};
    CHECK(gm_add_region(&r));
    struct vcpu_config cfg = {
        .engine = ENGINE_INTERP, .itc_icount = true, .cpu_model = IA64_CPU_MADISON};
    v = vcpu_create(&cfg);
    /* The sequence starts in the vCPU's own 2^32 range; the reset logged one
     * ALL entry. */
    CHECK(v != NULL && (uint32_t)v->mmu_seq == 1 && v->mmu_dseq == v->mmu_seq &&
          v->mmu_log[(v->mmu_seq - 1) % IA64_MMU_LOG].kind == IA64_MMU_ALL);
    static const struct {
        const char *name;
        void (*run)(void);
        bool always;
    } parts[] = {{"writers", test_writers, true},
                 {"replay", test_replay_equals_full_diff, true},
                 {"check", test_check_finds_unlogged_writes, true},
                 {"mutation", test_mutations, true},
                 {"fastpath", test_fast_path, true},
                 {"bench", bench_enter, false}};
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i)
        if (only ? !strcmp(only, parts[i].name) : parts[i].always)
            parts[i].run();
    CHECK(tests > 0);
    vcpu_destroy(v);
    printf("mmu_log_test: %u tests, %u checks passed\n", tests, checks);
    return 0;
}
