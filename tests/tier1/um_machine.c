/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit-test machine (see um_machine.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/um_machine.h"
#include "tests/tier1/compare.h"
#include "engine/interp/interp.h"
#include "engine/cache/validity.h"
#include "ia64vmm/guest_mem.h"
#include "tests/tier1/hostwin_linux.h"
#include "tests/tier1/bof.h"

/* memfd-backed (tests/tier1/hostwin_linux.c) so that host windows map the
 * same pages the interpreter and helpers use. */
static uint8_t *ram;

_Noreturn void um_fail(const char *file, unsigned line, const char *what)
{
    fprintf(stderr, "%s:%u: check failed: %s\n", file, line, what);
    exit(1);
}

void um_machine_init(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    ram = hostwin_linux_ram(UM_RAM_BASE, UM_RAM_SIZE);
    UM_CHECK(ram != NULL);
    UM_CHECK(gm_add_region(&(struct gm_region){
        .base = UM_RAM_BASE, .size = UM_RAM_SIZE, .kind = GM_RAM, .hva = ram, .name = "unit-ram"}));
    /* Every IVT bundle branches to itself: interruptions spin and retire. */
    struct ienc_bundle spin = {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}};
    for (uint64_t at = UM_IVT; at < UM_IVT + 0x8000; at += 16)
        um_emit(at, &spin, 1);
}

uint8_t *um_ram(uint64_t gpa)
{
    um_machine_init();
    UM_CHECK(gpa >= UM_RAM_BASE && gpa < UM_RAM_BASE + UM_RAM_SIZE);
    return ram + (gpa - UM_RAM_BASE);
}

void um_load(uint64_t gpa, const void *bytes, size_t n)
{
    UM_CHECK(gpa >= UM_RAM_BASE && gpa + n <= UM_RAM_BASE + UM_RAM_SIZE);
    memcpy(ram + (gpa - UM_RAM_BASE), bytes, n);
    code_notify_vmm_write(gpa, n);
}

void um_emit(uint64_t gpa, const struct ienc_bundle *b, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        uint64_t w[2];
        ienc_pack(&b[i], &w[0], &w[1]);
        um_load(gpa + 16u * i, w, 16);
    }
}

void um_load_file(uint64_t gpa, const char *path)
{
    FILE *f = fopen(path, "rb");
    UM_CHECK(f != NULL);
    static uint8_t buf[0x8000];
    size_t n = fread(buf, 1, sizeof(buf), f);
    UM_CHECK(n > 0 && !ferror(f) && feof(f));
    fclose(f);
    um_load(gpa, buf, n);
}

uint64_t um_cfm(unsigned sof, unsigned sol, unsigned sor, unsigned rrb_gr, unsigned rrb_pr)
{
    return (uint64_t)sof | ((uint64_t)sol << 7) | ((uint64_t)(sor / 8) << 14) |
           ((uint64_t)rrb_gr << 18) | ((uint64_t)rrb_pr << 32);
}

struct vcpu *um_vcpu(uint64_t ip)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg);
    UM_CHECK(v != NULL);
    vcpu_reset(v, &(struct ia64_entry_regs){.ip = ip, .psr = IA64_PSR_IC, .rsc = 3});
    v->s.cr[CR_IVA] = UM_IVT;
    v->s.cfm = um_cfm(8, 8, 0, 0, 0);
    return v;
}

void um_virtual_code(struct vcpu *v)
{
    const unsigned ps = 22;
    uint64_t page = UM_RAM_BASE & ~((UINT64_C(1) << ps) - 1);
    for (unsigned vrn = 0; vrn < 3; ++vrn) {
        unsigned rid = 0x100 + vrn;
        v->s.rr[vrn] = ((uint64_t)rid << 8) | (ps << 2);
        ia64_mmu_changed(v, IA64_MMU_RR, 0, vrn); /* direct writes log */
        /* PTE (SDM Vol.2 §4.1.1.5): p, ma = WB, a, d, pl 0, ar 1 (RX), ppn. */
        uint64_t pte = 1 | (UINT64_C(1) << 5) | (UINT64_C(1) << 6) | (UINT64_C(1) << 9) | page;
        v->s.tr[0][vrn] = (struct ia64_tlb_entry){
            .va = page, .pte = pte, .itir = ps << 2, .rid = rid, .page_shift = ps, .valid = true};
        ia64_mmu_changed(v, IA64_MMU_TR, 0, vrn);
    }
    v->s.psr |= IA64_PSR_IT;
}

void um_gr(struct vcpu *v, unsigned reg, uint64_t value, bool nat)
{
    ia64_gr_write(&v->s, reg, value, nat);
}

void um_pr(struct vcpu *v, unsigned reg, bool value)
{
    ia64_pr_write(&v->s, reg, value);
}

static void print_line(void *arg, const char *text)
{
    (void)arg;
    fprintf(stderr, "%s\n", text);
}

enum t1_compile_status um_compile(struct vcpu *v, struct um_region *r, char *err, size_t n)
{
    memset(r, 0, sizeof(*r));
    r->form = calloc(1, sizeof(*r->form));
    r->ir = calloc(1, sizeof(*r->ir));
    UM_CHECK(r->form && r->ir);
    unsigned max = T1_MAX_INSNS;
    for (;;) {
        const char *reason = NULL;
        enum t1_compile_status st = t1_form(v, 0, max, r->form, &reason);
        if (st != T1_COMPILED) {
            snprintf(err, n, "form: %s", reason ? reason : "?");
            return st;
        }
        unsigned fail_insn;
        if (!t1_lift(r->form, r->ir, &fail_insn)) {
            if (fail_insn && fail_insn < r->form->ninsns) {
                max = fail_insn;
                continue;
            }
            snprintf(err, n, "lift: %s", r->ir->fail_reason ? r->ir->fail_reason : "?");
            return T1_REJECTED;
        }
        break;
    }
    while (!t1_optimize(r->ir, true, err, n)) {
        if (getenv("T1_UNIT_IR_DUMP"))
            t1_ir_print(r->ir, print_line, NULL);
        unsigned boundary = r->ir->boundary_insn, fail_insn;
        const char *reason;
        /* An unfolded NATCHK ends the region before its instruction. */
        if (boundary == T1_NO_INSN || !boundary ||
            t1_form(v, 0, boundary, r->form, &reason) != T1_COMPILED ||
            !t1_lift(r->form, r->ir, &fail_insn))
            return T1_REJECTED;
    }
    if (getenv("T1_UNIT_IR_DUMP"))
        t1_ir_print(r->ir, print_line, NULL);
    r->max_path = t1_ir_max_path(r->ir, T1_HEADER_BLOCK);
    r->guards = (unsigned)(__builtin_popcountll(r->ir->nat_guard[0]) +
                           __builtin_popcountll(r->ir->nat_guard[1]));
    r->valid = true;
    return T1_COMPILED;
}

void um_region_free(struct um_region *r)
{
    free(r->form);
    free(r->ir);
    memset(r, 0, sizeof(*r));
}

bool um_can_enter(struct um_region *r, struct vcpu *v, uint64_t budget)
{
    if (!r || !r->valid)
        return false;
    const struct t1_contract *c = &r->form->c;
    const struct ia64_state *s = &v->s;
    /* No frame base in the contract: G names stacked registers by frame
     * offset, checked at the entry's BOF. */
    if (s->ip != c->ip || s->slot != c->slot || s->psr != c->psr || s->cfm != c->cfm ||
        v->requested_exit || budget < r->max_path)
        return false;
    for (unsigned p = 0; p < 128; ++p)
        if (((r->ir->nat_guard[p / 64] >> (p % 64)) & 1) && s->nat[t1t_phys(s->rse_bof, p)])
            return false;
    for (unsigned p = 0; p < r->form->npages; ++p)
        if (!code_source_publish(v, r->form->page[p].pa, r->form->page[p].generation)) {
            r->valid = false;
            return false;
        }
    return true;
}

uint64_t um_data_hash(void)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    const uint64_t *w = (const uint64_t *)um_ram(UM_DATA);
    for (unsigned i = 0; i < UM_DATA_SIZE / 8; ++i)
        h = (h ^ w[i]) * UINT64_C(0x100000001b3);
    return h;
}

struct vcpu_exit um_dispatch(struct um_region *r, struct vcpu *v, uint64_t budget,
                             struct um_run *run)
{
    while (budget) {
        bool nat_ok = true;
        if (r && r->valid && v->s.ip == r->form->c.ip && v->s.slot == r->form->c.slot)
            for (unsigned p = 0; p < 128; ++p)
                if (((r->ir->nat_guard[p / 64] >> (p % 64)) & 1) &&
                    v->s.nat[t1t_phys(v->s.rse_bof, p)])
                    nat_ok = false;
        if (!nat_ok)
            ++run->guard_rejects;
        if (nat_ok && um_can_enter(r, v, budget)) {
            struct ir_eval_result res;
            if (!ir_eval_run(r->ir, v, budget, run->hooks, &res)) {
                fprintf(stderr, "ir_eval: %s\n", res.error);
                UM_CHECK(!"ir_eval invariant");
            }
            ++run->entries;
            budget -= res.consumed;
            run->tier1_retired += res.consumed;
            unsigned kind = r->ir->point[res.point].kind;
            ++run->exits[kind];
            run->forced += res.forced;
            if (run->captures && run->ncaptures < run->max_captures) {
                struct um_capture *c = &run->captures[run->ncaptures++];
                c->v = *v;
                c->data_hash = um_data_hash();
                c->point = res.point;
                c->forced = res.forced;
            }
            /* After a before-effect exit the lower tier executes
             * the instruction (its first and only execution). */
            if ((kind == T1_PT_SITE || kind == T1_PT_HELPER ||
                 (kind == T1_PT_EXIT && !res.consumed)) &&
                budget) {
                uint64_t before = v->retired;
                struct vcpu_exit e = interp_run(v, 1);
                ++run->lower_steps;
                budget -= v->retired - before;
                if (e.reason != VCPU_EXIT_BUDGET)
                    return e;
            }
            continue;
        }
        uint64_t before = v->retired;
        struct vcpu_exit e = interp_run(v, 1);
        ++run->lower_steps;
        budget -= v->retired - before;
        if (e.reason != VCPU_EXIT_BUDGET)
            return e;
    }
    return interp_run(v, 0);
}

void um_snapshot_take(struct um_snapshot *s, const struct vcpu *v)
{
    s->v = *v;
    s->ram = malloc(UM_RAM_SIZE);
    UM_CHECK(s->ram != NULL);
    memcpy(s->ram, ram, UM_RAM_SIZE);
}

void um_snapshot_restore(const struct um_snapshot *s, struct vcpu *v)
{
    *v = s->v;
    for (unsigned off = 0; off < UM_RAM_SIZE; off += 4096)
        if (memcmp(ram + off, s->ram + off, 4096)) {
            memcpy(ram + off, s->ram + off, 4096);
            code_notify_vmm_write(UM_RAM_BASE + off, 4096);
        }
}

void um_snapshot_free(struct um_snapshot *s)
{
    free(s->ram);
    s->ram = NULL;
}

struct diff_note {
    const char *label;
    unsigned count;
};
static void note(const char *what, uint64_t index, uint64_t reference, uint64_t actual, void *arg)
{
    struct diff_note *d = arg;
    if (d->count++ < 4)
        fprintf(stderr, "%s: %s[%llu] reference %#llx tier1 %#llx\n", d->label, what,
                (unsigned long long)index, (unsigned long long)reference,
                (unsigned long long)actual);
}

unsigned um_compare(const struct um_snapshot *snap, struct vcpu *ref, const struct um_run *run,
                    const struct vcpu *got, const struct vcpu_exit *got_exit, uint64_t budget,
                    const char *label)
{
    static uint8_t got_ram[UM_RAM_SIZE];
    memcpy(got_ram, ram, UM_RAM_SIZE);
    um_snapshot_restore(snap, ref);
    struct diff_note d = {label, 0};
    uint64_t end = snap->v.retired + budget;
    struct vcpu_exit ref_exit = {0};
    bool exited = false;
    for (unsigned i = 0; i < run->ncaptures && !exited; ++i) {
        const struct um_capture *c = &run->captures[i];
        while (ref->retired < c->v.retired) {
            ref_exit = interp_run(ref, 1);
            if (ref_exit.reason != VCPU_EXIT_BUDGET) {
                exited = true;
                break;
            }
        }
        if (exited || ref->retired != c->v.retired) {
            note("capture retired", i, ref->retired, c->v.retired, &d);
            break;
        }
        struct compare_run a = {NULL, ref, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
        struct compare_run b = {NULL, &c->v, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
        unsigned n = compare_runs(&a, &b, note, &d);
        uint64_t h = um_data_hash();
        if (h != c->data_hash)
            note("data hash", i, h, c->data_hash, &d);
        if (n || h != c->data_hash) {
            fprintf(stderr, "%s: in-place capture %u (point %u, %s) differs\n", label, i, c->point,
                    c->forced ? "forced" : "natural");
            break;
        }
    }
    if (!d.count) {
        if (!exited)
            ref_exit = interp_run(ref, end > ref->retired ? end - ref->retired : 0);
        struct compare_run a = {&ref_exit, ref, ram, UM_RAM_SIZE, NULL, 0, NULL, 0, NULL, 0};
        struct compare_run b = {got_exit, got, got_ram, UM_RAM_SIZE, NULL, 0, NULL, 0, NULL, 0};
        compare_runs(&a, &b, note, &d);
    }
    if (d.count)
        fprintf(stderr, "%s: %u difference(s)\n", label, d.count);
    return d.count;
}
