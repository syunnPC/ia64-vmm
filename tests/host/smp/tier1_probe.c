/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include "engine/tier1/region.h"
#include "tests/tier1/harness.h"

#define PROBE_PA UINT64_C(0x10000)
#define PROBE_BITS 36u
static struct vcpu reader;
static const struct t1_compile_options options = {.flags = T1_COMPILE_STORE_LOG};
static struct t1_region *held, *result;
static atomic_uint stopped, resume, visits, stress_done, hits;
static unsigned held_event;
static bool expect_locked;

static void wait_flag(atomic_uint *flag)
{
    uint64_t end = hal_counter() + 10 * hal_counter_hz();
    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        T1H_CHECK(hal_counter() < end);
        sched_yield();
    }
}

static struct t1_region *make_region(gpa_t pa)
{
    struct t1_region *r = t1_region_new();
    if (!r)
        return NULL;
    r->c = (struct t1_contract){.ip = reader.s.ip,
                                .slot = reader.s.slot,
                                .cpu_model = reader.cfg.cpu_model,
                                .psr = reader.s.psr,
                                .psr_mask = UINT64_MAX,
                                .cfm = reader.s.cfm,
                                .cfm_mask = UINT64_MAX,
                                .abi = T1_ABI_VERSION,
                                .icount = reader.cfg.itc_icount,
                                .flags = options.flags,
                                .hostwin_bits = PROBE_BITS,
                                .entry_pa = pa};
    return r;
}

static struct t1_region *publish_region(gpa_t pa)
{
    struct t1_region *r = make_region(pa);
    T1H_CHECK(r && t1_region_publish(r));
    return r;
}

static struct t1_region *probe(void)
{
    return t1_region_probe(&reader, &options, PROBE_BITS, PROBE_PA);
}

static void hook(const struct t1_region *r, unsigned event)
{
    if (event == T1_REGION_TEST_PROBE)
        T1H_CHECK(code_lock_held() == expect_locked);
    if (r != held)
        return;
    if (event == T1_REGION_TEST_PROBE)
        atomic_fetch_add_explicit(&visits, 1, memory_order_relaxed);
    if (event != held_event)
        return;
    atomic_store_explicit(&stopped, 1, memory_order_release);
    wait_flag(&resume);
}

static void arm(struct t1_region *r, unsigned event)
{
    held = r;
    held_event = event;
    atomic_store(&stopped, 0);
    atomic_store(&resume, 0);
    atomic_store(&visits, 0);
}

static void *probe_thread(void *unused)
{
    (void)unused;
    code_quiescent(&reader);
    result = probe();
    return NULL;
}

static void *retire_thread(void *r)
{
    t1_region_retire(r);
    return NULL;
}

static void *publish_thread(void *r)
{
    T1H_CHECK(t1_region_publish(r));
    return NULL;
}

static void reclaim_all(void)
{
    code_quiescent(&reader);
    t1_flush();
    code_quiescent(&reader);
    t1_reclaim();
}

static void contracts(void)
{
    for (unsigned i = 0; i < 8; ++i) {
        struct t1_region *r = make_region(PROBE_PA);
        T1H_CHECK(r);
        switch (i) {
        case 0:
            ++r->c.cpu_model;
            break;
        case 1:
            r->c.psr ^= 1;
            break;
        case 2:
            r->c.cfm ^= 1;
            break;
        case 3:
            ++r->c.abi;
            break;
        case 4:
            r->c.icount = !r->c.icount;
            break;
        case 5:
            r->c.flags ^= T1_COMPILE_STORE_LOG;
            break;
        case 6:
            ++r->c.hostwin_bits;
            break;
        default:
            r->c.entry_pa += 16;
            break;
        }
        T1H_CHECK(t1_region_publish(r));
        T1H_CHECK(!probe());
    }
    struct t1_region *matching = publish_region(PROBE_PA);
    T1H_CHECK(probe() == matching);
    /* Parked executors have no epoch pin and use the locked path. */
    reader.parked = true;
    expect_locked = true;
    T1H_CHECK(probe() == matching);
    reader.parked = false;
    expect_locked = false;
    reclaim_all();
}

/* An old reader follows two unlinked intermediates. Neither record nor
 * its saved successor can be reused until that reader publishes again. */
static void retired_chain(void)
{
    struct t1_region *tail = publish_region(PROBE_PA);
    struct t1_region *middle = publish_region(PROBE_PA + 16);
    struct t1_region *head = publish_region(PROBE_PA);
    uint32_t head_id = head->id, middle_id = middle->id;
    uint64_t head_link = head->link;
    arm(head, T1_REGION_TEST_PROBE);
    pthread_t thread;
    T1H_CHECK(!pthread_create(&thread, NULL, probe_thread, NULL));
    wait_flag(&stopped);
    t1_region_retire(head);
    t1_region_retire(middle);
    t1_reclaim();
    T1H_CHECK(t1_region_get(head_id) == head && t1_region_get(middle_id) == middle);
    /* The locked iterator does not expose a retired record's saved links. */
    T1H_CHECK(!t1_region_lookup(&reader, &options, PROBE_BITS, head));
    publish_region(PROBE_PA + 48);
    atomic_store_explicit(&resume, 1, memory_order_release);
    T1H_CHECK(!pthread_join(thread, NULL));
    T1H_CHECK(result == tail);
    code_quiescent(&reader);
    t1_reclaim();
    T1H_CHECK(!t1_region_get(head_id) && !t1_region_get(middle_id));
    struct t1_region *replacement = publish_region(PROBE_PA + 64);
    T1H_CHECK(replacement->id == head_id || replacement->id == middle_id);
    T1H_CHECK(replacement->link != head_link && !t1_region_from_link(head_link));
    T1H_CHECK(probe() == tail);
    reclaim_all();
}

/* A reader sampling the retirement epoch must see the unlink, even when
 * the retiring thread still holds code_lock. */
static void retired_before_sample(void)
{
    struct t1_region *tail = publish_region(PROBE_PA);
    struct t1_region *head = publish_region(PROBE_PA + 16);
    uint32_t id = head->id;
    arm(head, T1_REGION_TEST_RETIRED);
    pthread_t thread;
    T1H_CHECK(!pthread_create(&thread, NULL, retire_thread, head));
    wait_flag(&stopped);
    code_quiescent(&reader);
    T1H_CHECK(probe() == tail);
    T1H_CHECK(!atomic_load_explicit(&visits, memory_order_relaxed));
    atomic_store_explicit(&resume, 1, memory_order_release);
    T1H_CHECK(!pthread_join(thread, NULL));
    t1_reclaim();
    T1H_CHECK(!t1_region_get(id));
    T1H_CHECK(probe() == tail);
    reclaim_all();
}

/* Publication keeps a complete new contract private until its bucket
 * store; an executor can probe the previous head while the writer holds
 * code_lock. */
static void publication(void)
{
    struct t1_region *tail = publish_region(PROBE_PA + 16);
    struct t1_region *head = make_region(PROBE_PA);
    T1H_CHECK(head);
    arm(head, T1_REGION_TEST_PUBLISH);
    pthread_t thread;
    T1H_CHECK(!pthread_create(&thread, NULL, publish_thread, head));
    wait_flag(&stopped);
    code_quiescent(&reader);
    T1H_CHECK(!probe());
    atomic_store_explicit(&resume, 1, memory_order_release);
    T1H_CHECK(!pthread_join(thread, NULL));
    T1H_CHECK(probe() == head && head->hash_next == tail);
    reclaim_all();
}

static void *stress_reader(void *unused)
{
    (void)unused;
    while (!atomic_load_explicit(&stress_done, memory_order_acquire)) {
        code_quiescent(&reader);
        struct t1_region *r = probe();
        if (r) {
            T1H_CHECK(t1_region_matches(r, &reader, options.flags, PROBE_BITS));
            T1H_CHECK(r->c.entry_pa == PROBE_PA && r->link);
            atomic_fetch_add_explicit(&hits, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

static void concurrent_reuse(void)
{
    held = NULL;
    struct t1_region *previous = publish_region(PROBE_PA);
    pthread_t thread;
    T1H_CHECK(!pthread_create(&thread, NULL, stress_reader, NULL));
    wait_flag(&hits);
    for (unsigned i = 0; i < 4096; ++i) {
        struct t1_region *r;
        while (!(r = make_region(PROBE_PA + (i & 1u) * 16))) {
            t1_reclaim();
            sched_yield();
        }
        T1H_CHECK(t1_region_publish(r));
        t1_region_retire(previous);
        previous = r;
        t1_reclaim();
    }
    atomic_store_explicit(&stress_done, 1, memory_order_release);
    T1H_CHECK(!pthread_join(thread, NULL));
    reclaim_all();
}

void smp_tier1_probe_test(void);
void smp_tier1_probe_test(void)
{
    T1H_CHECK(t1_init(&(struct t1_config){.max_regions = 32}));
    reader.s.ip = 0x1000;
    reader.s.psr = 0x2000;
    reader.s.cfm = 3;
    reader.cfg.itc_icount = true;
    code_register_vcpu(&reader);
    t1_test_region_hook = hook;
    contracts();
    retired_chain();
    retired_before_sample();
    publication();
    concurrent_reuse();
    code_unregister_vcpu(&reader);
    /* Unregistered fixtures retain the locked probe path. */
    expect_locked = true;
    struct t1_region *r = publish_region(PROBE_PA);
    T1H_CHECK(probe() == r);
    t1_test_region_hook = NULL;
    t1_flush();
    t1_shutdown();
}
