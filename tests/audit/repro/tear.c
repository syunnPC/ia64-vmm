/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Rev.2.3 Vol.2 §4.5: concurrent observations of aligned references
 * must never mix two values. This is the audit's tear.c, made bounded and
 * self-checking; cache mutation remains on the single executor. */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "ia64/memory.h"
#include "engine/cache/cache.h"
#include "core/log_internal.h"
#define ENTRY UINT64_C(0x100000)
#define COUNT 200000u
static _Alignas(4096) uint8_t ram[4096];
struct pair {
    uint64_t low, high;
};
struct race {
    unsigned size;
    bool observe;
    unsigned stop, torn, ready;
};
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "tear: line %u: %s\n", __LINE__, #c);                                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint64_t host_read(unsigned size)
{
    switch (size) {
    case 1:
        return __atomic_load_n((uint8_t *)ram, __ATOMIC_RELAXED);
    case 2:
        return __atomic_load_n((uint16_t *)ram, __ATOMIC_RELAXED);
    case 4:
        return __atomic_load_n((uint32_t *)ram, __ATOMIC_RELAXED);
    default:
        return __atomic_load_n((uint64_t *)ram, __ATOMIC_RELAXED);
    }
}
static void host_write(unsigned size, uint64_t value)
{
    switch (size) {
    case 1:
        __atomic_store_n((uint8_t *)ram, value, __ATOMIC_RELAXED);
        break;
    case 2:
        __atomic_store_n((uint16_t *)ram, value, __ATOMIC_RELAXED);
        break;
    case 4:
        __atomic_store_n((uint32_t *)ram, value, __ATOMIC_RELAXED);
        break;
    default:
        __atomic_store_n((uint64_t *)ram, value, __ATOMIC_RELAXED);
        break;
    }
}
static struct pair pair_cas(struct pair old, struct pair value)
{
    __asm__ volatile("lock cmpxchg16b %0"
                     : "+m"(*(struct pair *)ram), "+a"(old.low), "+d"(old.high)
                     : "b"(value.low), "c"(value.high)
                     : "cc", "memory");
    return old;
}
static void host_write16(uint64_t value)
{
    struct pair old = pair_cas((struct pair){0}, (struct pair){0});
    for (;;) {
        struct pair seen = pair_cas(old, (struct pair){value, value});
        if (seen.low == old.low && seen.high == old.high)
            return;
        old = seen;
    }
}
static void *race_thread(void *arg)
{
    struct race *r = arg;
    uint64_t mask = r->size == 16 ? UINT64_MAX : ia64_low_mask(r->size * 8);
    __atomic_store_n(&r->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&r->stop, __ATOMIC_ACQUIRE)) {
        if (r->observe) {
            bool torn;
            if (r->size == 16) {
                struct pair old = pair_cas((struct pair){0}, (struct pair){0});
                torn = old.low != old.high;
            } else {
                uint64_t value = host_read(r->size);
                torn = value != 0 && value != mask;
            }
            if (torn)
                __atomic_store_n(&r->torn, 1, __ATOMIC_RELAXED);
        } else if (r->size == 16) {
            host_write16(0);
            host_write16(mask);
        } else {
            host_write(r->size, 0);
            host_write(r->size, mask);
        }
    }
    return NULL;
}
static void stress(struct vcpu *v, unsigned size, bool store, bool fetch)
{
    host_write16(0);
    struct race r = {.size = size, .observe = store};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, race_thread, &r) == 0);
    while (!__atomic_load_n(&r.ready, __ATOMIC_ACQUIRE)) {
    }
    Ia64Instruction i = {.valid = true,
                         .unit = IA64_UNIT_M,
                         .operands.memory = {.destination = 8, .source = 9, .base = 10}};
    i.opcode = size == 16 ? (store ? IA64_OP_ST16 : IA64_OP_LD16)
                          : (store ? IA64_OP_ST1 : IA64_OP_LD1) + (unsigned)__builtin_ctz(size);
    i.mem_acquire = !store;
    i.mem_release = store;
    v->s.gr[10] = ENTRY;
    bool torn = false;
    for (unsigned n = 0; n < COUNT && !torn; ++n) {
        uint64_t mask = size == 16 ? UINT64_MAX : ia64_low_mask(size * 8);
        if (fetch) {
            const struct ia64_bundle *bundle;
            code_notify_vmm_write(ENTRY, 16);
            CHECK(code_get_bundle(v, ENTRY, &bundle).kind == IA64_STEP_COMPLETE);
            torn = bundle->low != bundle->high;
        } else {
            v->s.gr[9] = n & 1 ? mask : 0;
            v->s.ar[AR_CSD] = v->s.gr[9];
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
            if (!store) {
                uint64_t value = v->s.gr[8];
                torn = (value != 0 && value != mask) || (size == 16 && value != v->s.ar[AR_CSD]);
            }
        }
    }
    __atomic_store_n(&r.stop, 1, __ATOMIC_RELEASE);
    CHECK(pthread_join(thread, NULL) == 0);
    if (torn || r.torn)
        fprintf(stderr, "torn %s size=%u store=%u\n", fetch ? "bundle" : "datum", size, store);
    CHECK(!torn && !r.torn);
}
int main(void)
{
    log_set_level(LOG_ERROR);
    CHECK(gm_add_region(
        &(struct gm_region){.base = ENTRY, .size = sizeof(ram), .kind = GM_RAM, .hva = ram}));
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    vcpu_reset(v, NULL);
    for (unsigned size = 1; size <= 16; size *= 2)
        for (unsigned store = 0; store < 2; ++store)
            stress(v, size, store, false);
    /* This intentionally rewrites the page continuously.
     * Test atomic fetch in bring-up mode; a coherence publication check
     * cannot promise progress while its source never stabilizes. */
    CHECK(code_validity_configure(CODE_VALIDITY_NONE, CODE_SNAPSHOT_DEFAULT_BYTES));
    stress(v, 16, false, true);
    vcpu_destroy(v);
    puts("test-audit: concurrent ld/st1/2/4/8/16 and bundle snapshots passed");
    return 0;
}
