/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Generated code vs interp_run: the
 * regions of the guest fragments compiled by the real pipeline (isel, regalloc,
 * raverify, emit), run by t1_run with host windows, compared with the
 * interpreter in place at every materialized point and at the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/um_t1.h"
#include "tests/tier1/um_random.h"
#include "engine/tier1/ir.h"
#include "ia64/semantics.h"
#include "engine/interp/interp.h"
#include "ia64vmm/engine.h"
#include "tests/tier1/bof.h"

static uint64_t rng = 0x2545f4914f6cdd1du;
static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
static unsigned rnd(unsigned n)
{
    return (unsigned)(rnd64() % n);
}

struct totals {
    unsigned cases, captures, forced, retries, exits;
};
static struct totals T;

static void account(const struct umt_run *r)
{
    ++T.cases;
    T.captures += r->run.ncaptures;
    T.forced += r->forced;
    T.retries += r->retries;
    T.exits += r->nexits;
}

/* T1_UNIT_CODE_DUMP=dir: the code of every compiled region to dir/<label>.bin
 * (objdump -D -b binary -m i386:x86-64). */
static void dump_code(uint32_t id, const char *label)
{
    const char *dir = getenv("T1_UNIT_CODE_DUMP");
    if (!dir)
        return;
    const uint8_t *code;
    size_t size, ls, le;
    UM_CHECK(t1_test_region_code(id, &code, &size, &ls, &le));
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.bin", dir, label);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(code, 1, size, f);
    fclose(f);
    fprintf(stderr, "%s: loop body [%#zx, %#zx)\n", path, ls, le);
}

static void shladd_xor(const char *image)
{
    struct vcpu *v = um_vcpu(UM_CODE + 0x400), *ref = um_vcpu(UM_CODE + 0x400);
    um_load_file(UM_CODE, image);
    um_gr(v, 16, 5, false);
    um_gr(v, 17, 0x1234, false);
    um_gr(v, 18, 0xfeed, false);
    v->s.ar[AR_LC] = 50;
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    for (unsigned flags = 0; flags <= T1_COMPILE_VERIFY; flags += T1_COMPILE_VERIFY) {
        struct t1_compile_report rep;
        um_snapshot_restore(&snap, v);
        uint32_t id = umt_compile(v, flags, 0, 0, &rep, "shladd_xor");
        UM_CHECK(id != 0);
        dump_code(id, flags ? "shladd_xor-verify" : "shladd_xor");
        struct umt_run run;
        umt_case(v, ref, &snap, flags, 51 * 9 + 20, id, NULL, &run, false, "shladd_xor");
        account(&run);
        UM_CHECK(run.nexits >= 1);
        for (unsigned b = 0; b <= 60; ++b) {
            umt_case(v, ref, &snap, flags, b, id, NULL, &run, false, "shladd_xor budget");
            account(&run);
        }
        printf(
            "unit_codegen: shladd_xor (%s): %u insns, %u host insns in the loop, spills loop/restore %u/%u, "
            "%zu code bytes\n",
            flags ? "verify" : "production", rep.insns, rep.loop_host_insns, rep.loop_value_spills,
            rep.restore_only_spills, rep.code_bytes);
    }
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

/* Every site and helper point x occurrences 1..3 x the applicable forced
 * actions, then budgets 0..split_max, all compared in place and at the end. */
static void sweep(uint32_t id, struct vcpu *v, struct vcpu *ref, const struct um_snapshot *snap,
                  uint32_t flags, uint64_t budget, unsigned split_max, const char *label)
{
    char name[128];
    struct umt_run run;
    umt_case(v, ref, snap, flags, budget, id, NULL, &run, false, label);
    account(&run);
    UM_CHECK(run.nexits > 0);
    if (flags & T1_COMPILE_VERIFY) {
        unsigned n = t1_point_count(id);
        for (unsigned k = 0; k < n; ++k) {
            struct t1_point_info pi;
            UM_CHECK(t1_point_get(id, k, &pi));
            if (pi.kind != T1_PT_SITE && pi.kind != T1_PT_HELPER)
                continue;
            static const enum t1_force_action site_actions[] = {
                T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT, T1_FORCE_TRAP_RETRY};
            unsigned na = pi.kind == T1_PT_SITE ? 3 : 1;
            for (unsigned a = 0; a < na; ++a)
                for (unsigned occ = 1; occ <= 3; ++occ) {
                    struct umt_arm arm = {.point = pi.point_id,
                                          .occurrence = occ,
                                          .action = pi.kind == T1_PT_SITE ? site_actions[a]
                                                                          : T1_FORCE_HELPER_EXIT};
                    snprintf(name, sizeof(name), "%s force P%u #%u action %d", label, pi.point_id,
                             occ, arm.action);
                    umt_case(v, ref, snap, flags, budget, id, &arm, &run, false, name);
                    account(&run);
                }
        }
    }
    for (unsigned b = 0; b <= split_max; ++b) {
        snprintf(name, sizeof(name), "%s budget %u", label, b);
        umt_case(v, ref, snap, flags, b, id, NULL, &run, false, name);
        account(&run);
    }
}

enum gather_variant {
    GATHER_PHYS,
    GATHER_ROT,
    GATHER_ROTP,
    GATHER_SIDE_EXIT,
    GATHER_ALIAS63,
    GATHER_VIRT_VRN1,
    GATHER_VIRT_VRN2
};

static void gather(const char *image, enum gather_variant variant)
{
    static const char *names[] = {
        "gather-phys",           "gather-rot",           "gather-rotp",         "gather-side-exit",
        "gather-alias (bit 63)", "gather-alias (VRN 1)", "gather-alias (VRN 2)"};
    uint64_t alias = variant == GATHER_ALIAS63     ? UINT64_C(1) << 63
                     : variant == GATHER_VIRT_VRN1 ? UINT64_C(1) << 61
                     : variant == GATHER_VIRT_VRN2 ? UINT64_C(2) << 61
                                                   : 0;
    uint64_t entry = (UM_CODE + (variant == GATHER_ROTP ? 0x200 : 0)) | alias;
    struct vcpu *v = um_vcpu(entry), *ref = um_vcpu(entry);
    um_load_file(UM_CODE, image);
    unsigned n = 40;
    uint64_t index = UM_DATA, table = UM_DATA + 0x2000, out = UM_DATA + 0x4000;
    memset(um_ram(UM_DATA), 0, UM_DATA_SIZE);
    uint64_t t[64];
    for (unsigned i = 0; i < 64; ++i) {
        t[i] = rnd64() & 0xfffff;
        memcpy(um_ram(table + 8 * i), &t[i], 8);
    }
    for (unsigned i = 0; i < n; ++i) {
        uint64_t x = rnd(4) ? 1 + rnd(63) : 0;
        if (variant == GATHER_SIDE_EXIT && i == 30)
            x = 7;
        memcpy(um_ram(index + 8 * i), &x, 8);
    }
    code_notify_vmm_write(UM_DATA, UM_DATA_SIZE);
    if (variant == GATHER_ROT)
        v->s.cfm = um_cfm(8, 8, 8, 3, 0);
    if (variant == GATHER_ROTP)
        v->s.cfm = um_cfm(8, 8, 8, 5, 45);
    v->s.rse_bof = variant == GATHER_ROT ? 90 : 7;
    um_gr(v, 32, index, false);
    um_gr(v, 33, table, false);
    um_gr(v, 34, out, false);
    um_gr(v, 35, 0x80000, false);
    um_gr(v, 36, variant == GATHER_SIDE_EXIT ? t[7] : UINT64_MAX, false);
    um_gr(v, 14, 0, false);
    um_gr(v, 15, 0, false);
    v->s.ar[AR_LC] = n - 1;
    if (variant == GATHER_VIRT_VRN1 || variant == GATHER_VIRT_VRN2)
        um_virtual_code(v);
    for (unsigned p = 6; p <= 20; ++p)
        um_pr(v, p, true);
    um_pr(v, 61, true);
    um_pr(v, 62, true);
    um_pr(v, 63, true);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    for (unsigned flags = 0; flags <= T1_COMPILE_VERIFY; flags += T1_COMPILE_VERIFY) {
        struct t1_compile_report rep;
        um_snapshot_restore(&snap, v);
        char label[96];
        snprintf(label, sizeof(label), "%s%s", names[variant], flags ? " verify" : "");
        uint32_t id = umt_compile(v, flags, 0, 0, &rep, label);
        UM_CHECK(id != 0);
        if (variant == GATHER_PHYS)
            dump_code(id, flags ? "gather-verify" : "gather");
        sweep(id, v, ref, &snap, flags, 26 * n + 60, flags ? 40 : 120, label);
        if (variant == GATHER_PHYS)
            printf(
                "unit_codegen: %s: %u insns, %u blocks, %u sites, %u host insns in the loop, spills "
                "loop/restore %u/%u, %zu code bytes\n",
                label, rep.insns, rep.blocks, rep.sites, rep.loop_host_insns, rep.loop_value_spills,
                rep.restore_only_spills, rep.code_bytes);
    }
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

void unit_codegen_test(const char *image);
/* ---- hand-built regions ---- */

#define PROG (UM_CODE + 0x1000)
static struct vcpu *program(const struct ienc_bundle *b, unsigned n, struct vcpu **ref)
{
    um_emit(PROG, b, n);
    /* The fall-off target spins; formation stops at the unreachable
     * boundary after it, not at older programs' code. */
    struct ienc_bundle tail[] = {{IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
                                 {IENC_MII, {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}}};
    um_emit(PROG + 16u * n, tail, 2);
    *ref = um_vcpu(PROG);
    return um_vcpu(PROG);
}

static uint32_t point_of_kind(uint32_t id, unsigned kind, unsigned nth)
{
    for (unsigned k = 0; k < t1_point_count(id); ++k) {
        struct t1_point_info pi;
        UM_CHECK(t1_point_get(id, k, &pi));
        if (pi.kind == kind && !nth--)
            return pi.point_id;
    }
    return UINT32_MAX;
}

/* Equal data operands do not imply non-NaT operands. Exercise each fold
 * with predication, entry guard rejection, and instruction budget exits. */
static void same_operands(void)
{
    static const enum ienc_alu ops[] = {IENC_SUB, IENC_XOR, IENC_ANDCM, IENC_AND, IENC_OR};
    static const char *names[] = {"sub", "xor", "andcm", "and", "or"};
    static const uint64_t values[] = {0, 1, UINT64_C(0x8000000000000000),
                                      UINT64_C(0x0123456789abcdef), UINT64_MAX};
    static const unsigned budgets[] = {1, 2, 3, 6, 12};
    for (unsigned k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
        for (unsigned predicated = 0; predicated < 2; ++predicated) {
            struct ienc_bundle b = {
                IENC_STOP(IENC_MII),
                {ienc_nop_m(), ienc_alu(predicated ? 6 : 0, ops[k], 20, 14, 14), ienc_nop_i()}};
            struct vcpu *ref, *v = program(&b, 1, &ref);
            um_gr(v, 14, values[3], false);
            um_gr(v, 20, 0xdeadbeef, false);
            um_pr(v, 6, true);
            struct um_snapshot snap;
            um_snapshot_take(&snap, v);
            for (unsigned flags = 0; flags <= T1_COMPILE_VERIFY; flags += T1_COMPILE_VERIFY) {
                um_snapshot_restore(&snap, v);
                struct t1_compile_report rep;
                char label[64];
                snprintf(label, sizeof(label), "same %s%s %s", names[k],
                         predicated ? " predicated" : "", flags ? "verify" : "production");
                uint32_t id = umt_compile(v, flags, 0, 0, &rep, label);
                UM_CHECK(id != 0);
                if (!predicated)
                    printf("unit_codegen: %s: %u host insns in the loop, %zu code bytes\n", label,
                           rep.loop_host_insns, rep.code_bytes);
                for (unsigned a = 0; a < sizeof(values) / sizeof(values[0]); ++a)
                    for (unsigned nat = 0; nat < 2; ++nat)
                        for (unsigned qp = 0; qp <= predicated; ++qp) {
                            um_gr(&snap.v, 14, values[a], nat != 0);
                            um_pr(&snap.v, 6, qp != 0);
                            for (unsigned j = 0; j < sizeof(budgets) / sizeof(budgets[0]); ++j) {
                                struct umt_run run;
                                umt_case(v, ref, &snap, flags, budgets[j], id, NULL, &run, false,
                                         label);
                                if (!nat && budgets[j] == 12)
                                    UM_CHECK(run.nexits > 0);
                                account(&run);
                            }
                        }
            }
            um_snapshot_free(&snap);
            umt_release(v);
            vcpu_destroy(v);
            vcpu_destroy(ref);
        }
}

/* NaT counterexample under generated code: nat[r20] = 1,
 * (p8) mov r20 = r21 ;; (p6) st8 [r34] = r20 with p8 = 0: the guard
 * rejects the entry and the lower tier raises Register NaT Consumption;
 * p8 = 1: still rejected; an unconditional write: the region runs and
 * nat[r20] materializes as 0. */
static void natguard_counterexample(void)
{
    for (unsigned variant = 0; variant < 3; ++variant) {
        unsigned qp = variant == 2 ? 0 : 8;
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MII), {ienc_adds(qp, 20, 0, 21), ienc_nop_i(), ienc_nop_i()}},
            {IENC_STOP(IENC_MMI), {ienc_st(6, 8, false, 34, 20), ienc_nop_m(), ienc_nop_i()}},
        };
        struct vcpu *ref, *v = program(b, 2, &ref);
        uint64_t out = UM_DATA + 0x100;
        memset(um_ram(out), 0x5a, 8);
        um_gr(v, 34, out, false);
        um_gr(v, 21, 0x1234, false);
        um_gr(v, 20, 77, true);
        um_pr(v, 8, variant == 1);
        um_pr(v, 6, true);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "natguard");
        UM_CHECK(id && rep.nat_guards >= 2);
        struct t1_stats before, after;
        t1_get_stats(&before);
        struct umt_run run;
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 20, id, NULL, &run, false,
                 "natguard counterexample");
        account(&run);
        t1_get_stats(&after);
        /* Exits of this region (hotness may compile the spin loop too). */
        unsigned mine = 0;
        struct t1_exit_info xi;
        for (unsigned k = 0; k < t1_exit_count(umt_tv(v)); ++k)
            if (t1_exit_get(umt_tv(v), k, &xi) && xi.region_id == id)
                ++mine;
        if (variant == 2) {
            UM_CHECK(mine >= 1 && !v->s.nat[20] && v->s.gr[20] == 0x1234);
        } else {
            UM_CHECK(mine == 0 && after.nat_guard_fails > before.nat_guard_fails);
            if (variant == 0)
                UM_CHECK(v->fault.kind == IA64_FAULT_NAT);
        }
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
}

/* SDM Vol.3 Table 2-58: tnat.z tests either entry NaT value without
 * consuming it. Test static and wrapped stacked homes, including a forced exit. */
static void tnat_entry_tests(void)
{
    for (unsigned stacked = 0; stacked < 2; ++stacked)
        for (unsigned consume = 0; consume < 2; ++consume)
            for (unsigned nat = 0; nat < 2; ++nat) {
                unsigned source = stacked ? 33 : 20;
                struct ienc_bundle b[] = {
                    {IENC_STOP(IENC_MII),
                     {ienc_nop_m(), ienc_tnat(0, 0, 0, 0, 6, 7, source), ienc_nop_i()}},
                    {IENC_STOP(IENC_MII),
                     {ienc_ld(0, 8, false, 22, source), ienc_nop_i(), ienc_nop_i()}},
                };
                struct vcpu *ref, *v = program(b, consume ? 2 : 1, &ref);
                v->s.rse_bof = 95;
                um_gr(v, source, UM_DATA + 0x200, nat != 0);
                struct um_snapshot snap;
                um_snapshot_take(&snap, v);
                struct t1_compile_report rep;
                uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "tnat entry");
                UM_CHECK(id && rep.nat_guards == consume);
                struct umt_run run;
                umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 12, id, NULL, &run, false, "tnat entry");
                account(&run);
                UM_CHECK(((v->s.pr >> 6) & 3) == (nat ? 2u : 1u) &&
                         ia64_nat_read(&v->s, source) == (nat != 0));
                if (consume && nat) {
                    /* A cached interruption-vector loop may run after the fault. */
                    unsigned mine = 0;
                    struct t1_exit_info xi;
                    for (unsigned k = 0; k < run.nexits; ++k)
                        if (t1_exit_get(umt_tv(v), k, &xi) && xi.region_id == id)
                            ++mine;
                    UM_CHECK(!mine && v->fault.kind == IA64_FAULT_NAT);
                } else {
                    UM_CHECK(run.nexits);
                    struct umt_arm arm = {.point = point_of_kind(id, T1_PT_HELPER, 0),
                                          .occurrence = 1,
                                          .action = T1_FORCE_HELPER_EXIT};
                    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 12, id, &arm, &run, false,
                             "tnat entry forced");
                    account(&run);
                    UM_CHECK(run.forced == 1);
                }
                um_snapshot_free(&snap);
                umt_release(v);
                vcpu_destroy(v);
                vcpu_destroy(ref);
            }
}

/* frame-rule under generated code: adds r21 = 0, r20 ;;
 * adds r20 = 1, r20 ;; tnat.z p6, p7 = r20 ;; adds r20 = 0, r21 -- the
 * helper wrote 11 to r20's home, the frames restore 10 (r20 is in W). */
#define FRAME_RULE_SEQ(q)                                                                          \
    {IENC_STOP(IENC_MII), {ienc_adds(q, 21, 0, 20), ienc_adds(0, 20, 1, 20), ienc_nop_i()}},       \
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_tnat(0, 0, 0, 0, 6, 7, 20), ienc_nop_i()}},      \
    {                                                                                              \
        IENC_STOP(IENC_MII),                                                                       \
        {                                                                                          \
            ienc_adds(0, 20, 0, 21), ienc_nop_i(), ienc_nop_i()                                    \
        }                                                                                          \
    }

static void frame_rule_tests(void)
{
    char name[96];
    struct umt_run run;
    /* Normal exit after the sequence; exit at a memory point after it. */
    for (unsigned variant = 0; variant < 2; ++variant) {
        struct ienc_bundle b[] = {
            FRAME_RULE_SEQ(0),
            {IENC_STOP(IENC_MMI),
             {variant ? ienc_ld(0, 8, false, 22, 33) : ienc_nop_m(), ienc_nop_m(), ienc_nop_i()}},
        };
        struct vcpu *ref, *v = program(b, 4, &ref);
        um_gr(v, 20, 10, false);
        um_gr(v, 33, UM_DATA + 0x200, false);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "frame-rule");
        UM_CHECK(id);
        snprintf(name, sizeof(name), "frame-rule %s", variant ? "memory point" : "exit");
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 30, id, NULL, &run, false, name);
        account(&run);
        UM_CHECK(run.nexits >= 1 && v->s.gr[20] == 10 && v->s.gr[21] == 10);
        if (variant) {
            for (unsigned a = 0; a < 3; ++a) {
                static const enum t1_force_action acts[] = {T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT,
                                                            T1_FORCE_TRAP_RETRY};
                struct umt_arm arm = {
                    .point = point_of_kind(id, T1_PT_SITE, 0), .occurrence = 1, .action = acts[a]};
                umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 30, id, &arm, &run, false,
                         "frame-rule forced memory point");
                account(&run);
                UM_CHECK(run.forced == 1);
                if (acts[a] != T1_FORCE_TRAP_RETRY)
                    UM_CHECK(run.run.ncaptures >= 1 && run.run.captures[0].v.s.gr[20] == 10);
            }
            /* The helper's own forced exit. */
            struct umt_arm arm = {.point = point_of_kind(id, T1_PT_HELPER, 0),
                                  .occurrence = 1,
                                  .action = T1_FORCE_HELPER_EXIT};
            umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 30, id, &arm, &run, false,
                     "frame-rule forced helper");
            account(&run);
            UM_CHECK(run.forced == 1);
        }
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    /* A diamond where only one branch runs the helper; the point follows
     * the join. */
    for (unsigned taken = 0; taken < 2; ++taken) {
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MIB),
             {ienc_cmp(0, IENC_EQ, false, false, 8, 9, 1, 0), ienc_nop_i(),
              ienc_br_cond(8, 4 * 16)}},
            FRAME_RULE_SEQ(0),
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 22, 33), ienc_nop_m(), ienc_nop_i()}},
        };
        struct vcpu *ref, *v = program(b, 5, &ref);
        um_gr(v, 1, taken ? 0 : 1, false);
        um_gr(v, 20, 10, false);
        um_gr(v, 33, UM_DATA + 0x200, false);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "frame-rule diamond");
        UM_CHECK(id);
        struct umt_arm arm = {.point = point_of_kind(id, T1_PT_SITE, 0),
                              .occurrence = 1,
                              .action = T1_FORCE_TRAP_EXIT};
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 30, id, &arm, &run, false, "frame-rule diamond");
        account(&run);
        UM_CHECK(run.forced == 1);
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 30, id, NULL, &run, false,
                 "frame-rule diamond whole");
        account(&run);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    /* A loop: the helper runs in iteration i, the point (the load at the
     * header) is reached in iteration i + 1 before it. */
    {
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 22, 33), ienc_nop_m(), ienc_nop_i()}},
            FRAME_RULE_SEQ(0),
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(-4 * 16)}},
        };
        struct vcpu *ref, *v = program(b, 5, &ref);
        um_gr(v, 20, 10, false);
        um_gr(v, 33, UM_DATA + 0x200, false);
        v->s.ar[AR_LC] = 4;
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "frame-rule loop");
        UM_CHECK(id);
        for (unsigned k = 1; k <= 3; ++k) {
            struct umt_arm arm = {.point = point_of_kind(id, T1_PT_SITE, 0),
                                  .occurrence = k,
                                  .action = T1_FORCE_TRAP_EXIT};
            umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 60, id, &arm, &run, false,
                     "frame-rule loop");
            account(&run);
            UM_CHECK(run.forced == 1 && run.run.ncaptures >= 1 &&
                     run.run.captures[0].v.s.gr[20] == 10);
        }
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
}

/* ---- random programs (subset restricted to what the unit harness needs) ---- */

static bool codegen_failure(const char *why)
{
    static const char *const ours[] = {"isel:",     "regalloc:",  "raverify:",
                                       "emit:",     "optimize:",  "frame tables",
                                       "no memory", "code arena", "lift"};
    for (unsigned k = 0; k < sizeof(ours) / sizeof(ours[0]); ++k)
        if (why && strstr(why, ours[k]))
            return true;
    return false;
}

/* Exits of the last run that left region id. */
static unsigned exits_from(struct vcpu *v, uint32_t id)
{
    unsigned n = 0;
    struct t1_exit_info xi;
    for (unsigned k = 0; k < t1_exit_count(umt_tv(v)); ++k)
        if (t1_exit_get(umt_tv(v), k, &xi) && xi.region_id == id)
            ++n;
    return n;
}

static uint64_t header_ip;
static unsigned header_compiles;
static void on_compile(const struct vcpu *v, enum t1_compile_status st,
                       const struct t1_compile_report *rep, void *arg)
{
    (void)rep;
    (void)arg;
    if (v->s.ip == header_ip && st == T1_COMPILED)
        ++header_compiles;
}

static void random_programs(unsigned count)
{
    unsigned compiled = 0, entered = 0, forced_runs = 0, rejected = 0;
    unsigned rebased = 0, rebased_wrap = 0;
    unsigned helpers = 0, memory_ops = 0;
    char name[96];
    for (unsigned seed = 0; seed < count; ++seed) {
        struct vcpu *v, *ref;
        struct um_random_info info;
        um_random_program(seed, &v, &ref, &info);
        helpers += info.helpers;
        memory_ops += info.memory_ops;
        static const uint32_t flag_sets[] = {0, T1_COMPILE_VERIFY,
                                             T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG};
        uint32_t flags = flag_sets[seed % 3];
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct t1_compile_report rep;
        struct t1_compile_options opt = {flags, 0, 0};
        enum t1_compile_status st = t1_compile(umt_tv(v), &opt, &rep);
        if (st != T1_COMPILED) {
            if (codegen_failure(rep.reject_reason)) {
                fprintf(stderr, "random program %u: %s\n", seed, rep.reject_reason);
                UM_CHECK(!"random program compiles");
            }
            ++rejected;
            um_snapshot_free(&snap);
            umt_release(v);
            vcpu_destroy(v);
            vcpu_destroy(ref);
            continue;
        }
        ++compiled;
        uint64_t budget = 100 + rnd(300);
        struct umt_run run;
        snprintf(name, sizeof(name), "random %u", seed);
        umt_case(v, ref, &snap, flags, budget, rep.region_id, NULL, &run, false, name);
        account(&run);
        entered += run.nexits > 0;
        if ((flags & T1_COMPILE_VERIFY) && run.nexits) {
            uint32_t points[T1_MAX_POINTS];
            uint8_t kinds[T1_MAX_POINTS];
            unsigned n = 0;
            for (unsigned k = 0; k < t1_point_count(rep.region_id); ++k) {
                struct t1_point_info pi;
                UM_CHECK(t1_point_get(rep.region_id, k, &pi));
                if (pi.kind == T1_PT_SITE || pi.kind == T1_PT_HELPER) {
                    kinds[n] = (uint8_t)pi.kind;
                    points[n++] = pi.point_id;
                }
            }
            if (n) {
                unsigned pick = rnd(n);
                static const enum t1_force_action site_actions[] = {
                    T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT, T1_FORCE_TRAP_RETRY};
                struct umt_arm arm = {.point = points[pick],
                                      .occurrence = 1 + rnd(3),
                                      .action = kinds[pick] == T1_PT_SITE ? site_actions[rnd(3)]
                                                                          : T1_FORCE_HELPER_EXIT};
                snprintf(name, sizeof(name), "random %u force P%u #%llu", seed, arm.point,
                         (unsigned long long)arm.occurrence);
                umt_case(v, ref, &snap, flags, budget, rep.region_id, &arm, &run, false, name);
                account(&run);
                forced_runs += run.forced > 0;
            }
        }
        snprintf(name, sizeof(name), "random %u split", seed);
        umt_case(v, ref, &snap, flags, rnd((unsigned)budget + 1), rep.region_id, NULL, &run, false,
                 name);
        account(&run);
        /* The code addresses no stacked home, and the
         * region runs at another frame base: the snapshot rebased (half the
         * time so that the frame wraps), compared with interp_run from the
         * rebased state, the same exits of this region, no compile at the
         * header. */
        unsigned image;
        snprintf(name, sizeof(name), "random %u code", seed);
        const uint8_t *code;
        size_t size, ls, le;
        UM_CHECK(t1_test_region_code(rep.region_id, &code, &size, &ls, &le) &&
                 umt_no_stacked_home(code, size, name, &image));
        /* A fresh Tier1 vCPU: an arm of the forced case above that did not
         * fire would fire in a later run (arms survive restores). */
        umt_release(v);
        umt_case(v, ref, &snap, flags, budget, rep.region_id, NULL, &run, false,
                 "random rebase control");
        unsigned exits = exits_from(v, rep.region_id);
        unsigned sof = (unsigned)(snap.v.s.cfm & 127), from = snap.v.s.rse_bof;
        unsigned bof = sof > 1 && rnd(2) ? 95 - rnd(sof - 1) : rnd(96);
        if (bof == from)
            bof = (bof + 1) % 96;
        struct um_snapshot moved;
        um_snapshot_restore(&snap, v);
        t1t_rebase_bof(&v->s, bof);
        um_snapshot_take(&moved, v);
        header_ip = snap.v.s.ip;
        header_compiles = 0;
        t1_test_set_compile_observer(on_compile, NULL);
        snprintf(name, sizeof(name), "random %u at BOF %u (from %u, sof %u)", seed, bof, from, sof);
        umt_case(v, ref, &moved, flags, budget, rep.region_id, NULL, &run, false, name);
        t1_test_set_compile_observer(NULL, NULL);
        account(&run);
        if (exits_from(v, rep.region_id) != exits || header_compiles) {
            fprintf(stderr, "%s: %u exits of region %u (%u at BOF %u), %u compiles at the header\n",
                    name, exits_from(v, rep.region_id), rep.region_id, exits, from,
                    header_compiles);
            UM_CHECK(!"the region at another frame base");
        }
        rebased += exits > 0;
        rebased_wrap += exits > 0 && bof + sof > 96;
        um_snapshot_free(&moved);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    printf(
        "unit_codegen: %u random programs (%u memory ops, %u helpers): %u compiled, %u rejected by "
        "formation, %u entered, %u forced runs\n",
        count, memory_ops, helpers, compiled, rejected, entered, forced_runs);
    printf(
        "unit_codegen: no stacked home in %u regions; %u regions entered again at another BOF (%u with a "
        "wrapping frame) with the same exits and no compile\n",
        compiled, rebased, rebased_wrap);
    UM_CHECK(compiled > count * 3 / 4 && entered > count / 2);
    UM_CHECK(rebased * 2 > entered && rebased_wrap * 4 > rebased);
}

/* Constant data results must preserve predication, NaT and precise
 * retirement even when the variable data/count operand is eliminated. */
static void absorbing_constants(void)
{
    static const char *names[] = {"and-right",   "and-left", "or-right", "or-left",  "andcm-left",
                                  "andcm-right", "shl-zero", "shr-zero", "shru-zero"};
    static const uint64_t values[] = {0, 1, 63, 64, 65, UINT64_C(0x8000000000000000), UINT64_MAX};
    static const unsigned budgets[] = {1, 3, 6, 9, 18};
    for (unsigned k = 0; k < sizeof(names) / sizeof(names[0]); ++k)
        for (unsigned predicated = 0; predicated < 2; ++predicated) {
            unsigned qp = predicated ? 6 : 0;
            uint64_t insns[] = {
                ienc_alu(qp, IENC_AND, 20, 14, 0),
                ienc_alu(qp, IENC_AND, 20, 0, 14),
                ienc_alu(qp, IENC_OR, 20, 14, 15),
                ienc_alu(qp, IENC_OR, 20, 15, 14),
                ienc_alu(qp, IENC_ANDCM, 20, 0, 14),
                ienc_alu(qp, IENC_ANDCM, 20, 14, 15),
                ienc_shl(qp, 20, 0, 14),
                ienc_shr(qp, 20, 0, 14, false),
                ienc_shr(qp, 20, 0, 14, true),
            };
            struct ienc_bundle b[] = {
                {IENC_STOP(IENC_MII), {ienc_adds(0, 15, -1, 0), ienc_nop_i(), ienc_nop_i()}},
                {IENC_STOP(IENC_MII), {ienc_nop_m(), insns[k], ienc_nop_i()}},
            };
            struct vcpu *ref, *v = program(b, 2, &ref);
            um_gr(v, 14, values[4], false);
            um_gr(v, 20, 0xdeadbeef, false);
            um_pr(v, 6, true);
            struct um_snapshot snap;
            um_snapshot_take(&snap, v);
            for (unsigned flags = 0; flags <= T1_COMPILE_VERIFY; flags += T1_COMPILE_VERIFY) {
                um_snapshot_restore(&snap, v);
                struct t1_compile_report rep;
                char label[80];
                snprintf(label, sizeof(label), "absorbing %s%s %s", names[k],
                         predicated ? " predicated" : "", flags ? "verify" : "production");
                uint32_t id = umt_compile(v, flags, 0, 0, &rep, label);
                UM_CHECK(id != 0);
                if (!predicated)
                    printf("unit_codegen: %s: %zu code bytes\n", label, rep.code_bytes);
                for (unsigned a = 0; a < sizeof(values) / sizeof(values[0]); ++a)
                    for (unsigned nat = 0; nat < 2; ++nat)
                        for (unsigned p = 0; p <= predicated; ++p) {
                            um_gr(&snap.v, 14, values[a], nat != 0);
                            um_pr(&snap.v, 6, p != 0);
                            for (unsigned j = 0; j < sizeof(budgets) / sizeof(budgets[0]); ++j) {
                                struct umt_run run;
                                umt_case(v, ref, &snap, flags, budgets[j], id, NULL, &run, false,
                                         label);
                                if (!nat && budgets[j] == 18)
                                    UM_CHECK(run.nexits > 0);
                                account(&run);
                            }
                        }
            }
            um_snapshot_free(&snap);
            umt_release(v);
            vcpu_destroy(v);
            vcpu_destroy(ref);
        }
}

/* An absorbed load result must still execute the access. Sweep forced
 * misses, traps, retries and budget exits with the real capture oracle. */
static void absorbing_constant_load(void)
{
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MII), {ienc_adds(0, 20, 1, 20), ienc_nop_i(), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {ienc_ld(0, 8, false, 20, 14), ienc_nop_i(), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {ienc_alu(0, IENC_AND, 20, 20, 0), ienc_nop_i(), ienc_nop_i()}},
    };
    struct vcpu *ref, *v = program(b, 3, &ref);
    um_gr(v, 14, UM_DATA, false);
    um_gr(v, 20, 0x12345678, false);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "absorbing load");
    UM_CHECK(id != 0 && point_of_kind(id, T1_PT_SITE, 0) != UINT32_MAX);
    sweep(id, v, ref, &snap, T1_COMPILE_VERIFY, 24, 12, "absorbing load");
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

/* SDM Vol.3 br.call B5 reads the target BR before writing the link and
 * aligns it to a bundle. A failed target guard resumes at the call with
 * the caller's frame and all earlier instructions already retired. */
static void indirect_calls(void)
{
    for (unsigned alias = 0; alias < 2; ++alias)
        for (unsigned write = 0; write < 2; ++write) {
            unsigned source = alias ? 0 : 2;
            struct ienc_bundle b[] = {
                {IENC_STOP(IENC_MII),
                 {ienc_nop_m(), write ? ienc_mov_to_br(0, source, 18) : ienc_nop_i(),
                  ienc_adds(0, 14, 1, 14)}},
                {IENC_STOP(IENC_MIB),
                 {ienc_nop_m(), ienc_adds(0, 15, 1, 15), ienc_br_call_indirect(0, 0, source)}},
                {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_adds(0, 16, 1, 16), ienc_nop_i()}},
                {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
                {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}},
                {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}},
                {IENC_STOP(IENC_MII),
                 {ienc_ld(0, 8, false, 32, 17), ienc_adds(0, 33, 1, 33), ienc_nop_i()}},
                {IENC_STOP(IENC_MIB),
                 {ienc_nop_m(), ienc_tnat(0, 0, 0, 0, 6, 7, 33), ienc_br_ret(0, 0)}},
                {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_adds(0, 20, 1, 20), ienc_br_ret(0, 0)}},
            };
            struct vcpu *ref, *v = program(b, 9, &ref);
            v->s.cfm = um_cfm(6, 3, 0, 0, 0);
            v->s.rse_bof = 95;
            v->s.ar[AR_EC] = 40;
            v->s.br[source] = (PROG + 6 * 16) | 15;
            um_gr(v, 17, UM_DATA, false);
            um_gr(v, 18, (PROG + 6 * 16) | 3, false);
            struct um_snapshot snap;
            um_snapshot_take(&snap, v);
            for (unsigned flags = 0; flags <= T1_COMPILE_VERIFY; flags += T1_COMPILE_VERIFY) {
                um_snapshot_restore(&snap, v);
                uint32_t id = umt_compile(v, flags, 0, 0, NULL, "indirect call");
                UM_CHECK(id && point_of_kind(id, T1_PT_SITE, 0) != UINT32_MAX);
                if (flags & T1_COMPILE_VERIFY) {
                    struct umt_arm arm = {.point = point_of_kind(id, T1_PT_HELPER, 0),
                                          .occurrence = 1,
                                          .action = T1_FORCE_HELPER_EXIT};
                    struct umt_run forced;
                    umt_case(v, ref, &snap, flags, 48, id, &arm, &forced, false,
                             "indirect callee restoration");
                    account(&forced);
                    UM_CHECK(forced.forced == 1);
                    for (unsigned k = 0; k < forced.run.ncaptures; ++k) {
                        const struct um_capture *cap = &forced.run.captures[k];
                        if (!cap->forced)
                            continue;
                        UM_CHECK(cap->v.s.ip == PROG + 7 * 16 && cap->v.s.slot == 1 &&
                                 cap->v.s.cfm == 3 && cap->v.s.rse_bof == 2);
                    }
                }
                struct um_snapshot changed = snap;
                if (write)
                    um_gr(&changed.v, 18, (PROG + 8 * 16) | 7, false);
                else
                    changed.v.s.br[source] = (PROG + 8 * 16) | 7;
                struct umt_run run;
                umt_case(v, ref, &changed, flags, 48, id, NULL, &run, false,
                         "changed indirect target");
                account(&run);
                bool guard = false;
                for (unsigned k = 0; k < run.run.ncaptures; ++k) {
                    const struct vcpu *at = &run.run.captures[k].v;
                    if (at->s.ip != PROG + 16 || at->s.slot != 2)
                        continue;
                    UM_CHECK(at->retired == snap.v.retired + 5 && at->s.cfm == snap.v.s.cfm &&
                             at->s.ar[AR_BSP] == snap.v.s.ar[AR_BSP] &&
                             at->s.ar[AR_PFS] == snap.v.s.ar[AR_PFS] &&
                             at->s.br[source] == ((PROG + 8 * 16) | 7));
                    guard = true;
                }
                if (flags & T1_COMPILE_VERIFY)
                    UM_CHECK(guard);
                sweep(id, v, ref, &snap, flags, 48, 24, "indirect call");
            }
            um_snapshot_free(&snap);
            umt_release(v);
            vcpu_destroy(v);
            vcpu_destroy(ref);
        }
}

/* Zero-locals returns change CFM and EC without moving the register stack.
 * Larger caller frames expose outputs without filling; write them before
 * reading. Different rotations rename r32/r33 across the return. */
static void observed_returns(void)
{
    static const unsigned caller_sof[] = {8, 24, 96};
    for (unsigned size = 0; size < sizeof(caller_sof) / sizeof(caller_sof[0]); ++size)
        for (unsigned head = 0; head < 2; ++head)
            for (unsigned write = 0; write < 2; ++write) {
                unsigned sof = caller_sof[size], dirty = 96 - (sof > 16 ? sof : 16);
                uint64_t caller = um_cfm(sof, 0, 8, 3, 0);
                unsigned output = size ? 31 + sof : 32;
                struct ienc_bundle b[] = {
                    {IENC_STOP(IENC_MII),
                     {ienc_nop_m(), write ? ienc_mov_to_ar(0, AR_PFS, 18) : ienc_nop_i(),
                      write ? ienc_mov_to_br(0, 2, 19) : ienc_nop_i()}},
                    {IENC_STOP(IENC_MIB),
                     {ienc_nop_m(), ienc_adds(0, 33, 1, 33), ienc_br_ret(0, 2)}},
                    {IENC_STOP(IENC_MII),
                     {ienc_ld(0, 8, false, output, 17), ienc_adds(0, 33, 1, 33),
                      ienc_adds(0, 20, 0, 34)}},
                    {IENC_STOP(IENC_MIB),
                     {ienc_nop_m(), ienc_adds(0, output, 1, output), ienc_br_cond(0, 0)}},
                };
                struct vcpu *ref, *v = program(b, 4, &ref);
                v->s.cfm = um_cfm(16, 4, 8, 7, 0);
                v->s.rse_bof = 95;
                if (size) {
                    unsigned physical = 32 + (95 + sof - 1) % 96;
                    v->s.gr[physical] = UINT64_MAX;
                    v->s.nat[physical] = true;
                }
                /* These stay allocated: rotations map callee r37/r38 to
             * caller r33/r34, preserving their NaT and value. */
                um_gr(v, 37, 0x12345, true);
                um_gr(v, 38, 0xabcdef, false);
                v->s.rse_dirty = dirty;
                v->s.ar[AR_BSP] = UM_DATA + 0x1f8;
                v->s.ar[AR_BSPSTORE] = UM_DATA;
                v->s.psr = (v->s.psr & ~(UINT64_C(3) << 32)) | (UINT64_C(1) << 32);
                v->s.ar[AR_EC] = 9;
                v->s.ar[AR_PFS] = caller | (UINT64_C(41) << 52);
                v->s.br[2] = (PROG + 32) | 15;
                um_gr(v, 17, UM_DATA + 0x1000, false);
                um_gr(v, 18, v->s.ar[AR_PFS], false);
                um_gr(v, 19, v->s.br[2], false);
                if (head) {
                    v->s.ip += 16;
                    v->s.slot = 2;
                }
                struct um_snapshot snap;
                um_snapshot_take(&snap, v);
                for (unsigned flags = 0; flags <= T1_COMPILE_VERIFY; flags += T1_COMPILE_VERIFY) {
                    um_snapshot_restore(&snap, v);
                    uint32_t id = umt_compile(v, flags, 0, 0, NULL, "observed return");
                    UM_CHECK(id && point_of_kind(id, T1_PT_SITE, 0) != UINT32_MAX);
                    sweep(id, v, ref, &snap, flags, 36, 18, "observed return restoration");
                    for (unsigned change = 0; change < 5; ++change) {
                        struct um_snapshot changed = snap;
                        uint64_t pfs = snap.v.s.ar[AR_PFS], br = snap.v.s.br[2];
                        if (change == 0)
                            pfs = (pfs & ~UINT64_C(127)) | 9;
                        if (change == 1)
                            br = (PROG + 48) | 3;
                        if (change == 2)
                            pfs |= UINT64_C(2) << 62;
                        if (change == 3)
                            pfs |= UINT64_C(1) << 62;
                        if (change == 4)
                            pfs ^= UINT64_C(63) << 52;
                        if (write && !head) {
                            um_gr(&changed.v, 18, pfs, false);
                            um_gr(&changed.v, 19, br, false);
                        } else {
                            changed.v.s.ar[AR_PFS] = pfs;
                            changed.v.s.br[2] = br;
                        }
                        struct umt_run run;
                        umt_case(v, ref, &changed, flags, 36, id, NULL, &run, false,
                                 "changed return state");
                        account(&run);
                        bool guard = false, returned = false;
                        for (unsigned k = 0; k < run.run.ncaptures; ++k) {
                            const struct vcpu *at = &run.run.captures[k].v;
                            if (at->s.ip == PROG + 16 && at->s.slot == 2) {
                                UM_CHECK(at->retired == snap.v.retired + (head ? 0 : 5) &&
                                         at->s.cfm == snap.v.s.cfm && at->s.ar[AR_EC] == 9);
                                guard = true;
                            }
                            if (at->s.cfm == caller) {
                                UM_CHECK(at->s.rse_bof == 95 && at->s.rse_dirty == dirty &&
                                         at->s.ar[AR_BSP] == snap.v.s.ar[AR_BSP] &&
                                         at->s.ar[AR_BSPSTORE] == snap.v.s.ar[AR_BSPSTORE] &&
                                         at->s.ar[AR_RNAT] == snap.v.s.ar[AR_RNAT] &&
                                         at->s.ar[AR_PFS] == pfs && !at->s.rse_cfle &&
                                         !at->s.rse_missing &&
                                         at->s.ar[AR_EC] == ((pfs >> 52) & 63));
                                returned = true;
                            }
                        }
                        if (flags && change < 3)
                            UM_CHECK(guard);
                        if (flags && change >= 3) {
                            UM_CHECK(returned && !guard && v->s.gr[20] == 0xabcdef &&
                                     !v->s.nat[20] && ia64_nat_read(&v->s, 33));
                            if (size)
                                UM_CHECK(!v->s.nat[32 + (95 + sof - 1) % 96]);
                        }
                    }
                    struct um_snapshot overflow = snap;
                    overflow.v.s.rse_dirty = 97 - sof;
                    struct umt_run lower;
                    umt_case(v, ref, &overflow, flags, 36, id, NULL, &lower, false,
                             "return output overflow");
                    account(&lower);
                    /* Lower-tier progress can reach another compiled head. */
                    struct t1_exit_info xi;
                    for (unsigned k = 0; k < lower.nexits; ++k)
                        if (t1_exit_get(umt_tv(v), k, &xi))
                            UM_CHECK(xi.region_id != id);
                    /* Product entry must reject overflowing caller outputs even
                 * when a previously compiled region and fetch link exist. */
                    um_snapshot_restore(&snap, v);
                    v->upper = umt_tv(v);
                    v->cfg.engine = ENGINE_TIER1;
                    uint64_t link = t1_upper_probe(v, v->s.ip);
                    UM_CHECK(link);
                    v->s.rse_dirty = 97 - sof;
                    struct ia64_state before = v->s;
                    struct vcpu_exit out;
                    UM_CHECK(t1_upper_enter(v, &link, 36, &out) == T1_UPPER_DECLINED &&
                             v->retired == snap.v.retired &&
                             !memcmp(&before, &v->s, sizeof(before)));
                    v->s.rse_dirty = dirty;
                    if (head) {
                        v->s.br[2] = PROG + 48;
                        before = v->s;
                        UM_CHECK(t1_upper_enter(v, &link, 36, &out) == T1_UPPER_RAN_PENDING &&
                                 v->retired == snap.v.retired &&
                                 !memcmp(&before, &v->s, sizeof(before)));
                    }
                    v->upper = NULL;
                    v->cfg.engine = ENGINE_INTERP;
                }
                um_snapshot_free(&snap);
                umt_release(v);
                vcpu_destroy(v);
                vcpu_destroy(ref);
            }
}

extern t1_ir_out_fn t1_test_mir_print;
static void print_line(void *arg, const char *text)
{
    (void)arg;
    fprintf(stderr, "%s\n", text);
}

void unit_codegen_test(const char *image)
{
    umt_init();
    if (getenv("T1_UNIT_MIR_DUMP"))
        t1_test_mir_print = print_line;
    shladd_xor(image);
    gather(image, GATHER_PHYS);
    gather(image, GATHER_ROT);
    gather(image, GATHER_ROTP);
    gather(image, GATHER_SIDE_EXIT);
    gather(image, GATHER_ALIAS63);
    gather(image, GATHER_VIRT_VRN1);
    gather(image, GATHER_VIRT_VRN2);
    same_operands();
    natguard_counterexample();
    tnat_entry_tests();
    frame_rule_tests();
    absorbing_constants();
    absorbing_constant_load();
    indirect_calls();
    observed_returns();
    random_programs(getenv("T1_UNIT_RANDOM_COUNT") ? (unsigned)atoi(getenv("T1_UNIT_RANDOM_COUNT"))
                                                   : 1000);
    printf(
        "unit_codegen: %u runs equal to interp_run; %u in-place captures (%u forced), %u retries, "
        "%u region exits\n",
        T.cases, T.captures, T.forced, T.retries, T.exits);
    (void)rnd;
}
