/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real vCPUs/TCs and routes, fake executors on the threaded HAL.
 * Round barriers keep applying purges: even a finished issuer must serve
 * peers until every instruction in the round has completed. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include "core/smp.h"
#include "core/log_internal.h"
#include "tests/hal/hal_linux.h"
#include "ia64/smp.h"
#include "core/vm.h"
#include "engine/tier0/internal.h"
#include "ia64/tlb.h"
#include "ia64/mmu.h"
#include "ia64/memory.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/interrupts.h"
#include "platform/platform.h"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            _exit(1);                                                                              \
        }                                                                                          \
    } while (0)
#include "watchdog.h"

#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define STORE(p, x) __atomic_store_n(p, x, __ATOMIC_RELEASE)
#define VA UINT64_C(0x2000000000100000)
#define RID 0x123u
static struct platform_state platform;
static struct vm *domain;
static unsigned rounds = 1000, arrived, generation, finished, go;
static uint64_t hook_count, hook_all, elapsed[8], worst[8];
static unsigned park_ready, park_release, park_done, park_idle;
static unsigned diagnostic_lines, diagnostic_ack;
static bool serial;
static unsigned race_gate, race_unparked, race_release;

/* Called only by mutation binaries with an injected scheduling boundary. */
void smp_test_post_boundary(struct vcpu *v)
{
    if (v->cfg.index || !LOAD(&go))
        return;
    STORE(&race_gate, 1);
    while (!LOAD(&race_unparked))
        __builtin_ia32_pause();
}

void __real_ia64_smp_purge_hook(struct vcpu *, uint32_t, uint64_t, unsigned, bool);
void __wrap_ia64_smp_purge_hook(struct vcpu *v, uint32_t rid, uint64_t va, unsigned shift, bool all)
{
    __real_ia64_smp_purge_hook(v, rid, va, shift, all);
    CHECK(vm_cpu_of(v));
    if (all) {
        CHECK(!rid && !va && !shift);
        __atomic_fetch_add(&hook_all, 1, __ATOMIC_RELAXED);
    } else {
        CHECK(rid == RID && va >= VA && shift == 12);
        __atomic_fetch_add(&hook_count, 1, __ATOMIC_RELAXED);
    }
}
static unsigned delivery, issuer_entered, peer_entered, peer_prelock, peer_queued, reacquiring;
static unsigned delivered, issuer_done, issuer_boundary, in_purge;
static unsigned calls[2];
struct vcpu_exit __real_engine_run(struct vcpu *, uint64_t);
void __real_hal_idle_until(uint64_t);
void __wrap_hal_idle_until(uint64_t deadline)
{
    if (delivery == 2 && hal_cpu_index() == 2 && LOAD(&peer_prelock))
        STORE(&peer_queued, 1);
    if (delivery == 2 && hal_cpu_index() == 1 && LOAD(&in_purge))
        STORE(&reacquiring, 1);
    __real_hal_idle_until(deadline);
}
void __real_ia64_smp_global_purge(struct vcpu *, uint32_t, uint64_t, unsigned, bool);
void __wrap_ia64_smp_global_purge(struct vcpu *v, uint32_t rid, uint64_t va, unsigned shift,
                                  bool alat)
{
    /* Link-wrap the semantic call, including a cold and a warm Tier0 run.
     * A TR overlap's machine check is latched only after the broadcast. */
    CHECK(!tier0_arena.executing);
    CHECK(!v->pal_mca_pending);
    STORE(&in_purge, 1);
    __real_ia64_smp_global_purge(v, rid, va, shift, alat);
    STORE(&in_purge, 0);
    CHECK(!v->pal_mca_pending);
    if (delivery == 2 && !v->cfg.index) {
        CHECK(LOAD(&v->irq_kick));
        CHECK(!(v->s.cr[CR_IRR0 + 1] & (UINT64_C(1) << 33)));
    }
}
static void issue(struct vcpu *, unsigned);
struct vcpu_exit __wrap_engine_run(struct vcpu *v, uint64_t budget)
{
    if (!delivery)
        return __real_engine_run(v, budget);
    unsigned i = v->cfg.index, call = calls[i]++;
    if (!i) {
        if (!call) {
            STORE(&issuer_entered, 1);
            while (!(delivery == 2 ? LOAD(&peer_queued) : LOAD(&peer_entered)))
                __builtin_ia32_pause();
            issue(v, 0);
            STORE(&issuer_done, 1);
        } else if (delivery == 2) {
            CHECK(v->s.cr[CR_IRR0 + 1] & (UINT64_C(1) << 33));
            STORE(&issuer_boundary, 1);
        }
    } else if (!call && delivery == 1) {
        STORE(&peer_entered, 1);
        while (!LOAD(&v->irq_kick)) {
            CHECK(ia64_tlb_find(&v->s, VA, 1));
            __builtin_ia32_pause();
        }
    } else if (!call && delivery == 2) {
        CHECK(!ia64_tlb_find(&v->s, VA, 0) && !ia64_tlb_find(&v->s, VA, 1));
        STORE(&delivered, 1);
        while (!LOAD(&reacquiring))
            __builtin_ia32_pause();
        CHECK(platform_irq_inject(0, 97, 0));
    } else if (call) {
        CHECK(!ia64_tlb_find(&v->s, VA, 0) && !ia64_tlb_find(&v->s, VA, 1));
        STORE(&delivered, 1);
    }
    __atomic_exchange_n(&v->irq_kick, 0, __ATOMIC_ACQ_REL);
    return (struct vcpu_exit){.reason = VCPU_EXIT_BUDGET};
}

static struct vm *fixture(unsigned n, bool serialize)
{
    CHECK(!hal_linux_configure(true, n, 0, NULL, 0));
    CHECK(vm_purge_configure("purge_panic_ms=0"));
    domain = vm_create(n, &platform, serialize);
    CHECK(domain);
    for (unsigned i = 0; i < n; ++i) {
        struct vcpu_config cfg = {.index = i,
                                  .lid = IA64VMM_CPU_LID(i),
                                  .engine = ENGINE_INTERP,
                                  .itc_hz = IA64VMM_ITC_HZ};
        struct vcpu *v = vcpu_create(&cfg);
        CHECK(v);
        domain->cpu[i].v = v;
        domain->cpu[i].host_cpu = i + 1;
        STORE(&domain->cpu[i].state, VM_CPU_RUNNING);
        v->s.rr[1] = (uint64_t)RID << 8 | 12 << 2;
        CHECK(platform_irq_bind(v, i, IA64VMM_CPU_LID(i), i + 1));
    }
    return domain;
}
/* Warm microTLBs through the same instruction insertion path as
 * guests, then compare each translation with the associative lookup. */
static void instruction_insert(struct vcpu *v, uint64_t va, unsigned side, bool tr)
{
    Ia64Instruction i = {.opcode = tr     ? side ? IA64_OP_ITR_D : IA64_OP_ITR_I
                                   : side ? IA64_OP_ITC_D
                                          : IA64_OP_ITC_I};
    i.operands.system.source = 2;
    i.operands.system.register_index = 3;
    v->s.cr[CR_IFA] = va;
    v->s.cr[CR_ITIR] = 12 << 2;
    v->s.gr[2] = 0x200000 | 0x661;
    v->s.gr[3] = 0;
    CHECK(ia64_tlb(v, &i).kind == IA64_STEP_COMPLETE);
}
static void differential(struct vcpu *v, uint64_t va)
{
    v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
    for (unsigned side = 0; side < 2; ++side) {
        const struct ia64_tlb_entry *e = ia64_tlb_find(&v->s, va, side);
        struct ia64_translation t;
        unsigned rights = side ? 1 : 4;
        struct ia64_result r =
            ia64_translate(v, va, side ? IA64_REF_DATA : IA64_REF_FETCH, rights, 0, false, &t);
        CHECK(t.entry == e);
        if (e) {
            CHECK(r.kind == IA64_STEP_COMPLETE);
            CHECK(t.pa == ((e->pte & ((UINT64_C(1) << 50) - 4096) & ~ia64_low_mask(e->page_shift)) |
                           (va & ia64_low_mask(e->page_shift))));
        } else
            CHECK(r.fault == (side ? IA64_FAULT_ALT_DTLB : IA64_FAULT_ALT_ITLB));
    }
}
static void insert(struct vcpu *v, unsigned k)
{
    for (unsigned d = 0; d < 2; ++d)
        instruction_insert(v, VA + (uint64_t)k * 4096, d, false);
    differential(v, VA + (uint64_t)k * 4096);
}
static void issue(struct vcpu *v, unsigned k)
{
    v->s.gr[2] = 12 << 2;
    v->s.gr[3] = VA + (uint64_t)k * 4096;
    Ia64Instruction insn = {.opcode = k & 1 ? IA64_OP_PTC_GA : IA64_OP_PTC_G};
    insn.operands.system.source = 2;
    insn.operands.system.register_index = 3;
    CHECK(ia64_tlb(v, &insn).kind == IA64_STEP_COMPLETE);
}
static uint64_t delivery_slice(void *ctx, struct vm_cpu *c)
{
    (void)ctx;
    if (delivery == 2 && c->index == 1) {
        while (!LOAD(&issuer_entered))
            __builtin_ia32_pause();
        STORE(&peer_prelock, 1);
    }
    return VM_SLICE_BUDGET;
}
static void delivery_case(bool serialize)
{
    delivery = serialize ? 2 : 1;
    fixture(2, serialize);
    insert(domain->cpu[1].v, 0);
    CHECK(platform_irq_inject(1, 64, 0)); /* leave reset-hold */
    const struct vm_exec_ops ops = {.slice = delivery_slice};
    struct vm_exec exec = {.ops = &ops, .start = hal_counter(), .hz = hal_counter_hz()};
    CHECK(vm_start(domain, &exec));
    while (!LOAD(&issuer_done) || !LOAD(&delivered) || (serialize && !LOAD(&issuer_boundary)))
        __builtin_ia32_pause();
    vm_stop_all(domain, false);
    CHECK(vm_wait(domain, hal_counter() + hal_counter_hz()) == VMM_EXIT_TIMEOUT);
    CHECK(domain->cpu[1].v->purge_seen == 1);
    puts(
        serialize
            ? "purge: queued non-issuer applies before slice; reacquire preserves IRQ until boundary"
            : "purge: running TC reads -> kick -> executor apply -> clean next slice");
}
static void tier0_boundary(void)
{
    fixture(1, false);
    static unsigned char code[4096] __attribute__((aligned(4096)));
    FILE *f = fopen("build/tests/ia64/smp-boundary.bin", "rb");
    CHECK(f);
    CHECK(fread(code, 1, sizeof(code), f) > 0);
    fclose(f);
    CHECK(gm_add_region(
        &(struct gm_region){.base = 0x100000, .size = sizeof(code), .hva = code, .kind = GM_RAM}));
    struct vcpu *v = domain->cpu[0].v;
    v->cfg.engine = ENGINE_TIER0;
    for (unsigned warm = 0; warm < 2; ++warm) {
        struct ia64_entry_regs entry = {.ip = 0x100000};
        vcpu_reset(v, &entry);
        struct vcpu_exit out = engine_run(v, 8);
        CHECK(out.reason == VCPU_EXIT_BUDGET && v->retired == 8);
    }
    struct tier0_stats totals;
    tier0_get_stats(&totals);
    CHECK(totals.interpreter_instructions[TIER0_INTERP_GLOBAL_PURGE] == 4);
    puts("purge: cold/warm Tier0 global instructions run with no live region");
}
/* The post while parked must make a subsequent park check wake locally,
 * even though the issuer deliberately sent no kick to a parked target. */
static void pairing(void)
{
    fixture(2, false);
    struct vcpu *peer = domain->cpu[1].v;
    vm_cpu_park(peer);
    CHECK(!hal_cpu_poll());
    issue(domain->cpu[0].v, 0);
    CHECK(!LOAD(&peer->irq_kick));
    vm_cpu_park(peer);
    CHECK(hal_cpu_poll() & HAL_CPU_WAKE);
    vm_cpu_unpark(peer);
    CHECK(peer->purge_seen == 1);
    puts("purge: park/post tail recheck preserves the idle wake");
}
static void barrier(struct vcpu *v)
{
    unsigned g = LOAD(&generation);
    if (__atomic_add_fetch(&arrived, 1, __ATOMIC_ACQ_REL) == domain->count) {
        STORE(&arrived, 0);
        STORE(&generation, g + 1);
    } else
        while (LOAD(&generation) == g) {
            ia64_smp_apply_purges(v);
            __builtin_ia32_pause();
        }
    ia64_smp_apply_purges(v);
}
static void stress_worker(void *arg)
{
    struct vm_cpu *c = arg;
    struct vcpu *v = c->v;
    while (!LOAD(&go))
        __builtin_ia32_pause();
    for (unsigned r = 0; r < rounds; ++r) {
        barrier(v);
        for (unsigned k = 0; k < domain->count; ++k)
            insert(v, k);
        uint64_t before = v->mmu_seq;
        barrier(v);
        if (serial)
            vm_run_lock(v);
        uint64_t t = hal_counter();
        issue(v, c->index);
        t = hal_counter() - t;
        if (serial)
            vm_run_unlock(v);
        elapsed[c->index] += t;
        if (t > worst[c->index])
            worst[c->index] = t;
        barrier(v);
        CHECK(v->mmu_seq - before == 2 * domain->count);
        CHECK(LOAD(&v->purge_seen) == (uint64_t)(r + 1) * domain->count);
        for (unsigned d = 0; d < 2; ++d)
            for (unsigned k = 0; k < domain->count; ++k)
                CHECK(!ia64_tlb_find(&v->s, VA + (uint64_t)k * 4096, d));
    }
    __atomic_fetch_add(&finished, 1, __ATOMIC_RELEASE);
}
static void stress(unsigned n, bool serialize)
{
    serial = serialize;
    fixture(n, serialize);
    for (unsigned i = 0; i < n; ++i)
        CHECK(hal_cpu_start(i + 1, stress_worker, &domain->cpu[i]));
    STORE(&go, 1);
    /* All issuers must reach a barrier to advance generation. Merely
     * spinning or servicing the same purge is not forward progress. */
    unsigned progress = LOAD(&generation);
    while (LOAD(&finished) != n) {
        unsigned current = LOAD(&generation);
        if (current != progress) {
            progress = current;
            watchdog_progress();
        }
        hal_cpu_poll();
        usleep(100);
    }
    uint64_t sum = 0, max = 0;
    for (unsigned i = 0; i < n; ++i) {
        sum += elapsed[i];
        if (worst[i] > max)
            max = worst[i];
    }
    double load[3];
    CHECK(getloadavg(load, 3) == 3);
    printf(
        "purge: %u issuers, serialize=%u, %u rounds: every entry acknowledged, exact MMU counts; "
        "mean %.3f us max %.3f us; hooks=%llu load=%.2f %.2f %.2f\n",
        n, serialize, rounds, (double)sum / n / rounds / 1000, (double)max / 1000,
        (unsigned long long)hook_count, load[0], load[1], load[2]);
}
static void parked_worker(void *arg)
{
    struct vcpu *v = ((struct vm_cpu *)arg)->v;
    for (unsigned r = 1; r <= 100; ++r) {
        insert(v, 0);
        insert(v, 1);
        /* Unrelated TC and TR: overrun must clear the TC, preserve the TR. */
        instruction_insert(v, VA + 0x80000, 1, false);
        /* Keep the same TR across rounds: reinsertion without PTR would
         * itself require MCA (SDM Vol.2 Table 4-1). */
        if (r == 1)
            instruction_insert(v, VA + 0x100000, 1, true);
        differential(v, VA + 0x80000);
        differential(v, VA + 0x100000);
        uint64_t before = v->mmu_seq;
        vm_cpu_park(v);
        STORE(&park_ready, r);
        while (LOAD(&park_release) < r)
            __builtin_ia32_pause();
        usleep((r * 7919u) % 1000); /* randomized park/unpark delay */
        vm_cpu_unpark(v);
        CHECK(v->mmu_seq - before == (r % 2 ? 4u : 5u));
        CHECK(!ia64_tlb_find(&v->s, VA, 1) && !ia64_tlb_find(&v->s, VA + 4096, 0));
        CHECK(v->s.tr[1][0].valid);
        CHECK((bool)ia64_tlb_find(&v->s, VA + 0x80000, 1) == (bool)(r % 2));
        differential(v, VA);
        differential(v, VA + 4096);
        differential(v, VA + 0x80000);
        differential(v, VA + 0x100000);
        STORE(&park_done, r);
        while (LOAD(&park_idle) < r)
            __builtin_ia32_pause();
    }
}
static void local_forms(void)
{
    struct vcpu *issuer = domain->cpu[0].v, *peer = domain->cpu[1].v;
    insert(peer, 0);
    static const Ia64Opcode forms[] = {IA64_OP_PTC_L, IA64_OP_PTC_E, IA64_OP_PTR_D, IA64_OP_PTR_I};
    for (unsigned k = 0; k < sizeof(forms) / sizeof(forms[0]); ++k) {
        insert(issuer, 0);
        issuer->s.gr[2] = 12 << 2;
        issuer->s.gr[3] = VA;
        Ia64Instruction insn = {.opcode = forms[k]};
        insn.operands.system.source = 2;
        insn.operands.system.register_index = 3;
        CHECK(ia64_tlb(issuer, &insn).kind == IA64_STEP_COMPLETE);
        ia64_smp_apply_purges(peer);
        CHECK(peer->purge_seen == 0);
        CHECK(ia64_tlb_find(&peer->s, VA, 0) && ia64_tlb_find(&peer->s, VA, 1));
    }
}
static void ranges(void)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v);
    v->s.rr[1] = (uint64_t)RID << 8 | 12 << 2;
    insert(v, 0);
    ia64_tc_insert(v, VA, 1, 1 | 32 | 64 | 2 << 9, 16 << 2); /* target larger than purge */
    ia64_tc_insert(v, VA + 0x20000, 0, 1 | 32 | 64 | 2 << 9, 12 << 2); /* disjoint VA */
    v->s.rr[1] = (uint64_t)(RID + 1) << 8 | 12 << 2;
    ia64_tc_insert(v, VA, 0, 1 | 32 | 64 | 2 << 9, 12 << 2); /* disjoint RID */
    v->s.tr[1][0] = (struct ia64_tlb_entry){.valid = true, .va = VA, .rid = RID, .page_shift = 12};
    uint64_t before = v->mmu_seq;
    ia64_tlb_remote_purge(v, RID, VA + 4096, 13, false);
    CHECK(v->mmu_seq - before == 2 && v->s.tr[1][0].valid);
    CHECK(ia64_tlb_find(&v->s, VA, 0)); /* current RR differs from the posted RID */
    v->s.rr[1] = (uint64_t)RID << 8 | 12 << 2;
    CHECK(!ia64_tlb_find(&v->s, VA, 0));
    CHECK(ia64_tlb_find(&v->s, VA + 0x20000, 0));
    ia64_tlb_remote_purge(v, 0, 0, 0, true);
    CHECK(v->mmu_seq - before == 4 && v->s.tr[1][0].valid);
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned k = 0; k < IA64_TC_ENTRIES; ++k)
            ia64_tc_insert(v, VA + 0x40000 + (uint64_t)k * 4096, side, 1 | 32 | 64 | 2 << 9,
                           12 << 2);
    before = v->mmu_seq;
    ia64_tlb_remote_purge(v, 0, 0, 0, true);
    CHECK(v->mmu_seq - before == 2 * IA64_TC_ENTRIES && v->s.tr[1][0].valid);
    ia64_tlb_remote_purge(v, 0, 0, 0, true);
    CHECK(v->mmu_seq - before == 2 * IA64_TC_ENTRIES); /* invalid entries log nothing */
    puts(
        "purge: page sizes, different target RR, disjoint VA/RID, TR preservation, 128-entry MMU count passed");
}
/* SDM Vol.2 Table 4-1, §5.2.2.2.3: a ptc.g over a TR on the issuer machine
 * checks there, after the local and remote TC purges; a remote processor
 * purges its TCs, keeps its TR and takes no machine check. Without a
 * PALE_CHECK the issuer stops before purging or posting anything. Each
 * vCPU has DTR 0 and an ITC on the purged page. */
static void tr_overlap(void)
{
    fixture(2, false);
    struct vcpu *both[2] = {domain->cpu[0].v, domain->cpu[1].v}, *issuer = both[0], *peer = both[1];
    for (unsigned i = 0; i < 2; ++i)
        instruction_insert(both[i], VA, 1, true);
    for (unsigned check = 0; check < 2; ++check) {
        issuer->cfg.pale_check = check ? 0x100000 : 0;
        for (unsigned i = 0; i < 2; ++i)
            instruction_insert(both[i], VA, 0, false);
        uint64_t seen = peer->purge_seen;
        vm_cpu_park(peer);
        issuer->s.gr[2] = 12 << 2;
        issuer->s.gr[3] = VA;
        Ia64Instruction insn = {.opcode = IA64_OP_PTC_G};
        insn.operands.system.source = 2;
        insn.operands.system.register_index = 3;
        struct ia64_result r = ia64_tlb(issuer, &insn);
        vm_cpu_unpark(peer);
        if (!check) {
            CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_UNIMPLEMENTED &&
                  peer->purge_seen == seen);
            for (unsigned i = 0; i < 2; ++i)
                CHECK(ia64_tlb_find(&both[i]->s, VA, 0));
        } else {
            CHECK(r.kind == IA64_STEP_COMPLETE && issuer->pal_mca_pending &&
                  peer->purge_seen == seen + 1);
            for (unsigned i = 0; i < 2; ++i)
                CHECK(!ia64_tlb_find(&both[i]->s, VA, 0));
        }
        CHECK(!peer->pal_mca_pending);
        for (unsigned i = 0; i < 2; ++i)
            CHECK(ia64_tlb_find(&both[i]->s, VA, 1) == &both[i]->s.tr[1][0]);
    }
    puts(
        "purge: ptc.g over a TR machine checks only the issuer after every TC purge; without PALE_CHECK it posts nothing");
}
static void ring(void)
{
    fixture(2, false);
    local_forms();
    CHECK(hal_cpu_start(2, parked_worker, &domain->cpu[1]));
    struct vcpu *v = domain->cpu[0].v;
    uint64_t count = 0;
    for (unsigned r = 1; r <= 100; ++r) {
        while (LOAD(&park_ready) < r)
            __builtin_ia32_pause();
        /* 64 pending entries still fit; 65 and 70 force whole-TC recovery. */
        unsigned posts = r % 4 == 1 ? 2 : r % 4 == 3 ? 64 : r % 4 == 0 ? 65 : 70;
        for (unsigned k = 0; k < posts; ++k)
            issue(v, k % 2);
        count += posts;
        STORE(&park_release, r);
        while (LOAD(&park_done) < r) {
            hal_cpu_poll();
            __builtin_ia32_pause();
        }
        CHECK(LOAD(&domain->cpu[1].v->purge_seen) == count);
        STORE(&park_idle, r);
    }
    struct vcpu shadow = *v;
    uint64_t seen = v->purge_seen;
    ia64_smp_global_purge(&shadow, RID, VA, 12, false);
    CHECK(v->purge_seen == seen); /* no-route replay posts nothing */
    ia64_smp_apply_purges(v);
    CHECK(v->purge_seen == count);
    /* A lagging issuer must also report a whole-TC replay to lockstep. */
    STORE(&domain->cpu[1].v->parked, 1);
    STORE(&v->purge_seen, 0);
    issue(v, 0);
    CHECK(hook_all == 1);
    puts(
        "purge: ring wrap/overrun, 100 delayed park/unpark batches, TR preservation, no-route shadow passed");
}
static void race_worker(void *arg)
{
    struct vcpu *v = ((struct vm_cpu *)arg)->v;
    insert(v, 0);
    vm_cpu_park(v);
    STORE(&park_ready, 1);
    while (!LOAD(&race_gate))
        __builtin_ia32_pause();
    vm_cpu_unpark(v);
    STORE(&race_unparked, 1);
    while (!LOAD(&v->irq_kick) && !LOAD(&race_release))
        __builtin_ia32_pause();
    if (LOAD(&v->irq_kick))
        ia64_smp_apply_purges(v);
    STORE(&park_done, 1);
}
static void park_post_race(void)
{
    fixture(2, false);
    CHECK(hal_cpu_start(2, race_worker, &domain->cpu[1]));
    while (!LOAD(&park_ready))
        __builtin_ia32_pause();
    STORE(&go, 1);
    issue(domain->cpu[0].v, 0);
    STORE(&race_release, 1);
    while (!LOAD(&park_done))
        __builtin_ia32_pause();
    CHECK(!ia64_tlb_find(&domain->cpu[1].v->s, VA, 1));
    puts("purge: target unparked between parked snapshot and post: acknowledged");
}
static void delayed_worker(void *arg)
{
    struct vcpu *v = ((struct vm_cpu *)arg)->v;
    /* Let the issuer actually report twice before acknowledging. A sleep
     * can end before a descheduled issuer ever checks its diagnostic. */
    while (!diagnostic_ack || LOAD(&diagnostic_lines) < 2)
        usleep(100);
    ia64_smp_apply_purges(v);
    STORE(&finished, 1);
}
static void diagnostic_line(const char *s, size_t n)
{
    static const char prefix[] = "[error] purge wait vCPU1 ip=";
    if (n >= sizeof(prefix) - 1 && !memcmp(s, prefix, sizeof(prefix) - 1))
        __atomic_fetch_add(&diagnostic_lines, 1, __ATOMIC_RELEASE);
}
static void diagnostic(const char *mode)
{
    fixture(2, false);
    CHECK(!vm_purge_configure("purge_panic_ms=-1"));
    CHECK(!vm_purge_configure("purge_panic_ms=4294967296"));
    CHECK(vm_purge_configure(!strcmp(mode, "panic") ? "purge_panic_ms=1200" : "purge_panic_ms=0"));
    diagnostic_ack = strcmp(mode, "panic") != 0;
    log_set_fb_sink(diagnostic_line);
    CHECK(hal_cpu_start(2, delayed_worker, &domain->cpu[1]));
    issue(domain->cpu[0].v, 0);
    while (!LOAD(&finished))
        __builtin_ia32_pause();
    puts("purge: disabled panic, repeated diagnostics, delayed acknowledgement passed");
}

/* A linker wrap supplies a deterministic writer between the two sets of
 * long-format loads. Product memory helpers still perform every read. */
static uint64_t table[4096] __attribute__((aligned(32768)));
static uint64_t walk_pa;
static unsigned reads, change_word;
static bool set_ti, big;
struct ia64_result __real_ia64_physical_read(struct vcpu *, gpa_t, unsigned, uint64_t *);
struct ia64_result __wrap_ia64_physical_read(struct vcpu *v, gpa_t pa, unsigned size,
                                             uint64_t *value)
{
    struct ia64_result result = __real_ia64_physical_read(v, pa, size, value);
    if (pa >= walk_pa && pa < walk_pa + 24 && ++reads == 3 && change_word < 3) {
        uint64_t *entry = (uint64_t *)((char *)table + walk_pa);
        uint64_t val = big ? __builtin_bswap64(entry[change_word]) : entry[change_word];
        val ^= change_word == 0 ? 0x1000 : change_word == 1 ? 0x100 : 1;
        entry[change_word] = big ? __builtin_bswap64(val) : val;
        if (set_ti)
            entry[2] |= big ? 0x80 : UINT64_C(1) << 63;
    }
    return result;
}
static void walker(bool smp)
{
    struct gm_region region = {
        .base = 0, .size = sizeof(table), .hva = (uint8_t *)table, .kind = GM_RAM};
    CHECK(gm_add_region(&region));
    fixture(smp ? 2 : 1, false);
    struct vcpu *v = domain->cpu[0].v;
    for (unsigned endian = 0; endian < 2; ++endian)
        for (unsigned ti = 0; ti < 2; ++ti)
            for (unsigned word = 0; word < 4; ++word) {
                big = endian;
                set_ti = ti;
                change_word = smp ? word : 3;
                reads = 0;
                vcpu_reset(v, NULL);
                v->s.psr = IA64_PSR_DT | IA64_PSR_IC;
                v->s.rr[1] = (uint64_t)RID << 8 | 12 << 2 | 1;
                v->s.cr[CR_PTA] = 0x400000 | 15 << 2 | 0x101;
                v->s.cr[CR_DCR] = endian ? 2 : 0;
                uint64_t hash = ia64_thash(&v->s, VA);
                walk_pa = hash & 32767;
                ia64_tc_insert(v, hash, 1, 1 | 32 | 64 | 2 << 9, 15 << 2);
                uint64_t *e = (uint64_t *)((char *)table + walk_pa);
                e[0] = 0x200000 | 1 | 32 | 64 | 2 << 9;
                e[1] = 12 << 2;
                e[2] = ia64_ttag(&v->s, VA);
                if ((!smp || word == 3) && ti)
                    e[2] |= UINT64_C(1) << 63;
                if (big)
                    for (unsigned k = 0; k < 3; ++k)
                        e[k] = __builtin_bswap64(e[k]);
                struct ia64_translation out;
                struct ia64_result result =
                    ia64_translate(v, VA, IA64_REF_DATA, IA64_ACCESS_READ, 0, false, &out);
                bool stable = (!smp || word == 3) && !ti;
                CHECK((result.kind == IA64_STEP_COMPLETE) == stable);
                CHECK((ia64_tlb_find(&v->s, VA, 1) != NULL) == stable);
                CHECK(reads == (smp ? 6u : 3u));
            }
    puts(
        smp ? "purge: walker tag/PTE/ITIR rewrites, with/without ti, both byte orders: misses; stable controls insert"
            : "purge: single-vCPU walker uses three reads, both byte orders, ti still misses");
}
static int child(const char *mode, unsigned n, bool serialize)
{
    FILE *capture = tmpfile();
    CHECK(capture);
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        CHECK(dup2(fileno(capture), STDERR_FILENO) == STDERR_FILENO);
        watchdog_start(mode, getenv("SMP_PURGE_TEST_ALARM") ? 3 : 20);
        if (!strcmp(mode, "stress"))
            stress(n, serialize);
        else if (!strcmp(mode, "ring"))
            ring();
        else if (!strcmp(mode, "walker"))
            walker(true);
        else if (!strcmp(mode, "walker-single"))
            walker(false);
        else if (!strcmp(mode, "delivery"))
            delivery_case(false);
        else if (!strcmp(mode, "queued"))
            delivery_case(true);
        else if (!strcmp(mode, "tier0"))
            tier0_boundary();
        else if (!strcmp(mode, "pairing"))
            pairing();
        else if (!strcmp(mode, "race"))
            park_post_race();
        else if (!strcmp(mode, "ranges"))
            ranges();
        else if (!strcmp(mode, "overlap"))
            tr_overlap();
        else
            diagnostic(mode);
        fflush(NULL);
        _exit(0);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    rewind(capture);
    char output[32768];
    size_t bytes = fread(output, 1, sizeof(output) - 1, capture);
    output[bytes] = 0;
    fclose(capture);
    if (!strcmp(mode, "diagnostic") || !strcmp(mode, "panic")) {
        CHECK(strstr(output, "purge wait vCPU1 ip="));
        CHECK(strstr(output,
                     "parked=0 purge_seen=0 await=1 rid=123 va=2000000000100000 ps=12 alat=0"));
        if (!strcmp(mode, "panic"))
            CHECK(strstr(output, "[panic] global purge acknowledgement timeout\n"));
        else {
            char *first = strstr(output, "purge wait vCPU1 ip=");
            CHECK(strstr(first + 1, "purge wait vCPU1 ip="));
        }
    }
    if (status)
        fputs(output, stderr);
    if (status)
        fprintf(stderr, "purge child %s n=%u serialize=%u: status=%#x signal=%d exit=%d\n", mode, n,
                serialize, status, WIFSIGNALED(status) ? WTERMSIG(status) : 0,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return status;
}
int main(int argc, char **argv)
{
    log_set_level(LOG_ERROR);
    if (argc > 2)
        rounds = strtoul(argv[2], NULL, 0);
    if (argc > 1 && !strcmp(argv[1], "stress")) {
        unsigned n = argc > 3 ? strtoul(argv[3], NULL, 0) : 4;
        CHECK(n >= 2 && n <= 8 && rounds);
        CHECK(child("stress", n, argc > 4) == 0);
    } else if (argc > 1)
        CHECK(child(argv[1], 0, false) == 0);
    else {
        CHECK(child("tier0", 0, false) == 0);
        CHECK(child("delivery", 0, false) == 0);
        CHECK(child("queued", 0, false) == 0);
        CHECK(child("pairing", 0, false) == 0);
        CHECK(child("walker-single", 0, false) == 0);
        CHECK(child("walker", 0, false) == 0);
        CHECK(child("ranges", 0, false) == 0);
        CHECK(child("overlap", 0, false) == 0);
        CHECK(child("ring", 0, false) == 0);
        for (unsigned n = 2; n <= 8; ++n)
            CHECK(child("stress", n, false) == 0);
        CHECK(child("stress", 4, true) == 0);
        CHECK(child("diagnostic", 0, false) == 0);
        int s = child("panic", 0, false);
        CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 1);
    }
    return 0;
}
