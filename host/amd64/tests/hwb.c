/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual ring-0 faults, alias coherence, slot reuse, target IPI/idle
 * completion, one window instance per executor reusing one range in the
 * executors' private roots, and fatal
 * faults with an installed client. */
#include "host/amd64/amd64.h"
#include "host/amd64/smp.h"
#include "host/amd64/hostwin_amd64.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#include "core/vm.h"

#define PAGE 4096u
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c))                                                                                  \
            hal_panic("HWB selftest: " #c);                                                        \
    } while (0)
extern uint64_t amd64_hwb_load(uintptr_t address);
extern void amd64_hwb_store(uintptr_t address, uint64_t value);
extern char amd64_hwb_load_site[], amd64_hwb_store_site[];
extern char amd64_hwb_load_retry[], amd64_hwb_store_retry[];
extern char amd64_hwb_simd_access[], amd64_hwb_simd_site[], amd64_hwb_simd_landing[];
struct access {
    struct hostwin_vcpu *hw;
    struct vcpu *v;
    void *ctx;
    uint64_t gva, faults;
    uintptr_t address;
    bool write;
};
static struct access access[BOOT_MAX_CPUS];
static bool trap(struct hostwin_trap *t)
{
    struct access *a = &access[hal_cpu_index()];
    if (a->hw && t->rip == (uintptr_t)amd64_hwb_simd_site && t->fault_va == a->address &&
        !t->write) {
        ++a->faults;
        t->rip = (uintptr_t)amd64_hwb_simd_landing;
        return true;
    }
    uintptr_t site = (uintptr_t)(a->write ? amd64_hwb_store_site : amd64_hwb_load_site);
    if (!a->hw || t->rip != site || t->fault_va != a->address || t->write != a->write)
        return false;
    ++a->faults;
    t->rip = (uintptr_t)(a->write ? amd64_hwb_store_retry : amd64_hwb_load_retry);
    return true;
}
void amd64_hwb_resolve(void)
{
    struct access *a = &access[hal_cpu_index()];
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    CHECK(flags & 512); /* IRET restored IF: this is normal resolution context. */
    enum hostwin_exit_reason why;
    CHECK(hostwin_resolve(a->hw, a->v, a->gva, 8, a->write, &why) == HOSTWIN_MAPPED);
}
static uintptr_t bind(struct access *a, uint64_t gva)
{
    struct hostwin_tag tag;
    CHECK(hostwin_bind(a->hw, a->v, gva, &tag) == HOSTWIN_OK);
    a->gva = gva;
    a->address = gva + tag.delta;
    return a->address;
}
static uint64_t load(struct access *a, uint64_t gva)
{
    a->write = false;
    return amd64_hwb_load(bind(a, gva));
}
static void store(struct access *a, uint64_t gva, uint64_t value)
{
    a->write = true;
    amd64_hwb_store(bind(a, gva), value);
}
static uint64_t backing_pa;
static uint64_t isolated_value;
static const struct hostwin_backend *backend;
static void basic(void *unused)
{
    (void)unused;
    struct access *a = &access[hal_cpu_index()];
    void *ctx = a->ctx;
    uint64_t value = UINT64_C(0x123456789abcdef0), read = 0;
    CHECK(gm_write(backing_pa, &value, 8));
    CHECK(load(a, backing_pa) == value && a->faults == 1);
    store(a, backing_pa, 42);
    CHECK(gm_read(backing_pa, &read, 8) && read == 42);
    CHECK(backend->protect_ro(ctx, a->address));
    backend->flush(ctx, a->address, true);
    store(a, backing_pa, 43);
    CHECK(a->faults == 2 && gm_read(backing_pa, &read, 8) && read == 43);
    CHECK(backend->unmap(ctx, a->address & ~(uintptr_t)(PAGE - 1), PAGE));
    backend->flush(ctx, HOSTWIN_FLUSH_ALL, true);
    /* The #PF path (stub, amd64_exception, hostwin_trap, client) leaves the
     * interrupted code's YMM, MXCSR and x87 state as they were. */
    uint64_t faulted = 0;
    const char *problem =
        amd64_simd_selftest_call(amd64_hwb_simd_access, (void *)a->address, &faulted);
    if (problem) {
        char message[96];
        vmm_snprintf(message, sizeof(message),
                     "HWB selftest: #PF changed the interrupted code's %s", problem);
        hal_panic(message);
    }
    CHECK(faulted == 1 && a->faults == 3);
    CHECK(load(a, backing_pa) == 43 && a->faults == 4);
    uint64_t epoch = hostwin_bind_epoch(a->hw);
    for (unsigned i = 1; i <= HOSTWIN_MAX_SLOTS; ++i)
        bind(a, backing_pa + ((uint64_t)i << hostwin_bits(a->hw)));
    CHECK(hostwin_bind_epoch(a->hw) > epoch);
    CHECK(load(a, backing_pa) == 43 && a->faults == 5);
    hostwin_invalidate(a->hw);
    CHECK(amd64_shadow_used() == 0);
    /* Repeated detach/fill proves that the separate pool is reclaimed. */
    for (unsigned i = 0; i < 128; ++i) {
        CHECK(load(a, backing_pa) == 43);
        hostwin_invalidate(a->hw);
        CHECK(amd64_shadow_used() == 0);
    }
    /* Removing one of two leaves in a PT must use INVLPG once; removing
     * the final leaf detaches tables and needs one context invalidation. */
    uintptr_t pair = a->address & ~((uintptr_t)(1u << 21) - 1);
    CHECK(backend->map(ctx, pair, backing_pa, true) == HOSTWIN_MAP_OK);
    CHECK(backend->map(ctx, pair + PAGE, backing_pa, true) == HOSTWIN_MAP_OK);
    struct amd64_hostwin_stats before, after;
    amd64_hostwin_get_stats(ctx, &before);
    CHECK(backend->unmap(ctx, pair, PAGE));
    backend->flush(ctx, pair, true);
    amd64_hostwin_get_stats(ctx, &after);
    CHECK(after.flushes == before.flushes + 1 && after.page_flushes == before.page_flushes + 1 &&
          after.all_flushes == before.all_flushes);
    before = after;
    CHECK(backend->unmap(ctx, pair + PAGE, PAGE));
    backend->flush(ctx, HOSTWIN_FLUSH_ALL, true);
    amd64_hostwin_get_stats(ctx, &after);
    CHECK(after.flushes == before.flushes + 1 && after.all_flushes == before.all_flushes + 1 &&
          after.page_flushes == before.page_flushes && amd64_shadow_used() == 0);
    LOGI(
        "HWB local passed CPU%u: coherence, #PF RO retry, #PF with live SIMD state, unmap, rebind, shadow reclaim",
        hal_cpu_index());
}
static void declined_fault(void *unused)
{
    (void)unused;
    struct access *a = &access[hal_cpu_index()];
    CHECK(a->hw && a->v);
    a->address = hostwin_poison_va(a->hw);
    a->write = true;
    CHECK(hostwin_owns(a->address));
    LOGI("HWB fatal test: client installed; declined in-reservation fault at non-site RIP on CPU%u",
         hal_cpu_index());
    /* All client qualifiers except the RIP match a live executor access. */
    *(volatile uint64_t *)a->address = 1;
    hal_panic("HWB declined fault did not stop");
}
struct remote {
    unsigned phase;
    uint64_t before, after;
};
static void remote_active(void *arg)
{
    struct remote *r = arg;
    struct access *a = &access[hal_cpu_index()];
    store(a, backing_pa, 44); /* cache the writable translation */
    r->before = a->faults;
    __atomic_store_n(&r->phase, 1, __ATOMIC_RELEASE);
    uint64_t until = hal_counter() + hal_counter_hz() * 3;
    while (__atomic_load_n(&r->phase, __ATOMIC_ACQUIRE) != 2) {
        CHECK(hal_counter() < until);
        __asm__ volatile("pause");
    }
    store(a, backing_pa, 45);
    r->after = a->faults;
    __atomic_store_n(&r->phase, 3, __ATOMIC_RELEASE);
}
static void isolation_load(void *arg)
{
    (void)arg;
    struct access *a = &access[hal_cpu_index()];
    a->write = false;
    isolated_value = amd64_hwb_load(a->address);
}
static void backing_load(void *arg)
{
    (void)arg;
    isolated_value = load(&access[hal_cpu_index()], backing_pa);
}
static void remote_cache(void *arg)
{
    (void)arg;
    store(&access[hal_cpu_index()], backing_pa, 46);
}
/* Another page at the cached address, in this CPU's own root. */
static void map_other(void *arg)
{
    (void)arg;
    struct access *a = &access[hal_cpu_index()];
    CHECK(backend->map(a->ctx, a->address, backing_pa + PAGE, true) == HOSTWIN_MAP_OK);
}
static void wait_flag(unsigned *flag, unsigned value)
{
    uint64_t until = hal_counter() + hal_counter_hz() * 10;
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) != value) {
        enum hal_shutdown_kind kind;
        int code;
        if (hal_shutdown_requested(&kind, &code))
            hal_shutdown(kind, code);
        CHECK(hal_counter() < until);
        __asm__ volatile("pause");
    }
}
static void run(unsigned cpu, hal_cpu_work_fn fn, void *arg)
{
    CHECK(hal_cpu_start(cpu, fn, arg));
    wait_flag(&amd64_cpus[cpu].busy, 0);
}
#ifdef HWB_TEST_SHADOW_TABLES
struct fill {
    uint64_t gva;
    enum hostwin_fill result;
    enum hostwin_exit_reason why;
};
static void fill_here(void *arg)
{
    struct fill *f = arg;
    struct access *a = &access[hal_cpu_index()];
    bind(a, f->gva);
    f->result = hostwin_resolve(a->hw, a->v, f->gva, 8, false, &f->why);
}
/* A fill made by the executor of the instance, on its own CPU. */
static enum hostwin_fill fill_on(unsigned cpu, uint64_t gva, enum hostwin_exit_reason *why)
{
    struct fill f = {.gva = gva};
    run(cpu, fill_here, &f);
    *why = f.why;
    return f.result;
}
/* Every executor holds an equal share of the pool. One whose share is
 * exhausted drops its own window and retries once, which fits; the other
 * executors' tables never stop it. */
static void exhaustion(unsigned count)
{
    unsigned share = HWB_TEST_SHADOW_TABLES / count;
    CHECK(share == 3 || share == 4);
    uint64_t sparse = backing_pa + (UINT64_C(1) << 30);
    struct gm_region alias = {.base = sparse,
                              .size = PAGE,
                              .kind = GM_RAM,
                              .hva = gm_direct_ptr(backing_pa, PAGE),
                              .name = "sparse alias"};
    CHECK(gm_add_region(&alias));
    enum hostwin_exit_reason why;
    /* Executor by executor: a page needs a PDPT, PD and PT, all from the
     * executor's own share however much the earlier ones hold; a page
     * 1 GiB away needs two tables more than the share has, so the executor
     * drops its own window and retries once, which fits. */
    for (unsigned i = 0; i < count; ++i) {
        struct access *a = &access[hal_vcpu_cpu(i)];
        struct hostwin_stats stats;
        CHECK(fill_on(hal_vcpu_cpu(i), backing_pa, &why) == HOSTWIN_MAPPED);
        hostwin_get_stats(a->hw, &stats);
        CHECK(stats.backend_failures == 0 && amd64_shadow_used() == 3 * (i + 1));
        CHECK(fill_on(hal_vcpu_cpu(i), sparse, &why) == HOSTWIN_MAPPED);
        hostwin_get_stats(a->hw, &stats);
        CHECK(stats.fills == 2 && stats.backend_failures == 1 && stats.live_mappings == 1);
        CHECK(!hostwin_stop_detail(a->hw) && !a->v->requested_exit &&
              amd64_shadow_used() == 3 * (i + 1));
    }
    for (unsigned i = 0; i < count; ++i)
        hostwin_destroy(access[hal_vcpu_cpu(i)].hw);
    CHECK(amd64_shadow_used() == 0);
    LOGI(
        "HWB exhaustion passed: %u-table pool, %u executors with %u tables each; an exhausted share "
        "drops its own window and the retry fits",
        HWB_TEST_SHADOW_TABLES, count, share);
    hal_shutdown(HAL_SHUTDOWN_POWER_OFF, VMM_EXIT_SUCCESS);
}
#endif
static void free_vcpu(struct access *a)
{
    hal_free_pages(a->v, IA64VMM_ALIGN_UP(sizeof(*a->v), PAGE) / PAGE);
    a->v = NULL;
}
void amd64_hostwin_selftest(bool fatal, bool outside)
{
    unsigned count = hal_vcpu_count();
    CHECK(count >= 2);
    unsigned first = hal_vcpu_cpu(0), second = hal_vcpu_cpu(1), last = hal_vcpu_cpu(count - 1);
    uint8_t *page = hal_alloc_pages(2, HAL_PAGE_ZERO);
    CHECK(page);
    backing_pa = (uintptr_t)page - amd64_hhdm;
    struct gm_region ram = {.base = backing_pa,
                            .size = 2 * PAGE,
                            .kind = GM_RAM,
                            .hva = page,
                            .name = "HWB selftest RAM"};
    CHECK(gm_add_region(&ram));
    void *shared;
    backend = hostwin_host_backend(&shared);
    CHECK(backend && backend->cpu_context);
    /* Only executors' CPUs have contexts: not CPU 0, which manages on the
     * boot root, nor a helper. */
    CHECK(!backend->cpu_context(shared, 0));
    for (unsigned cpu = 1; cpu < amd64_cpu_count; ++cpu) {
        bool executor = false;
        for (unsigned i = 0; i < count; ++i)
            executor |= hal_vcpu_cpu(i) == cpu;
        CHECK(executor || !backend->cpu_context(shared, cpu));
    }
    /* w <= H - 4: slot 0's guard slot, the slots and the poison page fit below 2^47. */
    void *probe = backend->cpu_context(shared, first);
    uintptr_t base, poison;
    CHECK(!backend->reserve(probe, 44, HOSTWIN_MAX_SLOTS, &base, &poison));
    CHECK(backend->reserve(probe, 43, HOSTWIN_MAX_SLOTS, &base, &poison) &&
          poison < (UINT64_C(1) << 47));
    backend->release(probe, base, 43, HOSTWIN_MAX_SLOTS);
    /* One instance per executor in its host CPU's context, as the product
     * glue makes them: every reservation is the same range, each in another
     * private root. */
    struct hostwin_config cfg = {.w = 40, .slots = HOSTWIN_MAX_SLOTS, .check = true};
    for (unsigned i = 0; i < count; ++i) {
        unsigned cpu = hal_vcpu_cpu(i);
        struct access *a = &access[cpu];
        a->v = hal_alloc_pages(IA64VMM_ALIGN_UP(sizeof(*a->v), PAGE) / PAGE, HAL_PAGE_ZERO);
        CHECK(a->v);
        a->v->cfg.index = i;
        a->v->s.version = IA64_STATE_VERSION;
        a->v->s.cpu_model = a->v->cfg.cpu_model;
        a->v->s.psr = IA64_PSR_IC;
        a->ctx = backend->cpu_context(shared, cpu);
        a->hw = hostwin_create(&cfg, backend, a->ctx);
        CHECK(a->ctx && a->hw && hostwin_poison_va(a->hw) == hostwin_poison_va(access[first].hw));
        hostwin_enter(a->hw, a->v);
    }
    /* A second reservation in a context is refused. */
    CHECK(!backend->reserve(access[first].ctx, 40, HOSTWIN_MAX_SLOTS, &base, &poison));
#ifdef HWB_TEST_SHADOW_TABLES
    exhaustion(count);
#endif
    hostwin_set_trap_client(trap);
    for (unsigned i = 0; i < count; ++i)
        run(hal_vcpu_cpu(i), basic, NULL);
    struct amd64_hostwin_stats total;
    amd64_hostwin_get_stats(NULL, &total);
    CHECK(total.maps >= 3 * count && total.failures == 0);
    struct remote r = {0};
    CHECK(hal_cpu_start(second, remote_active, &r));
    wait_flag(&r.phase, 1);
    struct access *a = &access[second];
    void *ctx = a->ctx;
    unsigned ack = __atomic_load_n(&amd64_cpus[second].window_ack, __ATOMIC_ACQUIRE);
    CHECK(backend->protect_ro(ctx, a->address));
    backend->flush(ctx, a->address, true);
    CHECK(__atomic_load_n(&amd64_cpus[second].window_ack, __ATOMIC_ACQUIRE) != ack);
    CHECK(__atomic_load_n(&amd64_cpus[second].window_ack, __ATOMIC_ACQUIRE) ==
          __atomic_load_n(&amd64_cpus[second].window_request, __ATOMIC_ACQUIRE));
    __atomic_store_n(&r.phase, 2, __ATOMIC_RELEASE);
    wait_flag(&r.phase, 3);
    wait_flag(&amd64_cpus[second].busy, 0);
    CHECK(r.after == r.before + 1);
    run(second, remote_cache, NULL);
    wait_flag(&amd64_cpus[second].tlb_lazy, 1);
    CHECK(backend->protect_ro(ctx, a->address));
    backend->flush(ctx, a->address, true);
    uint64_t faults = a->faults;
    run(second, remote_cache, NULL);
    CHECK(a->faults == faults + 1);
    LOGI(
        "HWB remote passed CPU%u: active IPI and idle sync acknowledgements before writer proceeds",
        second);
    /* Each executor's first store faults and fills its own root, although
     * every executor before it maps the same address already. */
    for (unsigned i = 0; i < count; ++i)
        hostwin_invalidate(access[hal_vcpu_cpu(i)].hw);
    for (unsigned i = 0; i < count; ++i) {
        struct access *x = &access[hal_vcpu_cpu(i)];
        faults = x->faults;
        run(hal_vcpu_cpu(i), remote_cache, NULL);
        CHECK(x->faults == faults + 1 && x->address == access[first].address);
    }
    uint64_t different = 99;
    CHECK(gm_write(backing_pa + PAGE, &different, 8));
    run(last, map_other, NULL);
    for (unsigned i = 0; i < count; ++i) {
        run(hal_vcpu_cpu(i), isolation_load, NULL);
        CHECK(isolated_value == (hal_vcpu_cpu(i) == last ? 99 : 46));
    }
    LOGI(
        "HWB roots passed: identical VA maps different physical pages on CPU%u and %u other executors",
        last, count - 1);
    /* A fill of the last executor's root on CPU0 stops its vCPU and leaves
     * the root alone; a new instance in the context reserves the same
     * range, and its executor's next load faults and fills. */
    a = &access[last];
    hostwin_invalidate(a->hw);
    enum hostwin_exit_reason why;
    bind(a, backing_pa);
    CHECK(hostwin_resolve(a->hw, a->v, backing_pa, 8, false, &why) == HOSTWIN_EXIT);
    CHECK(why == HW_EXIT_BACKEND && a->v->requested_exit == VCPU_EXIT_BAD_ACCESS + 1);
    CHECK(hostwin_stop_detail(a->hw));
    hostwin_destroy(a->hw);
    a->v->requested_exit = 0;
    a->hw = hostwin_create(&cfg, backend, a->ctx);
    CHECK(a->hw && hostwin_poison_va(a->hw) == hostwin_poison_va(access[first].hw));
    hostwin_enter(a->hw, a->v);
    faults = a->faults;
    run(last, backing_load, NULL);
    CHECK(a->faults == faults + 1 && isolated_value == 46);
    LOGI("HWB product passed: %u executors, one instance each at poison %016llx in its CPU's root; "
         "CPU0 fill for CPU%u stops",
         count, (unsigned long long)hostwin_poison_va(a->hw), last);
    /* Watchdog teardown can stop a host CPU before destroying its windows.
     * Permanent quiescence must permit reclaim without an impossible IPI. */
    hal_cpu_request(last, HAL_CPU_STOP);
    wait_flag(&amd64_cpus[last].stopped, 1);
    for (unsigned i = 0; i < count; ++i) {
        struct access *x = &access[hal_vcpu_cpu(i)];
        struct hostwin_stats stats;
        hostwin_get_stats(x->hw, &stats);
        CHECK(stats.fills > 0 && stats.backend_failures == 0);
        if (!i)
            continue; /* the fatal checks fault on the first executor */
        hostwin_destroy(x->hw);
        x->hw = NULL;
        free_vcpu(x);
    }
    a = &access[first];
    hostwin_invalidate(a->hw);
    CHECK(amd64_shadow_used() == 0);
    LOGI("HWB terminal target passed: stopped CPU%u shadow tables reclaimed", last);
    if (fatal) {
        if (outside) {
            LOGI(
                "HWB fatal test: client installed; fault outside all reservations at 0000000000001000");
            *(volatile uint64_t *)(uintptr_t)0x1000 = 1;
        } else {
            run(first, declined_fault, NULL);
        }
        hal_panic("HWB fatal fault did not stop");
    }
    hostwin_destroy(a->hw);
    a->hw = NULL;
    free_vcpu(a);
    CHECK(amd64_shadow_used() == 0);
    hostwin_set_trap_client(NULL);
    LOGI(
        "HWB selftest passed: fills>0 backend_failures=0 private roots isolated and shadow tables reclaimed");
    hal_shutdown(HAL_SHUTDOWN_POWER_OFF, VMM_EXIT_SUCCESS);
}
