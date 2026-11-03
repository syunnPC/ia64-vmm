/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Behavioural contracts of the compile queue. The fake
 * compiler has six preemption points and publishes only at its last one. */
#define _DEFAULT_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "core/helper.h"
#include "engine/tier1/tier1.h"
#include "ia64vmm/hal.h"
#include "tests/hal/hal_linux.h"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
static unsigned create_count, reported_helpers;
unsigned __wrap_hal_helper_count(void)
{
    return reported_helpers ? reported_helpers : 3;
}
static unsigned empty_arm, empty_inside, empty_release, missed, post_in_submit, create_fail;
static unsigned parallel_arm, parallel_blocked, active;
void helper_test_event(unsigned event)
{
    if (event == 0 && hal_cpu_index() < 4 && LOAD(empty_arm)) {
        __atomic_fetch_or(&empty_inside, 1u << hal_cpu_index(), __ATOMIC_RELEASE);
        /* Consume the wakes that brought this worker here: after the release
         * only a slot release may wake a worker for the next request. */
        while (!LOAD(empty_release)) {
            hal_cpu_poll();
            sched_yield();
        }
    }
    if (event == 1 && hal_cpu_index() == 4 && LOAD(empty_inside) == 12 && !LOAD(empty_release)) {
        hal_cpu_poll();
        STORE(missed, 1);
    }
    if (event == 1 && LOAD(parallel_arm))
        STORE(parallel_blocked, 1);
    if (event == 2 && LOAD(post_in_submit)) {
        STORE(post_in_submit, 0);
        CHECK(helper_post(HELPER_RECLAIM, 0));
    }
}
static unsigned begun, stages, published, cancelled, block_stage, inside, release_stage;
static uint64_t last_key, last_ip;
static unsigned compiled_on, maintenance_on, maintenance_count, rank3only;
struct t1_compile_ctx {
    unsigned stage;
    struct t1_compile_request rq;
};
struct t1_compile_ctx *t1_compile_ctx_create(void)
{
    ++create_count;
    return create_fail ? NULL : calloc(1, sizeof(struct t1_compile_ctx));
}
void t1_compile_ctx_destroy(struct t1_compile_ctx *c)
{
    CHECK(!c->stage);
    free(c);
}
bool t1_compile_begin(struct t1_compile_ctx *c, const struct t1_compile_request *r, bool policy)
{
    CHECK(policy && !c->stage);
    c->rq = *r;
    c->stage = 1;
    __atomic_add_fetch(&begun, 1, __ATOMIC_RELAXED);
    CHECK(__atomic_add_fetch(&active, 1, __ATOMIC_RELEASE) <= 2);
    return true;
}
bool t1_compile_step(struct t1_compile_ctx *c, struct t1_compile_report *report)
{
    (void)report;
    CHECK(c->stage);
    STORE(inside, 1);
    while (LOAD(block_stage) && !LOAD(release_stage))
        sched_yield();
    STORE(inside, 0);
    __atomic_add_fetch(&stages, 1, __ATOMIC_RELAXED);
    if (++c->stage <= 6)
        return true;
    STORE(last_key, c->rq.key);
    STORE(last_ip, c->rq.ip);
    STORE(compiled_on, hal_cpu_index());
    c->stage = 0;
    __atomic_sub_fetch(&active, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&published, 1, __ATOMIC_RELEASE);
    return false;
}
void t1_compile_flush(struct t1_compile_ctx *c)
{
    (void)c;
}
void t1_compile_dropped(const struct t1_compile_request *r)
{
    (void)r;
    CHECK(!"no request is dropped here");
}
void t1_compile_trim(struct t1_compile_ctx *c)
{
    CHECK(!c->stage);
}
void t1_compile_cancel(struct t1_compile_ctx *c)
{
    if (c && c->stage) {
        c->stage = 0;
        __atomic_sub_fetch(&active, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&cancelled, 1, __ATOMIC_RELEASE);
    }
}
void tier0_reclaim_batch(unsigned limit)
{
    CHECK(limit == 64);
    STORE(maintenance_on, hal_cpu_index());
    __atomic_add_fetch(&maintenance_count, 1, __ATOMIC_RELEASE);
}
void t1_reclaim(void) {}
unsigned __wrap_hal_helper_rank(unsigned i)
{
    return i >= 3 ? 0 : rank3only || i == 2 ? 3 : 1;
}
static struct t1_compile_request request(uint64_t key)
{
    return (struct t1_compile_request){.key = key, .ip = key * 16, .npages = 1};
}
static void wait_for_at(unsigned *p, unsigned n, unsigned line)
{
    uint64_t deadline = hal_counter() + 10 * hal_counter_hz();
    while (__atomic_load_n(p, __ATOMIC_ACQUIRE) < n) {
        if (hal_counter() >= deadline) {
            fprintf(stderr, "%s:%u: waited 10 s for %u (at %u)\n", __FILE__, line, n,
                    __atomic_load_n(p, __ATOMIC_ACQUIRE));
            exit(1);
        }
        sched_yield();
    }
}
#define wait_for(p, n) wait_for_at((p), (n), __LINE__)
static void fallback(void)
{
    CHECK(helper_init(0));
    struct t1_compile_request r = request(11);
    unsigned before = LOAD(published), start = LOAD(stages);
    CHECK(helper_submit_t1(&r, NULL));
    r.ip = 0;
    r.key = 99;
    r = request(11);
    CHECK(!helper_submit_t1(&r, NULL));
    CHECK(LOAD(stages) == start);
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(helper_step());
        CHECK(LOAD(stages) == start + i + 1);
        CHECK(LOAD(published) == before);
        CHECK(!helper_submit_t1(&r, NULL));
    }
    helper_step();
    CHECK(LOAD(published) == before + 1);
    CHECK(LOAD(last_ip) == 176 && LOAD(last_key) == 11 && LOAD(compiled_on) == 0);
    CHECK(!helper_step());
    CHECK(helper_submit_t1(&r, NULL));
    CHECK(helper_step());
    helper_quiesce();
    CHECK(LOAD(cancelled));
    CHECK(!helper_step() && !helper_submit_t1(&r, NULL));
    helper_resume();
    CHECK(helper_submit_t1(&r, NULL));
    helper_shutdown();
    CHECK(!helper_submit_t1(&r, NULL) && !helper_step());
    CHECK(helper_init(0));
    for (unsigned i = 1; i <= 256; ++i) {
        r = request(i);
        CHECK(helper_submit_t1(&r, NULL));
    }
    r = request(257);
    CHECK(!helper_submit_t1(&r, NULL));
    start = LOAD(published);
    for (unsigned i = 0; helper_step(); ++i)
        CHECK(i < 256 * 7);
    CHECK(LOAD(published) == start + 256);
    /* Exercise wraparound and key reuse, rather than just filling once. */
    for (unsigned i = 0; i < 512; ++i) {
        r = request(i + 1);
        CHECK(helper_submit_t1(&r, NULL));
        while (helper_step()) {
        }
    }
    helper_shutdown();
}
/* helper.h statistics: an accepted submission counts with the queue depth
 * it found, every declined one (in-flight key, full queue, paused queue)
 * as refused; completed compiles count with their step time. */
static void statistics(void)
{
    CHECK(helper_init(0));
    struct helper_stats s;
    helper_get_stats(&s);
    CHECK(!s.submitted && !s.refused && !s.queued && !s.completed && !s.compile_ticks &&
          !s.wait_ticks);
    struct t1_compile_request r;
    for (unsigned i = 1; i <= 256; ++i) {
        r = request(i);
        CHECK(helper_submit_t1(&r, NULL));
    }
    r = request(257);
    CHECK(!helper_submit_t1(&r, NULL));
    r = request(1);
    CHECK(!helper_submit_t1(&r, NULL));
    helper_get_stats(&s);
    CHECK(s.submitted == 256 && s.refused == 2 && s.queued == 255 * 256 / 2 && !s.completed);
    while (helper_step()) {
    }
    helper_get_stats(&s);
    CHECK(s.completed == 256 && s.compile_ticks > 0 && !s.maintenance_ticks);
    /* Waiting time is measured only with the statistics gate. */
    CHECK(IA64VMM_TIER1_STATS ? s.wait_ticks > 0 : !s.wait_ticks);
    helper_quiesce();
    r = request(300);
    CHECK(!helper_submit_t1(&r, NULL));
    helper_resume();
    helper_get_stats(&s);
    CHECK(s.submitted == 256 && s.refused == 3);
    helper_shutdown();
    helper_get_stats(&s);
    CHECK(s.submitted == 256 && s.refused == 3 && s.completed == 256);
}
static void *unblock(void *arg)
{
    (void)arg;
    usleep(10000);
    STORE(release_stage, 1);
    return NULL;
}
static void *producer(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < 200; ++i) {
        struct t1_compile_request r = request(10000 + id * 200 + i);
        while (!helper_submit_t1(&r, NULL))
            sched_yield();
    }
    return NULL;
}
static unsigned other_started, other_release;
static void other_work(void *arg)
{
    (void)arg;
    STORE(other_started, 1);
    while (!LOAD(other_release))
        sched_yield();
}
/* Both slots are occupied before a third request is queued. Quiescence
 * cancels both owners, and a resume completes the queued request. */
static void parallel_workers(void)
{
    CHECK(helper_init(3));
    unsigned before = LOAD(published), cancelled_before = LOAD(cancelled);
    STORE(block_stage, 1);
    STORE(release_stage, 0);
    STORE(parallel_arm, 1);
    struct t1_compile_request a = request(1201), b = request(1202), c = request(1203);
    CHECK(helper_submit_t1(&a, NULL) && helper_submit_t1(&b, NULL));
    wait_for(&active, 2);
    CHECK(helper_submit_t1(&c, NULL));
    wait_for(&parallel_blocked, 1);
    CHECK(LOAD(active) == 2 && LOAD(published) == before);
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, unblock, NULL));
    helper_quiesce();
    CHECK(!pthread_join(thread, NULL));
    CHECK(!LOAD(active) && LOAD(cancelled) == cancelled_before + 2);
    STORE(parallel_arm, 0);
    STORE(block_stage, 0);
    helper_resume();
    wait_for(&published, before + 1);
    CHECK(!LOAD(active));
    helper_shutdown();
}
static void workers(void)
{
    CHECK(helper_init(hal_helper_count()));
    for (unsigned i = 0; i < 3; ++i)
        CHECK(hal_cpu_busy(hal_helper_cpu(i)));
    unsigned before = LOAD(published);
    struct t1_compile_request r = request(1234);
    STORE(block_stage, 1);
    STORE(release_stage, 0);
    CHECK(helper_submit_t1(&r, NULL));
    wait_for(&inside, 1);
    CHECK(!helper_submit_t1(&r, NULL));
    CHECK(LOAD(published) == before);
    pthread_t t;
    CHECK(!pthread_create(&t, NULL, unblock, NULL));
    helper_quiesce();
    CHECK(!pthread_join(t, NULL));
    unsigned stopped = LOAD(stages);
    for (unsigned i = 0; i < 1000; ++i)
        sched_yield();
    CHECK(LOAD(stages) == stopped && !helper_submit_t1(&r, NULL));
    STORE(block_stage, 0);
    helper_resume();
    CHECK(helper_submit_t1(&r, NULL));
    wait_for(&published, before + 1);
    CHECK(LOAD(compiled_on) >= 2 && LOAD(compiled_on) <= 4);
    before = LOAD(published);
    pthread_t producers[4];
    for (unsigned i = 0; i < 4; ++i)
        CHECK(!pthread_create(&producers[i], NULL, producer, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 4; ++i)
        CHECK(!pthread_join(producers[i], NULL));
    wait_for(&published, before + 800);
    CHECK(LOAD(published) == before + 800);
    CHECK(helper_post(HELPER_RECLAIM, 0));
    wait_for(&maintenance_count, 1);
    CHECK(LOAD(maintenance_on) != hal_helper_cpu(2));
    helper_shutdown();
    for (unsigned i = 0; i < 3; ++i)
        CHECK(!hal_cpu_busy(hal_helper_cpu(i)));
}
static void release_wakeup(void)
{
    CHECK(helper_init(3));
    struct t1_compile_request r = request(7001);
    unsigned before = LOAD(published);
    CHECK(helper_submit_t1(&r, NULL));
    wait_for(&published, before + 1);
    STORE(empty_arm, 1);
    /* Hold both slots after an empty pop, and consume the third worker's
     * wake before either holder releases its slot. */
    uint64_t deadline = hal_counter() + 10 * hal_counter_hz();
    while (LOAD(empty_inside) != 12) {
        CHECK(hal_counter() < deadline);
        hal_cpu_request(2, HAL_CPU_WAKE);
        hal_cpu_request(3, HAL_CPU_WAKE);
        sched_yield();
    }
    STORE(missed, 0);
    r = request(7002);
    CHECK(helper_submit_t1(&r, NULL));
    hal_cpu_request(4, HAL_CPU_WAKE);
    wait_for(&missed, 1);
    STORE(empty_release, 1);
    wait_for(&published, before + 2);
    STORE(empty_arm, 0);
    helper_shutdown();
}
static void admission(void)
{
    CHECK(helper_init(0));
    STORE(post_in_submit, 1);
    struct t1_compile_request r = request(8001);
    CHECK(helper_submit_t1(&r, NULL));
    CHECK(!LOAD(post_in_submit));
    unsigned before = LOAD(maintenance_count);
    for (unsigned i = 0; i < 100; ++i)
        CHECK(helper_post(HELPER_RECLAIM, 0));
    while (helper_step()) {
    }
    CHECK(LOAD(maintenance_count) == before + 1);
    helper_shutdown();
    create_fail = 1;
    CHECK(!helper_start(true, false, true));
    CHECK(!helper_active());
    create_fail = 0;
    CHECK(!helper_start(true, true, true));
    CHECK(!helper_start(true, false, false));
    CHECK(!helper_start(false, false, true));
    create_count = 0;
    reported_helpers = 300;
    CHECK(!helper_init(300));
    CHECK(create_count == 256 && !helper_active());
    reported_helpers = 0;
    CHECK(helper_start(true, false, true));
    helper_shutdown();
}
int main(void)
{
    alarm(40);
    CHECK(!hal_linux_configure(true, 1, 3, NULL, 0));
    CHECK(hal_helper_count() == 3 && hal_helper_cpu(0) == 2);
    CHECK(hal_helper_cpu(3) == UINT32_MAX && !hal_helper_rank(3));
    CHECK(!helper_init(4));
    fallback();
    statistics();
    parallel_workers();
    workers();
    release_wakeup();
    admission();
    CHECK(hal_cpu_start(hal_helper_cpu(1), other_work, NULL));
    wait_for(&other_started, 1);
    CHECK(!helper_init(3));
    CHECK(!hal_cpu_busy(hal_helper_cpu(0)) && hal_cpu_busy(hal_helper_cpu(1)));
    STORE(other_release, 1);
    while (hal_cpu_busy(hal_helper_cpu(1)))
        sched_yield();
    rank3only = 1;
    CHECK(helper_init(1));
    unsigned before = LOAD(maintenance_count);
    hal_cpu_poll();
    CHECK(helper_post(HELPER_RECLAIM, 0));
    hal_idle_until(hal_counter() + hal_counter_hz() / 10);
    CHECK(hal_cpu_poll() & HAL_CPU_WAKE);
    for (unsigned i = 0; i < 10000; ++i)
        sched_yield();
    CHECK(LOAD(maintenance_count) == before);
    helper_step();
    CHECK(LOAD(maintenance_count) == before + 1 && LOAD(maintenance_on) == 0);
    STORE(inside, 0);
    STORE(block_stage, 1);
    STORE(release_stage, 0);
    struct t1_compile_request r = request(901);
    before = LOAD(published);
    CHECK(helper_submit_t1(&r, NULL));
    wait_for(&inside, 1);
    r = request(902);
    CHECK(helper_submit_t1(&r, NULL));
    /* Management has no compile duty while the rank-3 worker owns it. */
    CHECK(!helper_step());
    STORE(release_stage, 1);
    wait_for(&published, before + 2);
    helper_shutdown();
    puts(
        "helper queue: PASS (fallback, statistics, immutable requests, coalescing, wrap, ranks, quiescence, "
        "shutdown)");
    return 0;
}
