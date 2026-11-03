/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Immutable requests, bounded management steps,
 * and no helper access to live vCPU state. Compiler slots bound retained
 * workspaces as well as active compiles. A worker returns its slot only
 * after flushing its batch and trimming its workspace. */
#include "core/helper.h"
#include "engine/tier1/tier1.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"

/* CPU-model-only test binaries need no Tier1 or code arena. */
#pragma weak t1_compile_ctx_create
#pragma weak t1_compile_ctx_destroy
#pragma weak t1_compile_begin
#pragma weak t1_compile_step
#pragma weak t1_compile_flush
#pragma weak t1_compile_cancel
#pragma weak t1_compile_trim
#pragma weak t1_compile_dropped
extern void t1_reclaim(void) __attribute__((weak));
extern void tier0_reclaim_batch(unsigned limit) __attribute__((weak));

#define CAPACITY 256u
#define MAX_HELPERS 256u
struct cell {
    uint64_t seq, submitted_at;
    struct t1_compile_request rq;
};
static struct cell queue[CAPACITY];
static uint64_t enqueue, dequeue;
/* Submission statistics (struct helper_stats), from any producer. */
static uint64_t submitted, refused, queued_sum;
static uint64_t keys[CAPACITY];
static unsigned key_lock, enabled, paused, stopping, count, compiler;
static unsigned admissions;
static unsigned maintenance, pause_generation, periodic;
struct worker {
    struct t1_compile_ctx *ctx;
    struct t1_compile_request rq;
    struct helper_stats stats;
    unsigned cpu, rank, idle;
    bool compiling, owns;
    uint64_t pending_keys[8];
    unsigned nkeys;
};
static struct worker workers[MAX_HELPERS], management;
/* A worker's statistics have one writer; live readers load them relaxed. */
#define STAT_ADD(field, n)                                                                         \
    __atomic_store_n(&(field), __atomic_load_n(&(field), __ATOMIC_RELAXED) + (n), __ATOMIC_RELAXED)
#ifdef IA64VMM_HOSTED
extern void helper_test_event(unsigned event) __attribute__((weak));
#define TEST_EVENT(e)                                                                              \
    do {                                                                                           \
        if (helper_test_event)                                                                     \
            helper_test_event(e);                                                                  \
    } while (0)
#else
#define TEST_EVENT(e) ((void)0)
#endif
static bool queued(void)
{
    uint64_t at = __atomic_load_n(&dequeue, __ATOMIC_RELAXED);
    return __atomic_load_n(&queue[at % CAPACITY].seq, __ATOMIC_ACQUIRE) == at + 1;
}
static void wake_workers(void);
static bool take_compiler(void)
{
    unsigned used = __atomic_load_n(&compiler, __ATOMIC_RELAXED);
    while (used < T1_COMPILE_SLOTS) {
        if (__atomic_compare_exchange_n(&compiler, &used, used + 1, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return true;
    }
    return false;
}
static void release_compiler(void)
{
    __atomic_fetch_sub(&compiler, 1, __ATOMIC_SEQ_CST);
    /* Order slot availability before the queue recheck: a waiting worker
     * may already have consumed the submit wake. */
    if (queued())
        wake_workers();
}

static void keys_take(void)
{
    while (__atomic_exchange_n(&key_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void keys_drop(void)
{
    __atomic_store_n(&key_lock, 0, __ATOMIC_RELEASE);
}
static void key_done(uint64_t key)
{
    keys_take();
    for (unsigned i = 0; i < CAPACITY; ++i)
        if (keys[i] == key) {
            keys[i] = 0;
            break;
        }
    keys_drop();
}
/* A worker that runs sees the queue and the maintenance word at its next
 * step: only an idle one is interrupted (hal_cpu_wake). */
static void wake_workers(void)
{
    if (!count || !periodic)
        hal_cpu_request(0, HAL_CPU_WAKE);
    for (unsigned i = 0; i < count; ++i)
        hal_cpu_wake(workers[i].cpu);
}

bool helper_active(void)
{
    return __atomic_load_n(&enabled, __ATOMIC_ACQUIRE);
}

static bool declined(void)
{
    __atomic_fetch_add(&refused, 1, __ATOMIC_RELAXED);
    return false;
}

bool helper_submit_t1(const struct t1_compile_request *rq, void *arg)
{
    (void)arg;
    if (!rq || !rq->key)
        return false;
    if (!helper_active() || __atomic_load_n(&paused, __ATOMIC_ACQUIRE))
        return declined();
    /* A producer never waits on another producer or a completing helper. */
    if (__atomic_exchange_n(&key_lock, 1, __ATOMIC_ACQUIRE))
        return declined();
    unsigned free = CAPACITY;
    for (unsigned i = 0; i < CAPACITY; ++i) {
        if (keys[i] == rq->key) {
            keys_drop();
            return declined();
        }
        if (!keys[i])
            free = i;
    }
    if (free == CAPACITY || !helper_active() || __atomic_load_n(&paused, __ATOMIC_ACQUIRE)) {
        keys_drop();
        return declined();
    }
    uint64_t at = __atomic_load_n(&enqueue, __ATOMIC_RELAXED);
    struct cell *c = &queue[at % CAPACITY];
    if (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) != at) {
        keys_drop();
        return declined();
    }
    TEST_EVENT(2);
    keys[free] = rq->key;
    c->rq = *rq;
    c->submitted_at = IA64VMM_TIER1_STATS ? hal_counter() : 0;
    __atomic_fetch_add(&submitted, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&queued_sum, at - __atomic_load_n(&dequeue, __ATOMIC_RELAXED),
                       __ATOMIC_RELAXED);
    __atomic_store_n(&enqueue, at + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&c->seq, at + 1, __ATOMIC_RELEASE);
    /* Keep the publication and wake inside the admission bracket: shutdown
     * cannot return while a submitter still touches a worker's mailbox. */
    if (!count)
        hal_cpu_request(0, HAL_CPU_WAKE);
    else
        hal_cpu_wake(workers[at % count].cpu);
    keys_drop();
    return true;
}

static bool pop(struct worker *w, struct t1_compile_request *rq)
{
    uint64_t at = __atomic_load_n(&dequeue, __ATOMIC_RELAXED);
    for (;;) {
        struct cell *c = &queue[at % CAPACITY];
        if (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) != at + 1)
            return false;
        if (!__atomic_compare_exchange_n(&dequeue, &at, at + 1, false, __ATOMIC_RELAXED,
                                         __ATOMIC_RELAXED))
            continue;
        *rq = c->rq;
        if (IA64VMM_TIER1_STATS)
            STAT_ADD(w->stats.wait_ticks, hal_counter() - c->submitted_at);
        __atomic_store_n(&c->seq, at + CAPACITY, __ATOMIC_RELEASE);
        return true;
    }
}

bool helper_post(enum helper_kind kind, uint64_t arg)
{
    (void)arg;
    /* Only reclamation is scheduled on helpers. */
    if (kind != HELPER_RECLAIM || !helper_active() || __atomic_load_n(&paused, __ATOMIC_ACQUIRE))
        return false;
    if (__atomic_load_n(&maintenance, __ATOMIC_RELAXED))
        return true;
    __atomic_add_fetch(&admissions, 1, __ATOMIC_SEQ_CST);
    bool accepted = helper_active() && !__atomic_load_n(&paused, __ATOMIC_SEQ_CST);
    if (accepted) {
        unsigned previous = __atomic_fetch_or(&maintenance, 1u << kind, __ATOMIC_RELEASE);
        if (!previous)
            wake_workers();
    }
    __atomic_sub_fetch(&admissions, 1, __ATOMIC_SEQ_CST);
    return accepted;
}

static void cancel(struct worker *w)
{
    t1_compile_cancel(w->ctx);
    for (unsigned i = 0; i < w->nkeys; ++i)
        key_done(w->pending_keys[i]);
    w->nkeys = 0;
    if (w->compiling) {
        key_done(w->rq.key);
        w->compiling = false;
        STAT_ADD(w->stats.cancelled, 1);
    }
    if (w->owns) {
        t1_compile_trim(w->ctx);
        w->owns = false;
        release_compiler();
        wake_workers();
    }
}
static void flush(struct worker *w)
{
    t1_compile_flush(w->ctx);
    for (unsigned i = 0; i < w->nkeys; ++i)
        key_done(w->pending_keys[i]);
    w->nkeys = 0;
}
static bool take_maintenance(void)
{
    unsigned expected = 1u << HELPER_RECLAIM;
    return __atomic_compare_exchange_n(&maintenance, &expected, expected | 1u, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static bool step(struct worker *w, bool compile)
{
    if (compile && !w->compiling) {
        if (w->owns || take_compiler()) {
            w->owns = true;
            if (pop(w, &w->rq)) {
                if (t1_compile_begin(w->ctx, &w->rq, true))
                    w->compiling = true;
                else {
                    /* No compile: the request completes unpublished. */
                    t1_compile_dropped(&w->rq);
                    key_done(w->rq.key);
                }
            } else
                TEST_EVENT(0);
            if (!w->compiling) {
                flush(w);
                t1_compile_trim(w->ctx);
                w->owns = false;
                release_compiler();
            }
        } else
            TEST_EVENT(1);
    }
    bool did_work = false, compiled = w->compiling;
    uint64_t start = hal_counter();
    if (w->compiling) {
        did_work = true;
        if (!t1_compile_step(w->ctx, NULL)) {
            w->pending_keys[w->nkeys++] = w->rq.key;
            w->compiling = false;
            STAT_ADD(w->stats.completed, 1);
            if (w->nkeys == IA64VMM_ARRAY_SIZE(w->pending_keys))
                flush(w);
            if (!queued()) {
                flush(w);
                t1_compile_trim(w->ctx);
                w->owns = false;
                release_compiler();
                wake_workers();
            }
        }
    } else if (w->rank != 3 && take_maintenance()) {
        did_work = true;
        if (tier0_reclaim_batch)
            tier0_reclaim_batch(64);
        if (t1_reclaim)
            t1_reclaim();
        __atomic_store_n(&maintenance, 0, __ATOMIC_RELEASE);
    }
    if (did_work) {
        STAT_ADD(w->stats.steps, 1);
        uint64_t ticks = hal_counter() - start;
        if (compiled)
            STAT_ADD(w->stats.compile_ticks, ticks);
        else
            STAT_ADD(w->stats.maintenance_ticks, ticks);
        if (ticks > w->stats.max_step_ticks)
            __atomic_store_n(&w->stats.max_step_ticks, ticks, __ATOMIC_RELAXED);
    }
    return did_work;
}
static void run(void *arg)
{
    struct worker *w = arg;
    for (;;) {
        unsigned requests = hal_cpu_poll();
        if ((requests & HAL_CPU_STOP) || __atomic_load_n(&stopping, __ATOMIC_ACQUIRE))
            break;
        unsigned pause = __atomic_load_n(&paused, __ATOMIC_SEQ_CST);
        if (pause) {
            cancel(w);
            __atomic_store_n(&w->idle, pause, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&paused, __ATOMIC_SEQ_CST) == pause)
                hal_idle_until(UINT64_MAX);
            continue;
        }
        __atomic_store_n(&w->idle, 0, __ATOMIC_SEQ_CST);
        /* Pair the idle publication with quiesce's pause and idle reads. */
        if (__atomic_load_n(&paused, __ATOMIC_SEQ_CST))
            continue;
        if (!step(w, true))
            hal_idle_until(UINT64_MAX);
    }
    cancel(w);
    __atomic_store_n(&w->idle, 1, __ATOMIC_RELEASE);
}

bool helper_step(void)
{
    if (!helper_active() || (count && periodic) || __atomic_load_n(&paused, __ATOMIC_ACQUIRE))
        return false;
    step(&management, !count);
    return management.compiling ||
           (!count && __atomic_load_n(&enqueue, __ATOMIC_ACQUIRE) !=
                          __atomic_load_n(&dequeue, __ATOMIC_ACQUIRE)) ||
           __atomic_load_n(&maintenance, __ATOMIC_ACQUIRE);
}
void helper_quiesce(void)
{
    if (!helper_active())
        return;
    unsigned pause = ++pause_generation;
    __atomic_store_n(&paused, pause, __ATOMIC_SEQ_CST);
    keys_take();
    keys_drop();
    while (__atomic_load_n(&admissions, __ATOMIC_SEQ_CST))
        __asm__ volatile("pause");
    wake_workers();
    if (!count)
        cancel(&management);
    for (unsigned i = 0; i < count; ++i)
        while (hal_cpu_busy(workers[i].cpu) &&
               __atomic_load_n(&workers[i].idle, __ATOMIC_SEQ_CST) != pause)
            __asm__ volatile("pause");
}
void helper_resume(void)
{
    if (!helper_active())
        return;
    __atomic_store_n(&paused, 0, __ATOMIC_RELEASE);
    wake_workers();
}
void helper_shutdown(void)
{
    if (!helper_active())
        return;
    helper_quiesce();
    keys_take();
    __atomic_store_n(&enabled, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
    keys_drop();
    wake_workers();
    for (unsigned i = 0; i < count; ++i) {
        while (hal_cpu_busy(workers[i].cpu))
            __asm__ volatile("pause");
        t1_compile_ctx_destroy(workers[i].ctx);
        workers[i].ctx = NULL;
    }
    if (management.ctx)
        t1_compile_ctx_destroy(management.ctx);
    management.ctx = NULL;
    struct helper_stats total;
    helper_get_stats(&total);
    management.stats = total;
    count = 0;
}
bool helper_init(unsigned helpers)
{
    if (helper_active() || helpers > hal_helper_count() || !t1_compile_ctx_create ||
        !t1_compile_ctx_destroy || !t1_compile_begin || !t1_compile_step || !t1_compile_cancel ||
        !t1_compile_flush || !t1_compile_trim || !t1_compile_dropped)
        return false;
    if (helpers > MAX_HELPERS)
        helpers = MAX_HELPERS;
    memset(workers, 0, sizeof(workers));
    memset(&management, 0, sizeof(management));
    memset(keys, 0, sizeof(keys));
    enqueue = dequeue = 0;
    submitted = refused = queued_sum = 0;
    admissions = key_lock = maintenance = compiler = stopping = count = periodic = 0;
    paused = ++pause_generation;
    for (unsigned i = 0; i < CAPACITY; ++i)
        queue[i].seq = i;
    for (unsigned i = 0; i < (helpers ? helpers : 1); ++i) {
        struct worker *w = helpers ? &workers[i] : &management;
        w->cpu = helpers ? hal_helper_cpu(i) : 0;
        w->rank = helpers ? hal_helper_rank(i) : 1;
        if (helpers && w->rank != 3)
            ++periodic;
        w->ctx = t1_compile_ctx_create();
        if (!w->ctx) {
            for (unsigned j = 0; j < i; ++j)
                t1_compile_ctx_destroy(workers[j].ctx);
            return false;
        }
    }
    __atomic_store_n(&enabled, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < helpers; ++i) {
        if (!hal_cpu_start(workers[i].cpu, run, &workers[i])) {
            /* All created contexts, including unstarted workers, are freed. */
            for (unsigned j = i; j < helpers; ++j)
                t1_compile_ctx_destroy(workers[j].ctx);
            helper_shutdown();
            return false;
        }
        ++count;
    }
    helper_resume();
    return true;
}
void helper_get_stats(struct helper_stats *out)
{
    *out = management.stats;
    for (unsigned i = 0; i < count; ++i) {
        const struct helper_stats *s = &workers[i].stats;
#define SUM(field) out->field += __atomic_load_n(&s->field, __ATOMIC_RELAXED)
        SUM(steps);
        SUM(completed);
        SUM(cancelled);
        SUM(compile_ticks);
        SUM(maintenance_ticks);
        SUM(wait_ticks);
#undef SUM
        uint64_t longest = __atomic_load_n(&s->max_step_ticks, __ATOMIC_RELAXED);
        if (longest > out->max_step_ticks)
            out->max_step_ticks = longest;
    }
    out->submitted = __atomic_load_n(&submitted, __ATOMIC_RELAXED);
    out->refused = __atomic_load_n(&refused, __ATOMIC_RELAXED);
    out->queued = __atomic_load_n(&queued_sum, __ATOMIC_RELAXED);
}

bool helper_start(bool requested, bool icount, bool tier1)
{
    return requested && !icount && tier1 && helper_init(hal_helper_count());
}
