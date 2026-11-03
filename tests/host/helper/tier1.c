/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Request-to-link and result equivalence.
 * The reference executes the same integer-loop workload first. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "core/helper.h"
#include "core/smp.h"
#include "engine/cache/background.h"
#include "tests/hal/hal_linux.h"
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/hostwin_linux.h"
#include "tests/tier1/provider.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/snapshot.h"
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "engine/hostwin/internal.h"
#include "engine/tier0/tier0.h"
#include "engine/tier0/internal.h"
#include "ia64vmm/hal.h"
#include "ia64/interrupt.h"
#include "tests/tier1/programs.h"
static struct snapshot snap;
static struct run_result expected, actual;
static struct t1h_logs ref_logs, logs;
static struct t1h_engine_vcpu ev;
static struct vcpu *v;
static struct vm *vm;
static unsigned done;
static bool fallback, delay, race, pressure, synchronous, many, concurrent, parallel;
static bool spans;
static uint8_t *span_raw[13], *span_base;
static struct t1_region *span_keep[2];
static unsigned span_writes;
void __real_hal_code_set_writable(void *p, size_t pages);
void __wrap_hal_code_set_writable(void *p, size_t pages);
void __wrap_hal_code_set_writable(void *p, size_t pages)
{
    __real_hal_code_set_writable(p, pages);
    if (spans && span_base && (uint8_t *)p >= span_base + 4096 &&
        (uint8_t *)p < span_base + T1_CHUNK_BYTES) {
        /* The adjacent survivor must execute during the writable phase. */
        T1H_CHECK(((int (*)(void))(uintptr_t)span_base)() == 42);
        __atomic_add_fetch(&span_writes, 1, __ATOMIC_RELAXED);
    }
}

/* Leave only closed empty pages available to the real background compiler. */
static void span_setup(void)
{
    for (unsigned i = 0; i < 13; ++i)
        T1H_CHECK((span_raw[i] = t1_arena_alloc(1)) != NULL);
    uint8_t *buf = calloc(1, T1_CHUNK_BYTES);
    T1H_CHECK(buf);
    static const uint8_t code[] = {0xb8, 42, 0, 0, 0, 0xc3};
    memcpy(buf, code, sizeof(code));
    struct t1_emitted e = {.buf = buf, .size = 16};
    unsigned chunk = UINT32_MAX;
    span_keep[0] = t1_region_new();
    struct t1_region *filler = t1_region_new();
    T1H_CHECK(span_keep[0] && filler && t1_arena_place_background(span_keep[0], &e, &chunk));
    span_base = span_keep[0]->code;
    e.size = T1_CHUNK_BYTES - 16;
    T1H_CHECK(t1_arena_place_background(filler, &e, &chunk));
    t1_arena_seal_background(&chunk);
    T1H_CHECK(chunk == UINT32_MAX);
    t1_region_discard(filler);
    span_keep[1] = t1_region_new();
    e.size = T1_CHUNK_BYTES;
    T1H_CHECK(span_keep[1] && t1_arena_place_background(span_keep[1], &e, &chunk));
    t1_arena_seal_background(&chunk);
    T1H_CHECK(chunk == UINT32_MAX && !t1_arena_free_chunks());
    free(buf);
}

static void span_cleanup(void)
{
    static const uint8_t code[] = {0xb8, 42, 0, 0, 0, 0xc3};
    T1H_CHECK(span_keep[0]->code == span_base && !memcmp(span_base, code, sizeof(code)));
    T1H_CHECK(((int (*)(void))(uintptr_t)span_base)() == 42);
    T1H_CHECK(__atomic_load_n(&span_writes, __ATOMIC_RELAXED));
    for (unsigned i = 0; i < 2; ++i)
        t1_region_discard(span_keep[i]);
    for (unsigned i = 0; i < 13; ++i)
        t1_arena_free(span_raw[i], 1);
    span_base = NULL;
}
static unsigned compiler_entered, compiler_release;
static unsigned parallel_gate, parallel_entered, parallel_release;
static _Thread_local bool frozen;
uint64_t __real_hal_counter(void);
uint64_t __wrap_hal_counter(void)
{
    return frozen ? 1 : __real_hal_counter();
}
static unsigned seals, mutation;
static gpa_t source;
static uint64_t elapsed, submission, old_link, executor_ticks_before, compile_ticks_before;
bool __real_t1_compile_step(struct t1_compile_ctx *, struct t1_compile_report *);
bool __wrap_t1_compile_step(struct t1_compile_ctx *, struct t1_compile_report *);
bool __wrap_t1_compile_step(struct t1_compile_ctx *ctx, struct t1_compile_report *rep)
{
    if (concurrent && !__atomic_load_n(&compiler_release, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&compiler_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&compiler_release, __ATOMIC_ACQUIRE))
            sched_yield();
    }
    if (delay) {
        static _Thread_local uint32_t random = 17;
        random = random * 1664525u + 1013904223u;
        struct timespec nap = {0, (random % 6) * 1000000};
        nanosleep(&nap, NULL);
    }
    bool more = __real_t1_compile_step(ctx, rep);
    static _Thread_local bool entered;
    if (more && __atomic_load_n(&parallel_gate, __ATOMIC_ACQUIRE) && !entered) {
        entered = true;
        __atomic_add_fetch(&parallel_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&parallel_release, __ATOMIC_ACQUIRE))
            sched_yield();
    }
    return more;
}
static void source_race(gpa_t page, enum code_test_stage stage)
{
    if (stage != CODE_TEST_SEAL || page != (source & ~UINT64_C(4095)))
        return;
    __atomic_add_fetch(&seals, 1, __ATOMIC_RELAXED);
    if (!__atomic_fetch_add(&mutation, 1, __ATOMIC_RELAXED))
        code_notify_vmm_write(source, 16);
}
/* Grace must protect the running executor, while eligible entries behind
 * a younger prefix still make progress across bounded reclamation passes. */
static void reclaim_tail(void)
{
    struct t1_region *a[70];
    struct tier0_region *b[70];
    for (unsigned i = 0; i < 70; ++i) {
        a[i] = t1_region_new();
        b[i] = tier0_region_allocate();
        T1H_CHECK(a[i] && b[i]);
    }
    a[0]->valid = b[0]->valid = true;
    uint64_t old = t1_region_link(a[0]);
    uint32_t old_id = a[0]->id;
    t1_region_retire(a[0]);
    tier0_region_retire(b[0], false);
    code_quiescent(v);
    uint32_t young[69];
    for (unsigned i = 1; i < 70; ++i) {
        a[i]->valid = b[i]->valid = true;
        young[i - 1] = a[i]->id;
        t1_region_retire(a[i]);
        tier0_region_retire(b[i], false);
    }
    t1_reclaim();
    tier0_reclaim_batch(64);
    T1H_CHECK(t1_region_get(old_id) == a[0]);
    t1_reclaim();
    T1H_CHECK(!t1_region_from_link(old) && !t1_region_get(old_id));
    T1H_CHECK(tier0_arena.available == (unsigned)(b[0] - tier0_arena.regions) + 1);
    /* Retired records stay allocated until grace, but no link names them. */
    for (unsigned i = 0; i < 69; ++i)
        T1H_CHECK(t1_region_get(young[i]));
    code_quiescent(v);
    t1_reclaim();
    t1_reclaim();
    tier0_reclaim_batch(64);
    tier0_reclaim_batch(64);
}
static void batch_contract(struct t1_vcpu *tv, const struct t1_compile_request *rq)
{
    struct t1_compile_ctx *ctx = t1_compile_ctx_create();
    T1H_CHECK(ctx);
    struct t1_stats before, after;
    t1_get_stats(&before);
    frozen = true;
    for (unsigned i = 0; i < 4; ++i) {
        T1H_CHECK(t1_compile_begin(ctx, rq, true));
        if (!i)
            T1H_CHECK(t1_compile_at(tv, &(struct t1_compile_options){0}, NULL) == T1_RETRY);
        while (t1_compile_step(ctx, NULL)) {
        }
    }
    t1_compile_flush(ctx);
    frozen = false;
    t1_get_stats(&after);
    T1H_CHECK(after.compiles == before.compiles + 4);
    T1H_CHECK(after.seals < before.seals + 4);
    uint64_t link = t1_upper_probe(v, rq->entry_pa);
    T1H_CHECK(link && t1_region_from_link(link)->sealed);
    t1_flush();
    code_quiescent(v);
    t1_reclaim();
    frozen = true;
    T1H_CHECK(t1_compile_begin(ctx, rq, true));
    struct t1_compile_report rep;
    while (t1_compile_step(ctx, &rep)) {
    }
    T1H_CHECK(!t1_upper_probe(v, rq->entry_pa));
    code_notify_vmm_write(rq->entry_pa, 16);
    t1_compile_flush(ctx);
    T1H_CHECK(!t1_upper_probe(v, rq->entry_pa));
    T1H_CHECK(!t1_region_get(rep.region_id));
    T1H_CHECK(t1_compile_begin(ctx, rq, true));
    while (t1_compile_step(ctx, &rep)) {
    }
    t1_compile_cancel(ctx);
    T1H_CHECK(!t1_region_get(rep.region_id));
    frozen = false;
    t1_compile_ctx_destroy(ctx);
}
static unsigned executing, may_return;
static struct t1_region *running;
static void running_gate(void)
{
    __atomic_store_n(&executing, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&may_return, __ATOMIC_ACQUIRE))
        sched_yield();
}
static void *reclaim_running(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&executing, __ATOMIC_ACQUIRE))
        sched_yield();
    uint32_t id = running->id;
    t1_region_retire(running);
    t1_reclaim();
    T1H_CHECK(t1_region_get(id) == running);
    __atomic_store_n(&may_return, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void execution_grace(void)
{
    /* AMD64 SysV: align the stack, call the gate, return through RX code. */
    uint8_t bytes[] = {0x48, 0x83, 0xec, 8,    0x48, 0xb8, 0,    0,    0, 0,   0,
                       0,    0,    0,    0xff, 0xd0, 0x48, 0x83, 0xc4, 8, 0xc3};
    uintptr_t gate = (uintptr_t)running_gate;
    memcpy(bytes + 6, &gate, sizeof(gate));
    struct t1_emitted e = {.buf = bytes, .size = sizeof(bytes)};
    running = t1_region_new();
    T1H_CHECK(running && t1_arena_place(running, &e));
    t1_arena_seal_for_entry(running);
    T1H_CHECK(t1_region_publish(running));
    uint64_t link = t1_region_link(running);
    pthread_t thread;
    T1H_CHECK(!pthread_create(&thread, NULL, reclaim_running, NULL));
    ((void (*)(void))(uintptr_t)running->code)();
    T1H_CHECK(!pthread_join(thread, NULL));
    code_quiescent(v);
    t1_reclaim();
    T1H_CHECK(!t1_region_from_link(link));
}
/* Another CPU allocates while the executor runs region code: its refusal
 * path reclaims a record the executor has passed, never the region it is
 * running, and returns without waiting for the executor. */
static struct t1_region *passed;
static void *allocate_running(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&executing, __ATOMIC_ACQUIRE))
        sched_yield();
    struct t1_stats s;
    t1_get_stats(&s);
    uint64_t freed = s.reclaimed;
    t1_region_retire(running);
    /* The passed record is freed; the running one is not. */
    void *p = t1_memory_alloc(1, 0);
    t1_get_stats(&s);
    T1H_CHECK(p && s.reclaimed == freed + 1 && running->retired);
    T1H_CHECK(!t1_memory_alloc(64, 0));
    t1_get_stats(&s);
    T1H_CHECK(s.reclaimed == freed + 1);
    t1_memory_free(p, 1);
    __atomic_store_n(&may_return, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void allocation_while_running(void)
{
    uint8_t bytes[] = {0x48, 0x83, 0xec, 8,    0x48, 0xb8, 0,    0,    0, 0,   0,
                       0,    0,    0,    0xff, 0xd0, 0x48, 0x83, 0xc4, 8, 0xc3};
    uintptr_t gate = (uintptr_t)running_gate;
    memcpy(bytes + 6, &gate, sizeof(gate));
    struct t1_emitted e = {.buf = bytes, .size = sizeof(bytes)};
    running = t1_region_new();
    T1H_CHECK(running && t1_arena_place(running, &e));
    t1_arena_seal_for_entry(running);
    T1H_CHECK(t1_region_publish(running));
    uint64_t link = t1_region_link(running);
    uint32_t id = running->id;
    passed = t1_region_new();
    T1H_CHECK(passed);
    passed->meta_pages = 4;
    passed->meta = t1_memory_alloc(4, HAL_PAGE_ZERO);
    T1H_CHECK(passed->meta && t1_region_publish(passed));
    t1_region_retire(passed);
    code_quiescent(v);
    struct t1_memory_stats s;
    t1_memory_get_stats(&s);
    uint64_t limit = s.budget_bytes;
    t1_memory_set_limit(s.used_bytes - s.workspace_bytes + s.reserve_bytes);
    __atomic_store_n(&executing, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&may_return, 0, __ATOMIC_RELEASE);
    pthread_t thread;
    T1H_CHECK(!pthread_create(&thread, NULL, allocate_running, NULL));
    static struct t1_exec x;
    t1_set_current(&x);
    ((void (*)(void))(uintptr_t)running->code)();
    t1_set_current(NULL);
    T1H_CHECK(!pthread_join(thread, NULL));
    code_quiescent(v);
    t1_memory_set_limit(limit);
    t1_reclaim();
    T1H_CHECK(!t1_region_from_link(link) && !t1_region_get(id) && !t1_memory_pressure());
}
/* A helper-like thread that makes one page code with the read formation
 * makes (code_source_read_physical), so TRACKED reaches every window
 * instance. An owner releases it from inside a critical section and goes
 * on once the tracker finished or spins on the owner's instance lock. */
static struct {
    gpa_t page;
    struct hostwin_vcpu *hw;
    uint64_t contended;
    unsigned go, done;
    pthread_t thread;
} tracker;
static void *track_page(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&tracker.go, __ATOMIC_ACQUIRE))
        sched_yield();
    uint64_t low, high, generation;
    T1H_CHECK(code_source_read_physical(tracker.page, &low, &high, &generation).kind ==
              IA64_STEP_COMPLETE);
    __atomic_store_n(&tracker.done, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void tracker_start(struct hostwin_vcpu *hw, gpa_t page)
{
    T1H_CHECK(!code_page_tracked_fast(page));
    tracker.page = page;
    tracker.hw = hw;
    tracker.go = tracker.done = 0;
    tracker.contended = __atomic_load_n(&hw->contended, __ATOMIC_RELAXED);
    T1H_CHECK(!pthread_create(&tracker.thread, NULL, track_page, NULL));
}
/* true: the tracker finished while the caller waited. */
static bool tracker_release(void)
{
    __atomic_store_n(&tracker.go, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&tracker.done, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&tracker.hw->contended, __ATOMIC_RELAXED) == tracker.contended)
        sched_yield();
    return __atomic_load_n(&tracker.done, __ATOMIC_ACQUIRE);
}
static void tracker_join(void)
{
    T1H_CHECK(!pthread_join(tracker.thread, NULL));
    T1H_CHECK(code_page_tracked_fast(tracker.page));
}
/* A page becomes tracked code while its
 * vCPU fills a window for it. The filler stops right after its code check
 * and releases the tracker there. Afterwards no alias of the tracked page
 * may be writable. */
bool __real_code_page_tracked_fast(gpa_t page);
bool __wrap_code_page_tracked_fast(gpa_t page);
static gpa_t fill_page;
bool __wrap_code_page_tracked_fast(gpa_t page)
{
    bool code = __real_code_page_tracked_fast(page);
    gpa_t armed = __atomic_load_n(&fill_page, __ATOMIC_ACQUIRE);
    if (!armed || page != armed)
        return code;
    __atomic_store_n(&fill_page, 0, __ATOMIC_RELAXED);
    tracker_release();
    return code;
}
static void writable_alias(uint64_t gva_page, gpa_t pa_page, bool writable, void *arg)
{
    (void)gva_page;
    if (pa_page == *(const gpa_t *)arg && writable)
        T1H_CHECK(!"writable alias of tracked code");
}
static gpa_t data_page(unsigned k)
{
    return T1H_TABLE + (T1H_TABLE_PAGES - 1 - k) * 4096;
}
static void tracking_fill(struct hostwin_vcpu *hw)
{
    gpa_t page = data_page(0);
    T1H_CHECK(!(v->s.psr & IA64_PSR_DT));
    struct hostwin_tag tag;
    T1H_CHECK(hostwin_bind(hw, v, page, &tag) == HOSTWIN_OK);
    tracker_start(hw, page);
    __atomic_store_n(&fill_page, page, __ATOMIC_RELEASE);
    enum hostwin_exit_reason why;
    T1H_CHECK(hostwin_resolve(hw, v, page, 8, true, &why) == HOSTWIN_MAPPED);
    tracker_join();
    hostwin_test_shadows(hw, writable_alias, &page);
    hostwin_invalidate(hw);
}
/* TRACKED walks an instance's records only
 * outside its owner's updates. A second instance, made before any helper
 * started, unmaps through a backend that releases the tracker inside the
 * owner's critical section: the drop path of hostwin_enter's update, a
 * slot rebinding in hostwin_bind, and hostwin_invalidate. The tracker must
 * still be waiting there. */
static struct hostwin_vcpu *probe_hw;
static struct vcpu *probe_v;
static struct hostwin_backend probe_backend;
static unsigned unmap_armed;
static bool probe_unmap(void *ctx, uintptr_t host_page, uint64_t bytes)
{
    if (__atomic_exchange_n(&unmap_armed, 0, __ATOMIC_ACQ_REL))
        T1H_CHECK(!tracker_release()); /* not done inside the owner's update */
    return hostwin_linux_backend()->unmap(ctx, host_page, bytes);
}
static void probe_create(void)
{
    probe_backend = *hostwin_linux_backend();
    probe_backend.unmap = probe_unmap;
    struct hostwin_config hc = {36, 2, false};
    probe_hw = hostwin_create(&hc, &probe_backend, hostwin_linux_ctx());
    probe_v = vcpu_create(&(struct vcpu_config){.engine = ENGINE_INTERP});
    T1H_CHECK(probe_hw && probe_v);
    probe_v->s.psr = 0; /* physical windows */
}
static void probe_fill(gpa_t page)
{
    struct hostwin_tag tag;
    enum hostwin_exit_reason why;
    T1H_CHECK(hostwin_bind(probe_hw, probe_v, page, &tag) == HOSTWIN_OK);
    T1H_CHECK(hostwin_resolve(probe_hw, probe_v, page, 8, true, &why) == HOSTWIN_MAPPED);
}
static void update_exclusion(void)
{
    struct hostwin_tag tag;
    /* hostwin_enter: PSR.pk is a field fills depend on, so the update
     * drops every shadow (drop_everything). */
    probe_fill(data_page(1));
    tracker_start(probe_hw, data_page(1));
    probe_v->s.psr ^= IA64_PSR_PK;
    __atomic_store_n(&unmap_armed, 1, __ATOMIC_RELEASE);
    hostwin_enter(probe_hw, probe_v);
    T1H_CHECK(!__atomic_load_n(&unmap_armed, __ATOMIC_ACQUIRE));
    tracker_join();
    /* hostwin_bind: a third tag rebinds the least recently bound slot,
     * the one holding the shadow. */
    probe_fill(data_page(2));
    T1H_CHECK(hostwin_bind(probe_hw, probe_v, UINT64_C(1) << 36, &tag) == HOSTWIN_OK);
    tracker_start(probe_hw, data_page(2));
    __atomic_store_n(&unmap_armed, 1, __ATOMIC_RELEASE);
    T1H_CHECK(hostwin_bind(probe_hw, probe_v, UINT64_C(2) << 36, &tag) == HOSTWIN_OK);
    T1H_CHECK(!__atomic_load_n(&unmap_armed, __ATOMIC_ACQUIRE));
    tracker_join();
    /* hostwin_invalidate. */
    probe_fill(data_page(3));
    tracker_start(probe_hw, data_page(3));
    __atomic_store_n(&unmap_armed, 1, __ATOMIC_RELEASE);
    hostwin_invalidate(probe_hw);
    T1H_CHECK(!__atomic_load_n(&unmap_armed, __ATOMIC_ACQUIRE));
    tracker_join();
}
/* A link names a region only while it is
 * valid. The executor resolves a stale link after its quiescent
 * publication, and a helper's maintenance pass runs right after the
 * resolution: whatever the resolution returned must still be allocated. */
struct t1_region *__real_t1_region_from_link(uint64_t link);
struct t1_region *__wrap_t1_region_from_link(uint64_t link);
static _Thread_local bool reclaim_after_resolve, freed_under_caller;
struct t1_region *__wrap_t1_region_from_link(uint64_t link)
{
    struct t1_region *r = __real_t1_region_from_link(link);
    if (!r || !reclaim_after_resolve)
        return r;
    uint32_t id = r->id;
    t1_reclaim();
    if (t1_region_get(id) == r)
        return r;
    freed_under_caller = true; /* never hand the freed record on */
    return NULL;
}
static void link_lifetime(void)
{
    uint8_t ret = 0xc3;
    struct t1_emitted e = {.buf = &ret, .size = 1};
    struct t1_region *r = t1_region_new();
    T1H_CHECK(r && t1_arena_place(r, &e));
    t1_arena_seal_for_entry(r);
    T1H_CHECK(t1_region_publish(r));
    uint64_t link = t1_region_link(r), stale = link;
    t1_region_retire(r);
    code_quiescent(v);
    reclaim_after_resolve = true;
    struct vcpu_exit out;
    T1H_CHECK(t1_upper_enter(v, &stale, 1000, &out) == T1_UPPER_DECLINED && !stale);
    reclaim_after_resolve = false;
    T1H_CHECK(!freed_under_caller);
    T1H_CHECK(!t1_region_from_link(link));
    t1_reclaim();
}
/* Window statistics read by another thread while the owner's no-change
 * path of hostwin_enter counts outside the instance lock (TSan). */
static unsigned stats_stop;
static void *read_window_stats(void *arg)
{
    (void)arg;
    struct hostwin_stats st;
    while (!__atomic_load_n(&stats_stop, __ATOMIC_ACQUIRE)) {
        hostwin_get_stats(probe_hw, &st);
        sched_yield();
    }
    return NULL;
}
static void stats_while_entering(void)
{
    struct hostwin_stats before, after;
    hostwin_enter(probe_hw, probe_v);
    hostwin_get_stats(probe_hw, &before);
    pthread_t reader;
    T1H_CHECK(!pthread_create(&reader, NULL, read_window_stats, NULL));
    for (unsigned i = 0; i < 100000; ++i)
        hostwin_enter(probe_hw, probe_v);
    __atomic_store_n(&stats_stop, 1, __ATOMIC_RELEASE);
    T1H_CHECK(!pthread_join(reader, NULL));
    hostwin_get_stats(probe_hw, &after);
    T1H_CHECK(after.context_unchanged == before.context_unchanged + 100000);
}
/* A region that was valid when its
 * link was resolved stays allocated, although another thread retires it
 * and reclaims, until the resolver's next quiescent publication. */
static struct t1_region *late;
static void *retire_and_reclaim(void *arg)
{
    (void)arg;
    t1_region_retire(late);
    t1_reclaim();
    return NULL;
}
static void link_grace(void)
{
    uint8_t ret = 0xc3;
    struct t1_emitted e = {.buf = &ret, .size = 1};
    struct t1_region *r = t1_region_new();
    T1H_CHECK(r && t1_arena_place(r, &e));
    t1_arena_seal_for_entry(r);
    T1H_CHECK(t1_region_publish(r));
    uint64_t link = t1_region_link(r);
    code_quiescent(v);
    late = t1_region_from_link(link);
    T1H_CHECK(late == r);
    uint32_t id = r->id;
    pthread_t thread;
    T1H_CHECK(!pthread_create(&thread, NULL, retire_and_reclaim, NULL));
    T1H_CHECK(!pthread_join(thread, NULL));
    T1H_CHECK(t1_region_get(id) == r && !r->valid);
    code_quiescent(v);
    t1_reclaim();
    T1H_CHECK(!t1_region_get(id));
}
static void many_regions(const struct t1_compile_request *original)
{
    uint8_t page[4096];
    T1H_CHECK(gm_read(original->page[0].pa, page, sizeof(page)));
    uint64_t start = hal_counter();
    for (unsigned i = 0; i < 96; ++i) {
        struct t1_compile_request rq = *original;
        uint64_t offset = (uint64_t)(i + 1) * 4096;
        rq.ip += offset;
        rq.entry_pa += offset;
        rq.npages = 1;
        rq.page[0].va += offset;
        rq.page[0].pa += offset;
        rq.page[0].generation = 0;
        rq.key += offset;
        T1H_CHECK(gm_write(rq.page[0].pa, page, sizeof(page)));
        memcpy(expected.ram + rq.page[0].pa - T1H_RAM_BASE, page, sizeof(page));
        while (!helper_submit_t1(&rq, NULL))
            sched_yield();
    }
    uint64_t saved_ip = v->s.ip;
    for (unsigned i = 0; i < 96; ++i) {
        uint64_t offset = (uint64_t)(i + 1) * 4096;
        v->s.ip = original->ip + offset;
        while (!t1_upper_probe(v, original->entry_pa + offset)) {
            T1H_CHECK(hal_counter() - start < 20 * hal_counter_hz());
            sched_yield();
        }
    }
    v->s.ip = saved_ip;
    printf("many-regions=96 compile_us=%.3f\n",
           (double)(hal_counter() - start) * 1e6 / hal_counter_hz());
}
/* Two real workers must hold distinct pipeline state at the same time.
 * The copied loop has the same behaviour and a separate source page. */
static void parallel_contract(struct t1_vcpu *tv, const struct t1_compile_request *original)
{
    struct t1_compile_request rq[2] = {*original, *original};
    uint8_t page[4096];
    T1H_CHECK(gm_read(original->page[0].pa, page, sizeof(page)));
    rq[1].ip += 4096;
    rq[1].entry_pa += 4096;
    rq[1].npages = 1;
    rq[1].page[0].va += 4096;
    rq[1].page[0].pa += 4096;
    rq[1].page[0].generation = 0;
    rq[1].key += 4096;
    T1H_CHECK(gm_write(rq[1].page[0].pa, page, sizeof(page)));
    memcpy(expected.ram + rq[1].page[0].pa - T1H_RAM_BASE, page, sizeof(page));
    memcpy(snap.ram + rq[1].page[0].pa - T1H_RAM_BASE, page, sizeof(page));
    __atomic_store_n(&parallel_gate, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 2; ++i)
        T1H_CHECK(helper_submit_t1(&rq[i], NULL));
    uint64_t start = hal_counter();
    while (__atomic_load_n(&parallel_entered, __ATOMIC_ACQUIRE) != 2) {
        T1H_CHECK(hal_counter() - start < 10 * hal_counter_hz());
        sched_yield();
    }
    uint64_t block = (t1_compile_workspace_bytes() + 4095) / 4096 * 4096;
    struct t1_memory_stats memory;
    t1_memory_get_stats(&memory);
    T1H_CHECK(memory.reserve_bytes == 2 * block && memory.workspace_bytes == 2 * block);
    struct t1_compile_ctx *ctx[3];
    for (unsigned i = 0; i < 3; ++i) {
        ctx[i] = t1_compile_ctx_create();
        T1H_CHECK(ctx[i]);
    }
    T1H_CHECK(!t1_compile_begin(ctx[2], original, true));
    T1H_CHECK(t1_compile_at(tv, &(struct t1_compile_options){0}, NULL) == T1_RETRY);
    for (unsigned i = 0; i < 2; ++i)
        T1H_CHECK(!helper_submit_t1(&rq[i], NULL));
    __atomic_store_n(&parallel_release, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 2; ++i) {
        v->s.ip = rq[i].ip;
        while (!t1_upper_probe(v, rq[i].entry_pa)) {
            T1H_CHECK(hal_counter() - start < 10 * hal_counter_hz());
            sched_yield();
        }
    }
    __atomic_store_n(&parallel_gate, 0, __ATOMIC_RELEASE);
    v->s.ip = original->ip;
    t1h_tier0t1_set_flags(0);
    struct vcpu_exit out = t1h_engine_run(&ev, UINT64_C(50000000));
    result_take(&actual, &out, v);
    T1H_CHECK(!result_compare(&expected, &actual, false, "parallel helper handoff"));
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    v->upper = tv;
    helper_quiesce();
    t1_flush();
    code_quiescent(v);
    t1_reclaim();
    frozen = true;
    /* Cancelling one owner must leave the other reservation and workspace
     * intact. A third context may replace only the cancelled one. */
    T1H_CHECK(t1_compile_begin(ctx[0], &rq[0], true));
    T1H_CHECK(t1_compile_begin(ctx[1], &rq[1], true));
    T1H_CHECK(!t1_compile_begin(ctx[2], &rq[0], true));
    T1H_CHECK(t1_compile_step(ctx[0], NULL) && t1_compile_step(ctx[1], NULL));
    t1_compile_cancel(ctx[0]);
    t1_compile_trim(ctx[0]);
    T1H_CHECK(t1_compile_begin(ctx[2], &rq[0], true));
    T1H_CHECK(!t1_compile_begin(ctx[0], &rq[0], true));
    t1_compile_cancel(ctx[2]);
    struct t1_compile_report rep[2];
    while (t1_compile_step(ctx[1], &rep[1])) {
    }
    T1H_CHECK(!t1_upper_probe(v, rq[1].entry_pa));
    code_notify_vmm_write(rq[1].entry_pa, 16);
    t1_compile_flush(ctx[1]);
    T1H_CHECK(!t1_region_get(rep[1].region_id));
    /* Flush one context without publishing or rewriting the other's code. */
    for (unsigned i = 0; i < 2; ++i) {
        T1H_CHECK(t1_compile_begin(ctx[i], &rq[i], true));
        while (t1_compile_step(ctx[i], &rep[i])) {
        }
    }
    struct t1_region *a = t1_region_get(rep[0].region_id), *b = t1_region_get(rep[1].region_id);
    T1H_CHECK(a && b && !a->valid && !b->valid);
    bool b_sealed = b->sealed;
    if (a->chunk == b->chunk) {
        T1H_CHECK(a->sealed && b->sealed);
        uintptr_t a_first = (uintptr_t)a->code / 4096;
        uintptr_t a_last = ((uintptr_t)a->code + a->code_size - 1) / 4096;
        uintptr_t b_first = (uintptr_t)b->code / 4096;
        uintptr_t b_last = ((uintptr_t)b->code + b->code_size - 1) / 4096;
        T1H_CHECK(a_last < b_first || b_last < a_first);
    }
    if (spans)
        T1H_CHECK(a->chunk == b->chunk && a->sealed && b->sealed);
    t1_compile_flush(ctx[0]);
    T1H_CHECK(a->valid && a->sealed && !b->valid && b->sealed == b_sealed);
    t1_compile_cancel(ctx[1]);
    t1_compile_trim(ctx[1]);
    T1H_CHECK(a->valid && !t1_region_get(rep[1].region_id));
    t1_flush();
    code_quiescent(v);
    t1_reclaim();
    /* Two workspaces fit, but region metadata does not: both compiles
     * defer, release their temporary state, and recover with more room. */
    for (unsigned i = 0; i < 2; ++i)
        t1_compile_trim(ctx[i]);
    t1_memory_get_stats(&memory);
    uint64_t limit = memory.budget_bytes;
    t1_memory_set_limit(memory.used_bytes + 2 * block);
    for (unsigned i = 0; i < 2; ++i) {
        T1H_CHECK(t1_compile_begin(ctx[i], &rq[i], true));
        T1H_CHECK(t1_compile_step(ctx[i], NULL));
    }
    for (unsigned i = 0; i < 2; ++i) {
        while (t1_compile_step(ctx[i], &rep[i])) {
        }
        T1H_CHECK(rep[i].reject_reason &&
                  !strcmp(rep[i].reject_reason, "no memory for region tables"));
        t1_compile_trim(ctx[i]);
    }
    t1_memory_get_stats(&memory);
    T1H_CHECK(!memory.workspace_bytes && memory.used_bytes <= memory.budget_bytes);
    t1_memory_set_limit(limit);
    t1_reclaim();
    for (unsigned i = 0; i < 2; ++i) {
        T1H_CHECK(t1_compile_begin(ctx[i], &rq[i], true));
        while (t1_compile_step(ctx[i], &rep[i])) {
        }
        T1H_CHECK(rep[i].region_id);
        t1_compile_flush(ctx[i]);
        T1H_CHECK(t1_region_get(rep[i].region_id)->valid);
    }
    for (unsigned i = 0; i < 3; ++i)
        t1_compile_ctx_destroy(ctx[i]);
    t1_flush();
    code_quiescent(v);
    t1_reclaim();
    frozen = false;
    helper_resume();
    puts(
        "parallel helpers: two active pipelines, bounded admission, cancel, isolated batches and memory recovery");
}
static void execute(void *arg)
{
    (void)arg;
    struct t1_vcpu *tv = t1h_tier1_vcpu(ev.ctx);
    v->upper = tv;
    const struct t1_compile_options opt = {0};
    t1_test_set_compile_options(tv, &opt);
    struct t1_compile_request rq;
    T1H_CHECK(t1_request_make(tv, v->s.ip & ~(UINT64_C(1) << 63), true, &rq));
    source = rq.entry_pa;
    const char *contracts = getenv("HELPER_CONTRACTS");
    if (contracts && !strcmp(contracts, "running"))
        execution_grace();
    else if (contracts && !strcmp(contracts, "allocation"))
        allocation_while_running();
    else if (contracts && !strcmp(contracts, "grace"))
        link_grace();
    else if (contracts && !strcmp(contracts, "yes")) {
        reclaim_tail();
        batch_contract(tv, &rq);
        execution_grace();
        allocation_while_running();
        tracking_fill(tv->hw);
        update_exclusion();
        stats_while_entering();
        link_lifetime();
        link_grace();
    }
    if (parallel)
        parallel_contract(tv, &rq);
    if (many)
        many_regions(&rq);
    if (pressure && !synchronous) {
        uint64_t previous = __atomic_load_n(t1_upper_sequence(), __ATOMIC_ACQUIRE);
        T1H_CHECK(helper_submit_t1(&rq, NULL));
        while (__atomic_load_n(t1_upper_sequence(), __ATOMIC_ACQUIRE) == previous)
            sched_yield();
        /* The sole record is retired but pinned by this executor. Helpers
         * must decline, release code_lock, and let its next boundary pass. */
        T1H_CHECK(!t1_upper_probe(v, rq.entry_pa));
        code_quiescent(v);
    }
    uint64_t sequence = __atomic_load_n(t1_upper_sequence(), __ATOMIC_ACQUIRE);
    uint64_t start = hal_counter();
    if (synchronous) {
        struct t1_compile_report rep;
        if (pressure) {
            /* The sole record is retired but pinned by this executor: the
             * compile is deferred until the executor's next boundary. */
            T1H_CHECK(t1_compile_at(tv, &opt, &rep) == T1_NO_MEMORY);
            code_quiescent(v);
        }
        T1H_CHECK(t1_compile_at(tv, &opt, &rep) == T1_COMPILED);
    } else
        while (!helper_submit_t1(&rq, NULL))
            sched_yield();
    submission = hal_counter() - start;
    if (concurrent) {
        while (!__atomic_load_n(&compiler_entered, __ATOMIC_ACQUIRE))
            sched_yield();
        uint64_t retired = v->retired;
        struct vcpu_exit guest = tier0_run(v, 1000);
        T1H_CHECK(guest.reason == VCPU_EXIT_BUDGET && v->retired >= retired + 1000);
        __atomic_store_n(&compiler_release, 1, __ATOMIC_RELEASE);
        do
            guest = tier0_run(v, UINT64_C(50000000));
        while (guest.reason == VCPU_EXIT_BUDGET);
        result_take(&actual, &guest, v);
        T1H_CHECK(!result_compare(&expected, &actual, false, "concurrent Tier0"));
        snapshot_restore(&snap, v);
        t1h_engine_restored(&ev);
        v->upper = tv;
    }
    /* Only the executor probes and links; a helper never runs guest code. */
    uint64_t link;
    do {
        link = t1_upper_probe(v, rq.entry_pa);
        if (!link)
            sched_yield();
        T1H_CHECK(hal_counter() - start < 10 * hal_counter_hz());
    } while (!link);
    while (__atomic_load_n(t1_upper_sequence(), __ATOMIC_ACQUIRE) == sequence) {
        T1H_CHECK(hal_counter() - start < 10 * hal_counter_hz());
        sched_yield();
    }
    elapsed = hal_counter() - start;
    /* The executor's context keeps its workspace block once it compiled
     * (synchronously here, or before the helpers started in the pressure
     * modes); a helper returns its block before releasing its compiler
     * slot. */
    uint64_t block = (t1_compile_workspace_bytes() + 4095) / 4096 * 4096;
    uint64_t held = synchronous || pressure ? block : 0;
    struct t1_memory_stats memory;
    for (;;) {
        t1_memory_get_stats(&memory);
        if (memory.workspace_bytes == held)
            break;
        T1H_CHECK(hal_counter() - start < 10 * hal_counter_hz());
        sched_yield();
    }
    struct t1_region *r = t1_region_from_link(link);
    T1H_CHECK(r && r->valid && (synchronous || r->sealed));
    if (spans)
        T1H_CHECK(r->code >= span_base + 4096 &&
                  r->code + r->code_size <= span_base + T1_CHUNK_BYTES);
    if (pressure)
        T1H_CHECK(link != old_link && !t1_region_from_link(old_link));
    if (race) {
        T1H_CHECK(__atomic_load_n(&seals, __ATOMIC_RELAXED) >= 2);
        T1H_CHECK(r->page[0].generation == code_source_generation(source));
    }
    t1h_tier0t1_set_flags(0);
    struct vcpu_exit e = t1h_engine_run(&ev, UINT64_C(50000000));
    result_take(&actual, &e, v);
    T1H_CHECK(!result_compare(&expected, &actual, false, "helper handoff"));
    code_quiescent(v);
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&vm->cpu[0].state, VM_CPU_STOPPED, __ATOMIC_RELEASE);
    hal_cpu_request(0, HAL_CPU_WAKE);
}
static void icount_contract(void)
{
    const struct guest_program *p = guest_program("gather_phys");
    struct vcpu_config cfg = guest_vcpu_config(p);
    T1H_CHECK(cfg.itc_icount);
    struct vcpu *ref = vcpu_create(&cfg);
    v = vcpu_create(&cfg);
    T1H_CHECK(ref && v);
    t1h_bind(ref, &ref_logs);
    t1h_bind(v, &logs);
    guest_setup(p, &(struct guest_params){.n = 7, .passes = 2}, ref);
    snapshot_init(&snap);
    snapshot_take(&snap, ref);
    result_init(&expected);
    result_init(&actual);
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    struct snapshot base = snap;
    const unsigned boundaries[] = {0, 7, 40, 119};
    for (unsigned requested = 0; requested < 2; ++requested) {
        T1H_CHECK(!helper_start(requested, cfg.itc_icount, true));
        T1H_CHECK(!helper_active());
        t1_cfg.submit = NULL;
        for (unsigned timer = 0; timer < 2; ++timer) {
            for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
                unsigned k = boundaries[i];
                struct schedule schedule;
                schedule_clear(&schedule);
                snap.v = base.v;
                if (timer) {
                    snap.v.s.cr[CR_ITV] = 0xef;
                    snap.v.s.cr[CR_ITM] = ia64_itc_read(&snap.v) + k;
                    ia64_timer_program(&snap.v);
                } else
                    schedule_add(&schedule,
                                 (struct injection){.at = k, .kind = INJ_RAISE, .address = 0x40});
                snapshot_restore(&snap, ref);
                struct vcpu_exit e = ref_run(ref, UINT64_C(50000000), &schedule);
                T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && t1h_bank0(ref, 24) == 1);
                T1H_CHECK(t1h_bank0(ref, 25) == k);
                result_take(&expected, &e, ref);
                snapshot_restore(&snap, v);
                t1h_engine_restored(&ev);
                schedule_rearm(&schedule);
                e = t1h_engine_run_sched(&ev, UINT64_C(50000000), &schedule);
                T1H_CHECK(t1h_bank0(v, 25) == k);
                result_take(&actual, &e, v);
                T1H_CHECK(!result_compare(&expected, &actual, false, "icount helper policy"));
            }
        }
    }
    t1h_engine_release(&ev);
    t1h_unbind(ref);
    t1h_unbind(v);
    vcpu_destroy(ref);
    vcpu_destroy(v);
    t1_shutdown();
    tier0_destroy();
    code_cache_flush();
    snapshot_free(&snap);
    result_free(&expected);
    result_free(&actual);
    puts("helper icount: interrupt and ITC delivery match synchronous boundaries (helpers on/off)");
}
int main(int argc, char **argv)
{
    alarm(40);
    T1H_CHECK(argc >= 3);
    fallback = !strncmp(argv[1], "management", 10);
    synchronous = !strncmp(argv[1], "synchronous", 11);
    T1H_CHECK(fallback || synchronous || !strncmp(argv[1], "helper", 6));
    delay = strstr(argv[1], "delay") != NULL;
    race = strstr(argv[1], "race") != NULL;
    pressure = strstr(argv[1], "pressure") != NULL;
    many = strstr(argv[1], "many") != NULL;
    concurrent = strstr(argv[1], "concurrent") != NULL;
    parallel = strstr(argv[1], "parallel") != NULL;
    spans = strstr(argv[1], "spans") != NULL;
    unsigned pins[4];
    const char *pin_list = getenv("HELPER_PIN_CPUS");
    if (pin_list) {
        char *end;
        for (unsigned i = 0; i < 4; ++i) {
            pins[i] = (unsigned)strtoul(pin_list, &end, 10);
            T1H_CHECK(end != pin_list && (i == 3 ? !*end : *end == ','));
            pin_list = end + (i != 3);
        }
    }
    T1H_CHECK(!hal_linux_configure(true, 1, (fallback || synchronous) ? 0 : 2,
                                   pin_list ? pins : NULL,
                                   pin_list ? ((fallback || synchronous) ? 2 : 4) : 0));
    T1H_CHECK(hostwin_linux_init());
    t1_test_workspace_poison = true;
    t1h_set_images(argc - 2, argv + 2);
    t1h_init();
    if (strstr(argv[1], "icount")) {
        icount_contract();
        return 0;
    }
    result_init(&expected);
    result_init(&actual);
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *ref = vcpu_create(&cfg);
    v = vcpu_create(&cfg);
    T1H_CHECK(ref && v);
    t1h_bind(ref, &ref_logs);
    t1h_bind(v, &logs);
    t1h_load_image("nat_guard_decline.bin");
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[20] = 7;
    regs.gr[8] = 10000;
    vcpu_reset(ref, &regs);
    uint64_t header;
    T1H_CHECK(t1h_run_to_loop_header(ref, 100000, &header));
    snapshot_init(&snap);
    snapshot_take(&snap, ref);
    struct vcpu_exit e = ref_run(ref, UINT64_C(50000000), NULL);
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&expected, &e, ref);
    if (pressure)
        T1H_CHECK(t1_init(
            &(struct t1_config){.max_regions = 1, .loop_threshold = 2, .trace_threshold = 8}));
    if (spans)
        T1H_CHECK(t1_init(&(struct t1_config){
            .code_bytes = 1u << 20, .loop_threshold = 2, .trace_threshold = 8}));
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    T1H_CHECK(tier0_configure(TIER0_CODE_DEFAULT_BYTES));
    if (pressure) {
        struct t1_compile_report rep;
        struct t1_compile_options opt = {0};
        T1H_CHECK(t1_compile_at(t1h_tier1_vcpu(ev.ctx), &opt, &rep) == T1_COMPILED);
        /* Close the compiled region's chunk with an unpublished filler;
         * only the real region remains when the retirement is pinned. */
        static struct t1_region padding;
        static struct t1_emitted emitted;
        emitted.buf = calloc(1, T1_CHUNK_BYTES);
        emitted.size = T1_CHUNK_BYTES;
        T1H_CHECK(emitted.buf && t1_arena_place(&padding, &emitted));
        t1_arena_seal_for_entry(&padding);
        t1_arena_release(&padding);
        free(emitted.buf);
        old_link = t1_region_link(t1_region_get(rep.region_id));
        code_register_vcpu(v);
        t1_region_retire(t1_region_get(rep.region_id));
    } else
        code_register_vcpu(v);
    if (spans)
        span_setup();
    const char *contracts = getenv("HELPER_CONTRACTS");
    if (contracts && !strcmp(contracts, "yes"))
        probe_create();
    struct t1_stats before;
    t1_get_stats(&before);
    compile_ticks_before = before.compile_ticks;
    executor_ticks_before = engine_tier1_glue()->executor_compile_ticks();
    t1_cfg.submit = synchronous ? NULL : helper_submit_t1;
    if (!synchronous)
        T1H_CHECK(helper_init(hal_helper_count()));
    code_background_set(helper_quiesce, helper_resume);
    if (race)
        code_validity_set_test_hook(source_race);
    vm = vm_create(1, NULL, false);
    T1H_CHECK(vm);
    vm->cpu[0].v = v;
    vm->cpu[0].host_cpu = 1;
    vm->cpu[0].state = VM_CPU_RUNNING;
    vm->cpu[0].code = 0;
    vm->first_terminal = 1;
    T1H_CHECK(hal_cpu_start(1, execute, NULL));
    T1H_CHECK(!vm_wait(vm, hal_counter() + 20 * hal_counter_hz()));
    T1H_CHECK(__atomic_load_n(&done, __ATOMIC_ACQUIRE));
    vm_destroy(vm);
    if (probe_hw) {
        hostwin_destroy(probe_hw);
        vcpu_destroy(probe_v);
    }
    code_background_set(NULL, NULL);
    code_validity_set_test_hook(NULL);
    code_unregister_vcpu(v);
    struct t1_stats after;
    t1_get_stats(&after);
    if (synchronous && IA64VMM_TIER1_STATS)
        T1H_CHECK(engine_tier1_glue()->executor_compile_ticks() > executor_ticks_before);
    else
        T1H_CHECK(engine_tier1_glue()->executor_compile_ticks() == executor_ticks_before);
    if (IA64VMM_TIER1_STATS)
        T1H_CHECK(after.compile_ticks > compile_ticks_before);
    t1_cfg.submit = NULL;
    if (spans)
        span_cleanup();
    t1h_engine_release(&ev);
    t1h_unbind(ref);
    t1h_unbind(v);
    vcpu_destroy(ref);
    vcpu_destroy(v);
    t1_shutdown();
    tier0_destroy();
    code_cache_flush();
    snapshot_free(&snap);
    result_free(&expected);
    result_free(&actual);
    struct helper_stats hs;
    helper_get_stats(&hs);
    printf("%s request-to-link_us=%.3f submit_us=%.3f max-step_us=%.3f result=reference\n", argv[1],
           (double)elapsed * 1e6 / hal_counter_hz(), (double)submission * 1e6 / hal_counter_hz(),
           (double)hs.max_step_ticks * 1e6 / hal_counter_hz());
    return 0;
}
