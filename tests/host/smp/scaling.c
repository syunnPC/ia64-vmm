/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "engine/tier0/tier0.h"
#include "tests/bench/job.h"
#include "ia64vmm/hal.h"

/* Port of jobdump.c's compiled-C setup. start.S derives gp from its entry
 * IP, so each executor gets an independent, relocated code/data/stack copy.
 * All copies use the same shared Tier0 arena, validator and code_lock. */
#define RAM_SIZE 1048576u
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "scaling FAIL %u: %s\n", __LINE__, #c);                                \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
struct copy {
    struct vcpu *v;
    uint8_t *ram;
    uint64_t base, ticks, retired;
    unsigned host_cpu;
};
static struct copy copies[4];
static pthread_barrier_t barrier;
static unsigned kernel;
static uint64_t warm_iters, measured_iters, expected;
static uint64_t now(void)
{
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
static bool stop(struct vcpu *v, uint64_t imm, uint64_t ip)
{
    (void)ip;
    if (imm != 0x1ffffe)
        return false;
    vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    return true;
}
static void setup(struct copy *c, uint64_t iterations)
{
    struct ia64_entry_regs regs = {.ip = c->base,
                                   .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
                                   .bsp = c->base + 0xf8000,
                                   .bspstore = c->base + 0xf8000};
    regs.gr[2] = kernel;
    regs.gr[3] = iterations;
    regs.gr[12] = c->base + 0xf0000;
    vcpu_reset(c->v, &regs);
    c->v->s.cr[CR_IVA] = c->base + 0x8000;
}
static void execute(struct copy *c, bool measured)
{
    struct vcpu_exit out;
    do {
        out = tier0_run(c->v, 100000);
    } while (out.reason == VCPU_EXIT_BUDGET);
    CHECK(out.reason == VCPU_EXIT_DEBUG && c->v->s.gr[9] == 0);
    if (measured)
        CHECK(c->v->s.gr[8] == expected);
}
static void *worker(void *opaque)
{
    struct copy *c = opaque;
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(c->host_cpu, &affinity);
    CHECK(!pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity));
    setup(c, warm_iters);
    execute(c, false);
    setup(c, measured_iters);
    (void)pthread_barrier_wait(&barrier); /* every code copy is warm */
    (void)pthread_barrier_wait(&barrier);
    uint64_t start = hal_counter();
    execute(c, true);
    c->ticks = hal_counter() - start;
    c->retired = c->v->retired;
    code_quiescent(c->v);
    __atomic_store_n(&c->v->parked, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static uint8_t *read_file(const char *name, size_t *length)
{
    FILE *f = fopen(name, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    long bytes = ftell(f);
    CHECK(bytes > 0);
    rewind(f);
    uint8_t *data = malloc(bytes);
    CHECK(data && fread(data, 1, bytes, f) == (size_t)bytes);
    CHECK(!fclose(f));
    *length = bytes;
    return data;
}
int main(int argc, char **argv)
{
    CHECK(argc >= 6);
    unsigned count = (unsigned)strtoul(argv[1], NULL, 10);
    kernel = (unsigned)strtoul(argv[2], NULL, 10);
    CHECK((count == 1 || count == 4) && kernel < 6 && argc == 5 + (int)count);
    size_t job_length, image_length;
    uint8_t *job_bytes = read_file(argv[3], &job_length),
            *image = read_file(argv[4], &image_length);
    const struct bj_job *job = (const void *)job_bytes;
    CHECK(job_length >= sizeof(*job) && job_length == job->size);
    CHECK(job->nvariant >= 2 && image_length <= RAM_SIZE);
    warm_iters = job->variant[0].gr_value;
    measured_iters = job->variant[1].gr_value;
    bool found = false;
    for (unsigned i = 0; i < job->variant[1].ncheck; ++i)
        if (job->variant[1].check[i].where == BJ_GR && job->variant[1].check[i].index == 8) {
            expected = job->variant[1].check[i].value;
            found = true;
        }
    CHECK(found);
    engine_set_break_hook(stop);
    CHECK(tier0_configure(TIER0_CODE_DEFAULT_BYTES));
    /* One guest has one fixed RAM map at either CPU count. Separate regions
     * per copy would add bitmap-span searches only to the four-CPU row. */
    uint8_t *ram = aligned_alloc(4096, 4 * RAM_SIZE);
    CHECK(ram);
    memset(ram, 0, 4 * RAM_SIZE);
    struct gm_region region = {.base = 0x100000, .size = 4 * RAM_SIZE, .kind = GM_RAM, .hva = ram};
    CHECK(gm_add_region(&region));
    pthread_t threads[4];
    CHECK(!pthread_barrier_init(&barrier, NULL, count + 1));
    for (unsigned i = 0; i < count; ++i) {
        struct copy *c = &copies[i];
        c->base = region.base + i * RAM_SIZE;
        c->host_cpu = (unsigned)strtoul(argv[5 + i], NULL, 10);
        c->ram = ram + i * RAM_SIZE;
        memcpy(c->ram, image, image_length);
        struct vcpu_config cfg = {.engine = ENGINE_TIER0, .index = i, .itc_icount = true};
        CHECK((c->v = vcpu_create(&cfg)) && engine_vcpu_attach(c->v));
    }
    /* Publish all immutable backed spans before any source reader runs. */
    CHECK(code_validity_active());
    for (unsigned i = 0; i < count; ++i)
        CHECK(!pthread_create(&threads[i], NULL, worker, &copies[i]));
    (void)pthread_barrier_wait(&barrier);
    struct code_lock_stats before, after;
    code_lock_get_stats(&before);
    uint64_t start = now();
    (void)pthread_barrier_wait(&barrier);
    uint64_t ticks = 0, retired = 0;
    for (unsigned i = 0; i < count; ++i) {
        CHECK(!pthread_join(threads[i], NULL));
        ticks += copies[i].ticks;
        retired += copies[i].retired;
    }
    uint64_t elapsed = now() - start;
    code_lock_get_stats(&after);
    printf("{\"cpus\":%u,\"kernel\":%u,\"ns\":%llu,\"retired\":%llu,\"executor_ticks\":%llu,"
           "\"wait_ticks\":%llu,\"acquisitions\":%llu,\"hz\":%llu}\n",
           count, kernel, (unsigned long long)elapsed, (unsigned long long)retired,
           (unsigned long long)ticks, (unsigned long long)(after.wait_ticks - before.wait_ticks),
           (unsigned long long)(after.acquisitions - before.acquisitions),
           (unsigned long long)hal_counter_hz());
    for (unsigned i = 0; i < count; ++i) {
        engine_vcpu_detach(copies[i].v);
        vcpu_destroy(copies[i].v);
    }
    code_cache_flush();
    tier0_destroy();
    free(ram);
    free(image);
    free(job_bytes);
    CHECK(!pthread_barrier_destroy(&barrier));
    return 0;
}
