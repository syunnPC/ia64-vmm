/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tests/tier1/engines.h"
#include "tests/tier1/engine_tier1.h"

/* SDM Vol.3 M32/M33, M29/M31 and I26/I28. Decode real bundles through
 * every engine, including Tier1's architectural lower-tier boundaries. */
static uint64_t rsv_mov(bool cr, bool write, unsigned index, unsigned gr)
{
    unsigned x6 = cr ? (write ? 0x2c : 0x24) :
        write ? 0x2a : index < 64 ? 0x22 : 0x32;
    return ((cr || index < 64) ? UINT64_C(1) << 37 : 0) |
        (uint64_t)x6 << 27 | (uint64_t)index << 20 |
        (uint64_t)gr << (write ? 13 : 6);
}
static void rsv_code(bool cr, bool write, unsigned index)
{
    struct ienc_bundle b = {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_nop_i(), ienc_nop_i()}};
    b.slot[cr || index < 64 ? 0 : 1] = rsv_mov(cr, write, index, 8);
    um_emit(PROG, &b, 1);
}
static struct vcpu_exit rsv_lockstep(void *ctx, struct vcpu *v, uint64_t budget)
{
    (void)ctx;
    v->cfg.engine = ENGINE_LOCKSTEP;
    return engine_run(v, budget);
}
static struct vcpu_exit rsv_run(struct t1h_engine_vcpu *ev, uint64_t budget)
{
    ev->v->fault = (struct ia64_fault_site){0};
    return t1h_engine_run(ev, budget);
}
/* The hot loop crosses a CR boundary on every iteration. Compile its
 * pure prefix explicitly (the runtime harness disables automatic hotness)
 * and prove native entries materialize the value consumed by mov cr. */
static void rsv_hot_boundary(const struct t1h_engine *engine)
{
    const uint64_t prog = PROG + 0x200;
    const unsigned loops = 128;
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_adds(0, 8, 1, 8), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {rsv_mov(true, true, 9, 8), ienc_nop_i(), ienc_nop_i()}},
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(-32)}},
        {IENC_STOP(IENC_MII), {rsv_mov(true, false, 9, 9), ienc_nop_i(), ienc_nop_i()}},
    };
    um_emit(prog, b, 4);
    struct vcpu *v = um_vcpu(prog);
    struct t1h_engine_vcpu ev = {0};
    t1h_engine_bind(&ev, engine, v);
    v->s.ar[AR_LC] = loops - 1; um_gr(v, 8, 0, false);
    uint32_t region = 0;
    if (!strcmp(engine->name, "tier1") || !strcmp(engine->name, "tier0+tier1")) {
        struct t1_compile_report report;
        struct t1_compile_options options = {.flags = T1_COMPILE_VERIFY};
        CHECK(t1_compile(t1h_tier1_vcpu(ev.ctx), &options, &report) == T1_COMPILED);
        CHECK(report.end_ip == prog + 16 && report.end_slot == 0);
        region = report.region_id;
        CHECK(region && t1_region_get(region)->entries == 0);
    }
    CHECK(rsv_run(&ev, loops * 9 + 1).reason == VCPU_EXIT_BUDGET);
    CHECK(v->s.cr[9] == loops && v->s.gr[9] == loops && !v->s.nat[9]);
    CHECK(v->s.ip == prog + 48 && v->s.slot == 1);
    if (region) CHECK(t1_region_get(region)->entries > 1);
    t1h_engine_release(&ev); vcpu_destroy(v);
}
static void t_reserved(void)
{
    const unsigned crs[] = {3, 4, 7, 9, 15, 18, 26, 27, 63, 75, 79, 82, 127};
    const unsigned ars[] = {8, 15, 20, 22, 23, 31, 33, 35, 37, 39, 41, 43, 46, 47, 67, 111};
    static const struct t1h_engine lockstep = {.name = "lockstep", .run = rsv_lockstep};
    for (unsigned e = 0; e <= t1h_engine_count(); ++e) {
        struct vcpu *v = um_vcpu(PROG), *other = um_vcpu(PROG);
        struct t1h_engine_vcpu ev = {0};
        t1h_engine_bind(&ev, e == t1h_engine_count() ? &lockstep : t1h_engine_at(e), v);
        for (unsigned bank = 0; bank < 2; ++bank) {
            bool cr = bank == 0;
            const unsigned *indices = cr ? crs : ars;
            unsigned n = cr ? sizeof(crs) / sizeof(crs[0]) : sizeof(ars) / sizeof(ars[0]);
            for (unsigned k = 0; k < n; ++k) {
                unsigned index = indices[k];
                uint64_t value = UINT64_C(0xfedcba9876543210) ^ index;
                CHECK((cr ? v->s.cr[index] : v->s.ar[index]) == 0);
                for (unsigned write = 0; write < 2; ++write) {
                    rsv_code(cr, write, index);
                    v->s.ip = PROG; v->s.slot = cr || index < 64 ? 0 : 1;
                    v->s.psr = IA64_PSR_IC | (cr ? 0 : UINT64_C(3) << 32);
                    um_gr(v, 8, value, !write);
                    CHECK(rsv_run(&ev, 1).reason == VCPU_EXIT_BUDGET);
                    CHECK(write ? (cr ? v->s.cr[index] : v->s.ar[index]) == value :
                          v->s.gr[8] == 0 && !v->s.nat[8]);
                }
                rsv_code(cr, false, index);
                v->s.ip = PROG; v->s.slot = cr || index < 64 ? 0 : 1;
                CHECK(rsv_run(&ev, 1).reason == VCPU_EXIT_BUDGET);
                CHECK(v->s.gr[8] == value && !v->s.nat[8]);
                CHECK((cr ? other->s.cr[index] : other->s.ar[index]) == 0);
                /* NaT write: no register changes before the fault. */
                rsv_code(cr, true, index);
                v->s.ip = PROG; v->s.slot = cr || index < 64 ? 0 : 1;
                v->s.psr = IA64_PSR_IC; um_gr(v, 8, ~value, true);
                CHECK(rsv_run(&ev, 1).reason == VCPU_EXIT_BUDGET);
                CHECK(v->fault.kind == IA64_FAULT_NAT);
                CHECK((cr ? v->s.cr[index] : v->s.ar[index]) == value);
                if (cr) {
                    for (unsigned write = 0; write < 2; ++write) {
                        rsv_code(true, write, index);
                        v->s.ip = PROG; v->s.slot = 0;
                        v->s.psr = IA64_PSR_IC | (UINT64_C(3) << 32);
                        um_gr(v, 8, ~value, false);
                        CHECK(rsv_run(&ev, 1).reason == VCPU_EXIT_BUDGET);
                        CHECK(v->fault.kind == IA64_FAULT_PRIVILEGED);
                        CHECK(v->s.cr[index] == value);
                    }
                }
            }
        }
        /* Defined CR2 keeps its alignment semantics. */
        rsv_code(true, true, CR_IVA); v->s.ip = PROG; v->s.slot = 0;
        v->s.psr = IA64_PSR_IC; um_gr(v, 8, UM_IVT + 7, false);
        CHECK(rsv_run(&ev, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.cr[CR_IVA] == UM_IVT);
        /* Ignored ARs accept writes but read zero in both unit forms. */
        const unsigned ignored[] = {48, 63, 112, 127};
        for (unsigned k = 0; k < sizeof(ignored) / sizeof(ignored[0]); ++k) {
            rsv_code(false, true, ignored[k]); v->s.ip = PROG;
            v->s.slot = ignored[k] < 64 ? 0 : 1; um_gr(v, 8, UINT64_MAX, false);
            CHECK(rsv_run(&ev, 1).reason == VCPU_EXIT_BUDGET);
            rsv_code(false, false, ignored[k]); v->s.ip = PROG;
            v->s.slot = ignored[k] < 64 ? 0 : 1;
            CHECK(rsv_run(&ev, 1).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == 0);
        }
        /* AR45 is illegal in either unit; AR64..111 is I-only. */
        for (unsigned index = 45; index <= 111; ++index) {
            if (index > 45 && index < 64) continue;
            for (unsigned unit = 0; unit < (index == 45 ? 2u : 1u); ++unit)
                for (unsigned write = 0; write < 2; ++write) {
                    struct ienc_bundle b = {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_nop_i(), ienc_nop_i()}};
                    b.slot[unit] = unit ? ((uint64_t)(write ? 0x2a : 0x32) << 27) |
                        ((uint64_t)index << 20) | (UINT64_C(8) << (write ? 13 : 6)) :
                        (UINT64_C(1) << 37) | ((uint64_t)(write ? 0x2a : 0x22) << 27) |
                        ((uint64_t)index << 20) | (UINT64_C(8) << (write ? 13 : 6));
                    um_emit(PROG, &b, 1);
                    v->s.ip = PROG; v->s.slot = unit; v->s.psr = IA64_PSR_IC;
                    uint64_t old = v->s.ar[index]; um_gr(v, 8, UINT64_MAX, false);
                    CHECK(rsv_run(&ev, 1).reason == VCPU_EXIT_BUDGET);
                    CHECK(v->fault.kind == IA64_FAULT_ILLEGAL && v->fault.ip == PROG);
                    CHECK(v->s.ar[index] == old);
                }
        }
        const struct t1h_engine *engine = ev.e;
        t1h_engine_release(&ev); vcpu_destroy(v); vcpu_destroy(other);
        rsv_hot_boundary(engine);
    }
    printf("unit_runtime: reserved CR/AR storage, CPL, NaT and ignored ARs across all engines\n");
}
