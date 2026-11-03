/* SPDX-License-Identifier: GPL-2.0-or-later */
static void arena_permissions(const void *p, bool executable)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    CHECK(maps != NULL);
    char line[512], permissions[5];
    unsigned long begin, end;
    bool found = false;
    while (fgets(line, sizeof(line), maps))
        if (sscanf(line, "%lx-%lx %4s", &begin, &end, permissions) == 3 &&
            (uintptr_t)p >= begin && (uintptr_t)p < end) {
            CHECK(permissions[0] == 'r');
            CHECK(permissions[1] == (executable ? '-' : 'w'));
            CHECK(permissions[2] == (executable ? 'x' : '-'));
            found = true; break;
        }
    fclose(maps); CHECK(found);
}
static const void *arena_checked_allocation;
static struct tier0_stats arena_during;
static void arena_write_hook(struct vcpu *v)
{
    CHECK(tier0_execution_active() && v->check_context != NULL);
    struct tier0_stats before, after; tier0_get_stats(&before);
    code_notify_cpu_write(BASE, 16);
    CHECK(!tier0_page_has_code(BASE));
    tier0_get_stats(&after);
    CHECK(after.arena_bytes == before.arena_bytes && after.reclaimed == before.reclaimed);
    arena_permissions(arena_checked_allocation, true);
    arena_during = after;
}
static void arena_key(struct vcpu *v, unsigned n)
{
    v->s.ip = BASE; v->s.slot = 0;
    unsigned sof = 0, sol = n;
    while (sol > sof) { sol -= sof + 1; ++sof; }
    v->s.cfm = sof | ((uint64_t)sol << 7); v->s.rse_bof = n % 96;
}
static void test_arena(struct vcpu *v)
{
    CHECK(!tier0_configure(0) && !tier0_configure(1048577));
    CHECK(!tier0_configure_cmdline("tier0_code_mib="));
    CHECK(!tier0_configure_cmdline("tier0_code_mib=0"));
    CHECK(!tier0_configure_cmdline("tier0_code_mib=18446744073709551615"));
    CHECK(!tier0_configure_cmdline("tier0_code_mib=-1"));
    /* Default code bytes: a quarter of the reserve (an eighth with Tier1),
     * whole MiB, within 32..1024 MiB. */
    const uint64_t mib = 1048576;
    CHECK(tier0_default_code_bytes(0, false) == 32 * mib && tier0_default_code_bytes(128 * mib, false) == 32 * mib);
    CHECK(tier0_default_code_bytes(256 * mib, false) == 64 * mib && tier0_default_code_bytes(256 * mib, true) == 32 * mib);
    CHECK(tier0_default_code_bytes(512 * mib, true) == 64 * mib && tier0_default_code_bytes(1000 * mib, false) == 250 * mib);
    CHECK(tier0_default_code_bytes(300 * mib + 4096, false) == 75 * mib);
    CHECK(tier0_default_code_bytes(UINT64_C(4096) * mib, false) == 1024 * mib);
    CHECK(tier0_default_code_bytes(UINT64_C(16384) * mib, true) == 1024 * mib);
    CHECK(tier0_default_code_bytes(UINT64_MAX, false) == 1024 * mib);
    CHECK(tier0_configure_cmdline("x=1\ttier0_code_mib=1 y=2"));
    CHECK(code_validity_configure(CODE_VALIDITY_NONE, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(BASE, 0x11, nop, nop, validator_branch(BASE, BASE));
    bundle(BASE + 4096, 0x11, nop, nop, validator_branch(BASE + 4096, BASE + 4096));
    restart(v, ENGINE_TIER0, BASE);
    struct tier0_stats before, after; tier0_get_stats(&before);
    validator_run(v, 3);
    const void *a, *entry_a, *b, *entry_b; bool executable;
    CHECK(tier0_test_mapping(v, &a, &entry_a, &executable) && !executable);
    arena_permissions(a, false);
    v->s.ip = BASE + 4096; validator_run(v, 3);
    CHECK(tier0_test_mapping(v, &b, &entry_b, &executable) && !executable);
    CHECK(a != b && ((uintptr_t)a >> 12) == ((uintptr_t)b >> 12));
    CHECK((uintptr_t)b - (uintptr_t)a < 4096);
    arena_permissions(b, false);
    v->s.ip = BASE; validator_run(v, 3);
    CHECK(tier0_test_mapping(v, &a, &entry_a, &executable) && executable);
    arena_permissions(a, true); arena_permissions(b, true);
    tier0_get_stats(&after);
    CHECK(after.publications == before.publications + 2);
    CHECK(after.protection_changes == before.protection_changes + 1);
    before = after;
    code_notify_vmm_write(BASE + 4096, 16);
    CHECK(!tier0_page_has_code(BASE + 4096) && tier0_page_has_code(BASE));
    tier0_get_stats(&after);
    CHECK(after.arena_bytes == before.arena_bytes && after.reclaimed == before.reclaimed);
    arena_permissions(a, true); arena_permissions(b, true);
    validator_run(v, 3); tier0_get_stats(&after);
    CHECK(after.reclaimed == before.reclaimed + 1 && after.arena_bytes < before.arena_bytes);
    CHECK(after.protection_changes == before.protection_changes);
    /* Fill and wrap the small arena. The oldest sealed chunk must leave
     * lookup while the newest chunk and its key survive. */
    before = after;
    for (unsigned n = 1; n <= 2000; ++n) {
        arena_key(v, n); validator_run(v, 3);
    }
    tier0_get_stats(&after);
    CHECK(after.evictions > before.evictions);
    CHECK(after.arena_bytes <= 1048576 && after.protection_changes - before.protection_changes < 200);
    before = after; arena_key(v, 2000); validator_run(v, 3); tier0_get_stats(&after);
    CHECK(after.translations == before.translations && after.hits == before.hits + 1);
    arena_key(v, 0);
    CHECK(!tier0_test_mapping(v, &a, &entry_a, &executable));
    validator_run(v, 3); tier0_get_stats(&after);
    CHECK(after.translations == before.translations + 1);
    /* With registered executors, evicted regions are reclaimed after every
     * executor's next quiescent boundary. A full ring evicts its oldest
     * chunk (FIFO), by code space or by region metadata, never the whole
     * ring, and the newest keys survive. Chunks and region records are
     * evicted ahead of need: a peer that passes a boundary every 4 keys
     * never makes a translation wait (no interpreter fallback). A peer that
     * stops for 64 keys makes translations wait, still without evicting
     * the ring. */
    struct vcpu *peer = vcpu_create(&(struct vcpu_config){.engine = ENGINE_TIER0, .itc_icount = true, .index = 1});
    CHECK(peer != NULL);
    for (unsigned large = 0; large < 2; ++large) {
        /* Three instructions: metadata fills first; 24: code space does. */
        for (unsigned k = 0; large && k < 7; ++k) bundle(BASE + 16 * k, 0, nop, add(8,8,9), nop);
        bundle(BASE + 16 * 7 * large, 0x11, nop, nop, validator_branch(BASE + 16 * 7 * large, BASE));
        CHECK(tier0_configure_cmdline("tier0_code_mib=1"));
        code_register_vcpu(v); code_register_vcpu(peer); restart(v, ENGINE_TIER0, BASE);
        uint64_t wraps = 0, steps = large ? 24 : 3, keys = large ? 600 : 4000, newest = large ? 64 : 256;
        struct tier0_stats first, frozen = {0}, thawed = {0}; tier0_get_stats(&first);
        const unsigned freeze = keys / 2 / 16 * 16;
        for (unsigned n = 0; n < keys; ++n) {
            if (n == freeze) tier0_get_stats(&frozen);
            if (n == freeze + 64) tier0_get_stats(&thawed);
            if (!(n % 4) && (n < freeze || n >= freeze + 64)) code_quiescent(peer);
            if (!(n % 16)) tier0_get_stats(&before);
            arena_key(v, n); validator_run(v, steps);
            if (n % 16 != 15) continue;
            tier0_get_stats(&after);
            if (after.evictions == before.evictions) continue;
            ++wraps;
            CHECK(after.evictions - before.evictions < (large ? 64 : 1048576 / 1024 / 4));
        }
        CHECK(wraps > 0);
        CHECK(frozen.interpreter_instructions[TIER0_INTERP_CACHE_FAILURE] ==
              first.interpreter_instructions[TIER0_INTERP_CACHE_FAILURE]);
        CHECK(after.interpreter_instructions[TIER0_INTERP_CACHE_FAILURE] ==
              thawed.interpreter_instructions[TIER0_INTERP_CACHE_FAILURE]);
        tier0_get_stats(&before);
        for (unsigned n = keys - 1; n >= keys - newest; --n) { arena_key(v, n); validator_run(v, steps); }
        tier0_get_stats(&after);
        CHECK(after.translations == before.translations && after.hits == before.hits + newest);
        code_unregister_vcpu(peer); code_unregister_vcpu(v);
    }
    /* Region records running out long before the ring wraps (small regions
     * in 2 MiB): the record reserve evicts old chunks, while untouched
     * chunks need no grace period. With a peer that never passes a boundary
     * after registering, every key translates as long as a record is free,
     * also after the reserve evicted. */
    bundle(BASE, 0x11, nop, nop, validator_branch(BASE, BASE));
    CHECK(tier0_configure_cmdline("tier0_code_mib=2"));
    code_register_vcpu(v); code_register_vcpu(peer); restart(v, ENGINE_TIER0, BASE);
    struct tier0_stats fresh; tier0_get_stats(&fresh);
    const unsigned records = tier0_arena.capacity;
    for (unsigned n = 0; n < records; ++n) { arena_key(v, n); validator_run(v, 3); }
    tier0_get_stats(&after);
    CHECK(after.translations == fresh.translations + records && after.evictions > fresh.evictions);
    CHECK(after.interpreter_instructions[TIER0_INTERP_CACHE_FAILURE] ==
          fresh.interpreter_instructions[TIER0_INTERP_CACHE_FAILURE]);
    CHECK(after.allocated_bytes - fresh.allocated_bytes < 2 * 1048576 - 2 * TIER0_CHUNK_BYTES);
    code_unregister_vcpu(peer); code_unregister_vcpu(v);
    vcpu_destroy(peer);
    bundle(BASE, 0x11, nop, nop, validator_branch(BASE, BASE));
    /* Thousands of different keys fit without evicting at the default cap. */
    CHECK(tier0_configure(TIER0_CODE_DEFAULT_BYTES));
    restart(v, ENGINE_TIER0, BASE); tier0_get_stats(&before);
    for (unsigned n = 0; n < 3000; ++n) { arena_key(v, n); validator_run(v, 3); }
    for (unsigned n = 0; n < 3000; ++n) { arena_key(v, n); validator_run(v, 3); }
    tier0_get_stats(&after);
    CHECK(after.translations == before.translations + 3000 && after.hits == before.hits + 3000);
    CHECK(after.evictions == before.evictions && after.arena_bytes < TIER0_CODE_DEFAULT_BYTES);
    /* Invalidation from an RX helper cannot recycle its operands before
     * the generated return address and the lockstep replay are finished. */
    code_cache_flush();
    bundle(BASE, 0x11, store(3,9,10), nop, validator_branch(BASE, BASE));
    bundle(BASE + 4096, 0x11, nop, nop, validator_branch(BASE + 4096, BASE + 4096));
    restart(v, ENGINE_LOCKSTEP, BASE); v->s.gr[10] = DEVICE;
    validator_run(v, 3);
    v->s.ip = BASE + 4096; validator_run(v, 3);
    v->s.ip = BASE; validator_run(v, 3);
    CHECK(tier0_test_mapping(v, &arena_checked_allocation, &entry_a, &executable) && executable);
    tier0_get_stats(&before);
    arena_invalidate_on_write = true; validator_run(v, 1); arena_invalidate_on_write = false;
    tier0_get_stats(&after);
    CHECK(after.checked_regions == before.checked_regions + 1);
    CHECK(after.reclaimed == arena_during.reclaimed && after.arena_bytes == arena_during.arena_bytes);
    CHECK(tier0_page_has_code(BASE + 4096));
    arena_permissions(arena_checked_allocation, true);
    v->s.ip = BASE + 4096; v->s.slot = 0; validator_run(v, 3); tier0_get_stats(&after);
    CHECK(after.reclaimed > arena_during.reclaimed && after.arena_bytes < arena_during.arena_bytes);
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    CHECK(after.unchecked_instructions == before.unchecked_instructions);
}
