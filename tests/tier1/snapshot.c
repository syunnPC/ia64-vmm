/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Snapshots of the Tier1 test machine; see snapshot.h. */
#include <stdlib.h>
#include "ia64/mmu.h"
#include <string.h>
#include "tests/tier1/snapshot.h"
#include "tests/tier1/storelog.h"

void snapshot_init(struct snapshot *s)
{
    memset(s, 0, sizeof(*s));
    s->ram = calloc(1, T1H_RAM_SIZE);
    T1H_CHECK(s->ram != NULL);
}

void snapshot_free(struct snapshot *s)
{
    free(s->ram);
    s->ram = NULL;
}

void ram_save(uint8_t *buf)
{
    const uint8_t *live = t1h_ram();
    for (uint64_t off = 0; off < T1H_RAM_SIZE; off += 4096)
        if (memcmp(buf + off, live + off, 4096))
            memcpy(buf + off, live + off, 4096);
}

void ram_load(const uint8_t *buf)
{
    uint8_t *live = t1h_ram();
    for (uint64_t off = 0; off < T1H_RAM_SIZE; off += 4096)
        if (memcmp(buf + off, live + off, 4096)) {
            memcpy(live + off, buf + off, 4096);
            code_notify_vmm_write(T1H_RAM_BASE + off, 4096);
        }
}

void snapshot_take(struct snapshot *s, const struct vcpu *v)
{
    s->v = *v;
    ram_save(s->ram);
}

void snapshot_restore(const struct snapshot *s, struct vcpu *v)
{
    /* struct vcpu owns two allocations through pointers; copying a vCPU that
     * has them would alias or leak them. */
    T1H_CHECK(!v->trace && !v->check_workspace && !s->v.trace && !s->v.check_workspace);
    ram_load(s->ram);
    *v = s->v;
    ia64_mmu_cache_reset(v);
    v->timer_cache.valid = false;
    v->memory_hook = NULL;
    v->physical_region = NULL;
    struct t1h_logs *logs = t1h_logs_of(v);
    if (logs) {
        t1h_logs_reset(logs);
        if (logs->stores_on)
            storelog_attach(v);
    }
}
