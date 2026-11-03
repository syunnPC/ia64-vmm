/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boundary cache. Each step runs
 * one 3-instruction region, so it crosses exactly one boundary, and is
 * compared with the interpreter; the counters show whether that boundary
 * used the cache. Hosted builds also compare every hit with the full
 * translation and key (a stale hit stops the test).
 */
static uint64_t boundary_hits(void)
{
    struct tier0_stats s; tier0_get_stats(&s);
    return s.boundary_hits;
}
/* Tier0 runs first: the interpreter's own validation must not repair what
 * a hit left undone. */
static void boundary_step(struct vcpu *v, struct vcpu *ref, bool hit)
{
    *ref = *v; ref->cfg.engine = ENGINE_INTERP;
    ref->check_workspace = NULL; ref->check_workspace_pages = 0;
    uint64_t before = boundary_hits();
    struct vcpu_exit actual = engine_run(v, 3);
    CHECK(boundary_hits() == before + hit);
    struct vcpu_exit expected = interp_run(ref, 3);
    CHECK(actual.reason == expected.reason && actual.ip == expected.ip && actual.slot == expected.slot);
    CHECK(!memcmp(&v->s, &ref->s, sizeof(v->s)) && v->retired == ref->retired);
}
static uint64_t self_loop(gpa_t at, uint64_t ip, uint64_t operation)
{
    bundle(at, 0x11, nop, operation, validator_branch(ip, ip));
    return ip;
}
/* The region of v's key at fetch address pa, as an arena index; capacity if none. */
static unsigned boundary_region(uint64_t ip, gpa_t pa)
{
    for (unsigned i = 0; i < tier0_arena.capacity; ++i) {
        const struct tier0_region *r = &tier0_arena.regions[i];
        if (r->valid && r->key.ip == ip && r->key.gpa == pa) return i;
    }
    return tier0_arena.capacity;
}
/* Key fields: PSR without its run-time fields, the frame
 * shape, not the rotation bases; and no lockstep. */
static void test_boundary_keys(struct vcpu *v, struct vcpu *ref)
{
    code_cache_flush(); restart(v, ENGINE_TIER0, self_loop(BASE, BASE, add(8,8,9)));
    v->s.gr[9] = 3;
    v->s.cfm = 16 | (UINT64_C(16) << 7) | (UINT64_C(1) << 14);   /* sof 16, sol 16, sor 8 */
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    boundary_step(v, ref, true);
    CHECK(v->s.gr[8] == 9);
    v->s.psr ^= IA64_PSR_IC;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    v->s.psr ^= IA64_PSR_IC;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    /* Run-time PSR fields (CODE_PSR_RUNTIME) are not key fields: the same
     * region keeps hitting, also with a stale PSR.ri after an rfi. */
    const uint64_t runtime[] = {IA64_PSR_DI, IA64_PSR_I, UINT64_C(2) << 41, UINT64_C(1) << 21, UINT64_C(1) << 3};
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(runtime); ++k) {
        v->s.psr ^= runtime[k];
        boundary_step(v, ref, true);
        v->s.psr ^= runtime[k];
        boundary_step(v, ref, true);
    }
    v->s.cfm += UINT64_C(1) << 18;                              /* rrb.gr */
    boundary_step(v, ref, true);
    v->s.cfm += UINT64_C(5) << 32;                              /* rrb.pr */
    boundary_step(v, ref, true);
    v->s.cfm += 1;                                              /* sof 17 */
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    v->cfg.engine = ENGINE_LOCKSTEP;
    boundary_step(v, ref, false);
    boundary_step(v, ref, false);
    v->cfg.engine = ENGINE_TIER0;
    boundary_step(v, ref, true);
}
/* A hit after a run-time PSR field changed obeys its live value. PSR.ac=1:
 * an unaligned load raises Unaligned Data Reference (SDM Vol.2 Table 3-2,
 * vector 0x5a00). PSR.i=1: a pending unmasked vector is taken at the
 * boundary (Vol.2 §5.8.2, vector 0x3000). PSR.db=1 with a data breakpoint
 * armed at the CPL: the boundary goes to the interpreter, which raises the
 * Data Debug fault (Vol.2 §7.1). Faults and interrupts run the nop
 * bundles at DEBUG_IVA. */
static void test_boundary_runtime_psr(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t ac = UINT64_C(1) << 3;
    debug_start(v, ENGINE_TIER0, 0);
    v->s.psr &= ~IA64_PSR_DB;
    bundle(BASE, 0x11, load(3,8,10), nop, validator_branch(BASE, BASE)); v->s.gr[10] = DATA;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    v->s.psr |= ac; v->s.gr[10] = DATA + 1;
    boundary_step(v, ref, true);
    CHECK(v->fault.kind == IA64_FAULT_UNALIGNED && v->s.cr[CR_IIP] == BASE && v->s.cr[CR_IFA] == DATA + 1);
    CHECK((v->s.ip & ~UINT64_C(0xff)) == DEBUG_IVA + 0x5a00);

    debug_start(v, ENGINE_TIER0, 0);
    v->s.psr &= ~IA64_PSR_DB;
    self_loop(BASE, BASE, add(8,8,9)); v->s.gr[9] = 1;
    vcpu_raise_external(v, 150);
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    v->s.psr |= IA64_PSR_I;
    boundary_step(v, ref, false);
    CHECK(v->s.cr[CR_IIP] == BASE && !(v->s.cr[CR_IPSR] >> 41 & 3) && (v->s.cr[CR_IPSR] & IA64_PSR_I));
    CHECK((v->s.ip & ~UINT64_C(0xff)) == DEBUG_IVA + 0x3000 && !(v->s.psr & IA64_PSR_I));

    debug_start(v, ENGINE_TIER0, 0);
    v->s.psr &= ~IA64_PSR_DB;
    bundle(BASE, 0x11, load(3,8,10), nop, validator_branch(BASE, BASE)); v->s.gr[10] = DATA;
    v->s.dbr[0] = DATA; v->s.dbr[1] = DEBUG_XR | DEBUG_PLM(0) | DEBUG_MASK;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    CHECK(v->fault.kind == IA64_FAULT_NONE);
    v->s.psr |= IA64_PSR_DB;
    struct tier0_stats before, after; tier0_get_stats(&before);
    boundary_step(v, ref, false);
    tier0_get_stats(&after);
    CHECK(after.interpreter_instructions[TIER0_INTERP_DEBUG] > before.interpreter_instructions[TIER0_INTERP_DEBUG]);
    debug_fault(v, BASE, 0, UINT64_C(1) << 34, DATA);
}
/* Item 2: a logged I-side mapping change moves the instruction-side
 * sequence; the next boundary translates and runs the newly mapped page. */
static void test_boundary_mmu(struct vcpu *v, struct vcpu *ref)
{
    code_cache_flush(); virtual_start(v, ENGINE_TIER0, VIRTUAL_IP);
    self_loop(BASE, VIRTUAL_IP, add(8,9,0));
    self_loop(BASE + 4096, VIRTUAL_IP, add(8,10,0));
    v->s.gr[9] = 11; v->s.gr[10] = 22;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    CHECK(v->s.gr[8] == 11);
    v->s.tr[0][0].valid = false;
    ia64_tc_insert(v, VIRTUAL_IP, 0, (BASE + 4096) | FETCH_FLAGS, 12 << 2);
    boundary_step(v, ref, false);
    CHECK(v->s.gr[8] == 22);
    boundary_step(v, ref, true);
}
/* In virtual mode an entry compares the instruction-side MMU
 * sequence. D-side TC and TR changes (itc.d or a walker insert, itr.d, a
 * purge) keep the hit; an I-side TC or TR change, an RR, a PKR and a reset
 * record each take the full path, also where the fetch translation stays
 * the same. In physical mode none of them does. Fixtures that edit entries
 * directly log the edit, as the architectural writers do. */
static void boundary_iside_changes(struct vcpu *v, struct vcpu *ref, bool hit)
{
    ia64_tc_insert(v, VIRTUAL_IP + 0x300000, 0, (BASE + 0x1000) | FETCH_FLAGS, 12 << 2);
    boundary_step(v, ref, hit); boundary_step(v, ref, true);
    virtual_map(v, 0, 3, VIRTUAL_IP + 0x400000, BASE + 0x2000, 12, FETCH_FLAGS);
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 3);
    boundary_step(v, ref, hit); boundary_step(v, ref, true);
    v->s.tr[0][3].valid = false; ia64_mmu_changed(v, IA64_MMU_TR, 0, 3);
    boundary_step(v, ref, hit); boundary_step(v, ref, true);
    v->s.rr[1] = 0x555500 | 48; ia64_mmu_changed(v, IA64_MMU_RR, 0, 1);
    boundary_step(v, ref, hit); boundary_step(v, ref, true);
    v->s.pkr[0] = UINT64_C(0x777700) | 1; ia64_mmu_changed(v, IA64_MMU_PKR, 0, 0);
    boundary_step(v, ref, hit); boundary_step(v, ref, true);
    ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0);
    boundary_step(v, ref, hit); boundary_step(v, ref, true);
}
static void test_boundary_iside(struct vcpu *v, struct vcpu *ref)
{
    code_cache_flush(); virtual_start(v, ENGINE_TIER0, VIRTUAL_IP);
    self_loop(BASE, VIRTUAL_IP, add(8,8,9));
    v->s.gr[9] = 1;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    ia64_tc_insert(v, VIRTUAL_IP + 0x100000, 1, DATA | FETCH_FLAGS, 12 << 2);
    boundary_step(v, ref, true);
    virtual_map(v, 1, 2, VIRTUAL_IP + 0x200000, DATA, 12, FETCH_FLAGS);
    ia64_mmu_changed(v, IA64_MMU_TR, 1, 2);
    boundary_step(v, ref, true);
    v->s.tr[1][2].valid = false; ia64_mmu_changed(v, IA64_MMU_TR, 1, 2);
    boundary_step(v, ref, true);
    boundary_iside_changes(v, ref, false);
    CHECK(v->s.gr[8] == 17);
    code_cache_flush(); restart(v, ENGINE_TIER0, self_loop(BASE, BASE, add(8,8,9)));
    v->s.gr[9] = 1; v->s.gr[8] = 0;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    boundary_iside_changes(v, ref, true);
    CHECK(v->s.gr[8] == 14);
}
/* Item 3: the serial. vCPU w maps the same IP to another page and
 * translates into the slot of v's retired region; v must not take w's
 * region from its entry. */
static void test_boundary_serial(struct vcpu *v, struct vcpu *ref)
{
    struct vcpu *w = vcpu_create(&(struct vcpu_config){.engine = ENGINE_TIER0, .itc_icount = true, .index = 1});
    CHECK(w != NULL);
    code_cache_flush(); tier0_reclaim();
    virtual_start(v, ENGINE_TIER0, VIRTUAL_IP);
    self_loop(BASE, VIRTUAL_IP, add(8,9,0));
    self_loop(BASE + 4096, VIRTUAL_IP, add(8,10,0));
    v->s.gr[9] = 11; v->s.gr[10] = 22;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    unsigned slot = boundary_region(VIRTUAL_IP, BASE);
    CHECK(slot < tier0_arena.capacity);
    self_loop(BASE, VIRTUAL_IP, add(8,9,0));                   /* a notified write retires it */
    CHECK(!tier0_arena.regions[slot].valid);
    virtual_start(w, ENGINE_TIER0, VIRTUAL_IP);
    w->s.tr[0][0].pte = BASE + 4096 + FETCH_FLAGS;
    w->s.gr[9] = 11; w->s.gr[10] = 22;
    CHECK(engine_run(w, 3).reason == VCPU_EXIT_BUDGET && w->s.gr[8] == 22);
    CHECK(boundary_region(VIRTUAL_IP, BASE + 4096) == slot);
    boundary_step(v, ref, false);
    CHECK(v->s.gr[8] == 11);
    vcpu_destroy(w);
}
/* A hit runs enter(): a stay that ended (a non-budget exit) compares the
 * bundle again, so DMA between stays is seen. */
static void test_boundary_enter(struct vcpu *v, struct vcpu *ref)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    code_cache_flush(); restart(v, ENGINE_TIER0, self_loop(BASE, BASE, add(8,9,0)));
    v->s.gr[9] = 11; v->s.gr[10] = 22;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    CHECK(v->s.gr[8] == 11);
    vcpu_request_exit(v, VCPU_EXIT_HALT);
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_HALT && v->s.ip == BASE);
    dma_bundle(BASE, 0x11, nop, add(8,10,0), validator_branch(BASE, BASE));
    boundary_step(v, ref, false);
    CHECK(v->s.gr[8] == 22);
}
/* Item 1: with CFLE set the fetch translation performs the mandatory RSE
 * loads (SDM Vol.2 §6.8), so a br.ret into a cached boundary with an
 * incomplete frame takes the full path. */
static void test_boundary_frame(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t br_ret = UINT64_C(0x108001100);
    const gpa_t backing = BASE + 0x6000;
    const uint64_t caller = 10 | (UINT64_C(8) << 7) | (UINT64_C(1) << 14) |
        (UINT64_C(3) << 18) | (UINT64_C(5) << 32);
    code_cache_flush();
    bundle(BASE, 0x11, nop, nop, br_ret);
    self_loop(BASE + 0x100, BASE + 0x100, add(40,32,33));
    for (unsigned i = 0; i < 8; ++i) { uint64_t x = random64(); CHECK(gm_write(backing + 8 * i, &x, 8)); }
    restart(v, ENGINE_TIER0, BASE + 0x100);
    v->s.psr = IA64_PSR_IC; v->s.cfm = caller; v->s.rse_bof = 40;
    for (unsigned p = 16; p < 128; ++p) { v->s.gr[p] = random64(); v->s.nat[p] = false; }
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    v->s.ip = BASE; v->s.slot = 0;
    v->s.ar[AR_PFS] = caller; v->s.br[0] = BASE + 0x100;
    v->s.rse_bof = 2; v->s.rse_dirty = 0; v->s.cfm = 2;
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = backing + 64;
    v->s.ar[AR_RNAT] = UINT64_C(0x24);
    boundary_step(v, ref, false);
    CHECK(v->s.ip == BASE + 0x100 && v->s.rse_missing == 8 && v->s.rse_cfle);
    boundary_step(v, ref, false);
    CHECK(!v->s.rse_missing && !v->s.rse_cfle);
    boundary_step(v, ref, true);
}
/* A restored state can repeat the instruction-side sequence with another
 * mapping: the restore hook forgets the vCPU's entries. */
static void test_boundary_restore(struct vcpu *v, struct vcpu *ref)
{
    static struct vcpu saved;
    code_cache_flush(); virtual_start(v, ENGINE_TIER0, VIRTUAL_IP);
    self_loop(BASE, VIRTUAL_IP, add(8,9,0));
    self_loop(BASE + 4096, VIRTUAL_IP, add(8,10,0));
    v->s.gr[9] = 11; v->s.gr[10] = 22;
    v->s.tr[0][0].valid = false;
    saved = *v;
    ia64_tc_insert(v, VIRTUAL_IP, 0, BASE | FETCH_FLAGS, 12 << 2);
    uint64_t seq = v->mmu_iseq;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    CHECK(v->s.gr[8] == 11);
    *v = saved; tier0_vcpu_state_restored(v);
    ia64_tc_insert(v, VIRTUAL_IP, 0, (BASE + 4096) | FETCH_FLAGS, 12 << 2);
    CHECK(v->mmu_iseq == seq);
    boundary_step(v, ref, false);
    CHECK(v->s.gr[8] == 22);
}
/* Exclusions: tracing; the vCPU (owner), even at an equal sequence;
 * PTA, which the MMU log does not cover. */
static void test_boundary_exclusions(struct vcpu *v, struct vcpu *ref)
{
    code_cache_flush(); restart(v, ENGINE_TIER0, self_loop(BASE, BASE, add(8,8,9)));
    v->s.gr[9] = 1;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    ia64_set_trace(v, 8);
    boundary_step(v, ref, false);
    boundary_step(v, ref, false);
    ia64_set_trace(v, 0);
    boundary_step(v, ref, true);
    code_cache_flush(); virtual_start(v, ENGINE_TIER0, VIRTUAL_IP);
    self_loop(BASE, VIRTUAL_IP, add(8,9,0));
    self_loop(BASE + 4096, VIRTUAL_IP, add(8,10,0));
    v->s.gr[9] = 11; v->s.gr[10] = 22;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    v->s.cr[CR_PTA] ^= UINT64_C(1) << 8;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    v->s.cr[CR_PTA] ^= UINT64_C(1) << 8;
    boundary_step(v, ref, false);
    /* w shares v's executor slots and sequence value but maps the IP elsewhere. */
    struct vcpu *w = vcpu_create(&(struct vcpu_config){.engine = ENGINE_TIER0, .itc_icount = true});
    CHECK(w != NULL);
    virtual_start(w, ENGINE_TIER0, VIRTUAL_IP);
    w->s.tr[0][0].pte = BASE + 4096 + FETCH_FLAGS;
    w->s.gr[9] = 11; w->s.gr[10] = 22;
    w->mmu_seq = v->mmu_seq; w->mmu_iseq = v->mmu_iseq;
    boundary_step(w, ref, false);
    CHECK(w->s.gr[8] == 22);
    vcpu_destroy(w);
}
/* A vCPU created where a destroyed one lived (same executor) never repeats
 * that one's sequence, so its entries do not apply. */
static void test_boundary_successor(struct vcpu *ref)
{
    struct vcpu_config cfg = {.engine = ENGINE_TIER0, .itc_icount = true, .index = 2};
    struct vcpu *a = vcpu_create(&cfg);
    CHECK(a != NULL);
    code_cache_flush(); virtual_start(a, ENGINE_TIER0, VIRTUAL_IP);
    self_loop(BASE, VIRTUAL_IP, add(8,9,0));
    self_loop(BASE + 4096, VIRTUAL_IP, add(8,10,0));
    a->s.gr[9] = 11; a->s.gr[10] = 22;
    boundary_step(a, ref, false);
    boundary_step(a, ref, true);
    uint64_t seq = a->mmu_iseq;
    vcpu_destroy(a);
    struct vcpu *b = vcpu_create(&cfg);
    CHECK(b != NULL);
    virtual_start(b, ENGINE_TIER0, VIRTUAL_IP);
    b->s.tr[0][0].pte = BASE + 4096 + FETCH_FLAGS;
    b->s.gr[9] = 11; b->s.gr[10] = 22;
    CHECK(b->mmu_iseq != seq);
    boundary_step(b, ref, false);
    CHECK(b->s.gr[8] == 22);
    vcpu_destroy(b);
}
/* Item 3: an entry outlives its region. Another executor's translations
 * wrap the small arena's chunk ring and evict the region's chunk; v's
 * entry is then stale (the region retired, or its slot reused under
 * another serial). */
static void test_boundary_eviction(struct vcpu *v, struct vcpu *ref)
{
    CHECK(tier0_configure(1048576));
    code_cache_flush(); restart(v, ENGINE_TIER0, self_loop(BASE, BASE, add(8,8,9)));
    v->s.gr[9] = 1;
    boundary_step(v, ref, false);
    boundary_step(v, ref, true);
    unsigned slot = boundary_region(BASE, BASE);
    CHECK(slot < tier0_arena.capacity);
    uint64_t serial = tier0_arena.regions[slot].serial;
    struct vcpu *w = vcpu_create(&(struct vcpu_config){.engine = ENGINE_TIER0, .itc_icount = true, .index = 1});
    CHECK(w != NULL);
    restart(w, ENGINE_TIER0, BASE);
    struct tier0_stats before, after; tier0_get_stats(&before);
    for (unsigned n = 1; n <= 2000; ++n) {
        arena_key(w, n);
        CHECK(engine_run(w, 3).reason == VCPU_EXIT_BUDGET);
    }
    tier0_get_stats(&after);
    CHECK(after.evictions > before.evictions);
    CHECK(!tier0_arena.regions[slot].valid || tier0_arena.regions[slot].serial != serial);
    before = after;
    boundary_step(v, ref, false);
    tier0_get_stats(&after);
    CHECK(after.boundary_stale == before.boundary_stale + 1);
    boundary_step(v, ref, true);
    vcpu_destroy(w);
    CHECK(tier0_configure(TIER0_CODE_DEFAULT_BYTES));
}
/* Capacity: the heads of eight consecutive pages never displace each other.
 * 96 one-bundle regions at scattered bundles of those pages form a cycle;
 * after warming, every boundary of a cycle hits. */
static void test_boundary_capacity(struct vcpu *v, struct vcpu *ref)
{
    enum { HEADS = 96 };
    const gpa_t page = BASE + 0x4000;
    uint64_t head[HEADS];
    for (unsigned k = 0; k < HEADS; ++k) head[k] = page + 16 * ((k * 181 + 5) % 2048);
    code_cache_flush();
    for (unsigned k = 0; k < HEADS; ++k)
        bundle(head[k], 0x11, nop, add(8,8,9), validator_branch(head[k], head[(k + 1) % HEADS]));
    restart(v, ENGINE_TIER0, head[0]); v->s.gr[9] = 1;
    for (unsigned warm = 0; warm < 2; ++warm) validator_run(v, 3 * HEADS);
    for (unsigned k = 0; k < HEADS; ++k) boundary_step(v, ref, true);
    CHECK(v->s.ip == head[0] && v->s.gr[8] == 3 * HEADS);
    code_cache_flush();
}
/* Two ways per set: the set of IP and slot is
 * (IP >> 4) ^ (IP >> 15) ^ (slot << 9), modulo 2048, so heads 32 KiB + 16
 * bytes apart share a set. Two such heads alternating both stay. A fill
 * moves the set's first entry to the second: a third head entered once
 * evicts the older of them, and the other's next fill evicts the second. */
static void test_boundary_ways(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t a = BASE + 0x2000, b = a + 0x8000 + 16, c = b + 0x8000 + 16;
    CHECK((((a >> 4) ^ (a >> 15)) & 2047) == (((b >> 4) ^ (b >> 15)) & 2047));
    CHECK((((a >> 4) ^ (a >> 15)) & 2047) == (((c >> 4) ^ (c >> 15)) & 2047));
    code_cache_flush();
    bundle(a, 0x11, nop, add(8,8,9), validator_branch(a, b));
    bundle(b, 0x11, nop, add(8,8,9), validator_branch(b, a));
    bundle(c, 0x11, nop, add(8,8,9), validator_branch(c, a));
    restart(v, ENGINE_TIER0, a); v->s.gr[9] = 1;
    for (unsigned warm = 0; warm < 2; ++warm) validator_run(v, 6);
    for (unsigned k = 0; k < 6; ++k) boundary_step(v, ref, true);
    CHECK(v->s.ip == a);
    v->s.ip = c;
    static const bool hits[] = {false, false, false, true, true, true};   /* c, a, b, a, b, a */
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(hits); ++k) boundary_step(v, ref, hits[k]);
    code_cache_flush();
}
/* Entries are per IP and slot: the regions at slots 0 and 1 of one bundle
 * (the second entered after a budget exit) each hit their own entry; the
 * hosted check compares every hit with the full lookup. */
static void test_boundary_slots(struct vcpu *v)
{
    code_cache_flush(); restart(v, ENGINE_TIER0, self_loop(BASE, BASE, add(8,8,9))); v->s.gr[9] = 1;
    for (unsigned n = 0; n < 4; ++n) {
        v->s.ip = BASE; v->s.slot = 1;
        CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET && v->s.ip == BASE && v->s.slot == 0);
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->s.ip == BASE && v->s.slot == 0);
    }
    uint64_t hits = boundary_hits();
    v->s.slot = 1; CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET);
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && boundary_hits() == hits + 2 && v->s.gr[8] == 10);
    code_cache_flush();
}
static void test_boundary(struct vcpu *v, struct vcpu *ref)
{
    test_boundary_keys(v, ref);
    test_boundary_runtime_psr(v, ref);
    test_boundary_mmu(v, ref);
    test_boundary_iside(v, ref);
    test_boundary_serial(v, ref);
    test_boundary_enter(v, ref);
    test_boundary_frame(v, ref);
    test_boundary_restore(v, ref);
    test_boundary_exclusions(v, ref);
    test_boundary_successor(ref);
    test_boundary_eviction(v, ref);
    test_boundary_capacity(v, ref);
    test_boundary_ways(v, ref);
    test_boundary_slots(v);
}
