/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Ia64/Firmware/SalSystemTableParser.cpp,
 * Tests/Platform/Ia64/Firmware/SalSystemTableParserTests.cpp
 */
/* SAL table layouts follow Intel SAL §3.2.6.
 * No TR descriptor is emitted unless firmware actually owns that mapping.
 */
#include "sal.h"
bool sal_build(struct sal_system_table *t, uint64_t base, uint64_t pal, uint64_t sal, uint64_t gp,
               unsigned cpus, const struct sal_memory *memory, size_t n, const struct sal_tr *trs,
               size_t tr_count)
{
    if (!t || !cpus || cpus > TABLE_MAX_CPUS || (n && !memory) || (tr_count && !trs) || n > 128 ||
        tr_count > 1 || !pal || !sal || !gp || (pal & 15) || (sal & 15) || (gp & 7) || base % 8 ||
        base > UINT64_MAX - sizeof(t->bytes))
        return false;
    table_zero(t, sizeof(*t));
    uint8_t *p = t->bytes;
    table_text(p, "SST_", 4);
    table_put(p + 8, 0x0320, 2);
    table_put(p + 10, 4 + n + tr_count, 2);
    table_put(p + 20, 0x0100, 2);
    table_put(p + 22, 0x0100, 2);
    table_text(p + 24, "IA64-VMM", 8);
    table_text(p + 56, "IA-64 VMM virtual platform", 26);
    size_t at = 96;
    p += at;
    table_put(p + 8, pal, 8);
    table_put(p + 16, sal, 8);
    table_put(p + 24, gp, 8);
    at += 48;
    for (size_t i = 0; i < n; ++i, at += 32) {
        const struct sal_memory *m = memory + i;
        if (m->base % 4096 || !m->size || m->size % 4096 || m->size / 4096 > UINT32_MAX ||
            m->size > UINT64_MAX - m->base || m->attribute > 7 || m->usage > 9)
            return false;
        p = t->bytes + at;
        p[0] = 1;
        p[2] = m->attribute;
        p[3] = 3;
        p[4] = (uint8_t)(1u << m->attribute);
        p[7] = m->usage;
        table_put(p + 8, m->base, 8);
        table_put(p + 16, m->size / 4096, 4);
    }
    p = t->bytes + at;
    p[0] = 2;
    at += 16;
    for (size_t i = 0; i < tr_count; ++i, at += 32) {
        if (trs[i].kind || trs[i].index || (trs[i].itir & 3))
            return false;
        p = t->bytes + at;
        p[0] = 3;
        p[1] = trs[i].kind;
        p[2] = trs[i].index;
        table_put(p + 8, trs[i].address, 8);
        table_put(p + 16, trs[i].itir, 8);
    }
    /* SAL §3.2.6: every vCPU belongs to one platform coherence domain.
     * The CPU's SMP broadcast implementation remains a VMM prerequisite. */
    size_t domains = at + 32, lids = domains + 16;
    if (lids + cpus * 2 > sizeof(t->bytes))
        return false;
    p = t->bytes + at;
    p[0] = 4;
    table_put(p + 4, 1, 4);
    table_put(p + 8, base + domains, 8);
    at += 16;
    p = t->bytes + at;
    p[0] = 5;
    table_put(p + 8, 0xff, 8);
    at += 16;
    /* Auxiliary arrays remain outside Length/EntryCount but in runtime RAM. */
    table_put(t->bytes + domains, cpus, 8);
    table_put(t->bytes + domains + 8, base + lids, 8);
    for (unsigned i = 0; i < cpus; ++i) {
        table_put(t->bytes + lids + i * 2, i, 2);
    }
    table_put(t->bytes + 4, at, 4);
    t->bytes[12] = (uint8_t)-table_sum(t->bytes, at);
    t->used = lids + cpus * 2;
    return true;
}
