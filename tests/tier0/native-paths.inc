/* SPDX-License-Identifier: GPL-2.0-or-later */
static void bench_time(const char *name, uint64_t start, unsigned count)
{
    printf("native-bench %s count=%u ticks=%llu hz=%llu\n", name, count,
        (unsigned long long)(hal_counter() - start), (unsigned long long)hal_counter_hz());
}
static void bench_native_paths(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(DATA, 0x11, nop, nop, validator_branch(DATA, DATA));
    restart(v, ENGINE_TIER0, BASE);
    uint64_t low, high, version;
    struct code_dependency d = {.invalidate = stay_retire, .mask = {1}};
    CHECK(code_source_read(v, DATA, &low, &high, &version).kind == IA64_STEP_COMPLETE);
    code_dependency_attach(DATA, &d); code_dependency_remove(&d);
    CHECK(!code_page_is_tracked(DATA));
    uint64_t start = hal_counter();
    for (unsigned n = 0; n < 500000; ++n) code_notify_cpu_write(DATA + 512, 8);
    bench_time("untracked-notify", start, 500000);
    Ia64Instruction i = {.opcode = IA64_OP_ST8, .valid = true, .unit = IA64_UNIT_M,
        .address = BASE, .operands.memory = {.source = 9, .base = 10}};
    struct tier0_region region = {.valid = true, .count = 1, .insns = &i, .slots = {1}};
    struct tier0_execution e = {.region = &region};
    v->s.gr[9] = 42; v->s.gr[10] = DATA + 512;
    start = hal_counter();
    for (unsigned n = 0; n < 500000; ++n) CHECK(!tier0_helper(v, &i, &e));
    bench_time("helper-store", start, 500000);
    bundle(BASE, 1, nop, add(8,8,9), nop);
    bundle(BASE + 0x40000, 1, nop, add(8,8,9), nop);
    code_cache_flush();
    struct code_cache_stats before, after; code_cache_get_stats(&before);
    const struct ia64_bundle *decoded;
    start = hal_counter();
    for (unsigned n = 0; n < 50000; ++n)
        CHECK(code_get_bundle(v, n & 1 ? BASE : BASE + 0x40000, &decoded).kind == IA64_STEP_COMPLETE);
    bench_time("decoded-conflict", start, 50000);
    code_cache_get_stats(&after);
    printf("native-bench decoded-count decodes=%llu hits=%llu\n", (unsigned long long)(after.decodes - before.decodes),
        (unsigned long long)(after.hits - before.hits));
    bench_bundle_stores(v);
    for (unsigned kind = 0; kind < 4; ++kind) {
        code_cache_flush();
        for (unsigned n = 0; n < 8; ++n) {
            uint64_t m = kind ? load(3,8,10) : nop;
            if (kind == 3) m = (m & ~(UINT64_C(0xf) << 37)) | (UINT64_C(5) << 37);
            bundle(BASE + 16 * n, n == 7 ? 0x11 : 1, m, add(9,9,11),
                n == 7 ? validator_branch(BASE + 112, BASE) : add(11,11,9));
        }
        struct tier0_stats generated_before, generated_after; tier0_get_stats(&generated_before);
        restart(v, ENGINE_TIER0, BASE);
        v->s.gr[10] = kind == 2 ? DATA + 128 : BASE + 512;
        validator_run(v, 48);
        tier0_get_stats(&generated_after);
        printf("native-bench code-bytes kind=%u bytes=%llu\n", kind,
            (unsigned long long)(generated_after.bytes - generated_before.bytes));
        start = hal_counter(); validator_run(v, 2400000);
        const char *names[] = {"prefix-24", "code-load", "physical-load", "post-load"};
        bench_time(names[kind], start, 2400000);
    }
}
static void test_provisional_source_read(struct vcpu *v)
{
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_NONE; ++mode) {
        CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
        bundle(DATA, 0x11, nop, nop, validator_branch(DATA, DATA));
        restart(v, ENGINE_TIER0, BASE);
        uint64_t low, high, version;
        struct code_dependency d = {.invalidate = stay_retire, .mask = {1}};
        CHECK(code_source_read(v, DATA, &low, &high, &version).kind == IA64_STEP_COMPLETE);
        code_dependency_attach(DATA, &d); code_dependency_remove(&d);
        CHECK(!code_page_is_tracked(DATA));
        CHECK(ia64_physical_write(v, DATA, 8, 42).kind == IA64_STEP_COMPLETE);
        CHECK(code_source_read(v, DATA, &low, &high, &version).kind == IA64_STEP_COMPLETE && low == 42);
        /* A provisional read is tracked before any dependent attaches.
         * Removing this distinction admits a stale epoch-mode publication. */
        if (mode != CODE_VALIDITY_NONE) {
            CHECK(code_page_is_tracked(DATA));
            uint64_t stale = version;
            CHECK(ia64_physical_write(v, DATA, 8, 43).kind == IA64_STEP_COMPLETE);
            CHECK(!code_dependency_publish(v, DATA, stale, &d));
            CHECK(code_source_read(v, DATA, &low, &high, &version).kind == IA64_STEP_COMPLETE && low == 43);
            stale = version;
            code_notify_cpu_write(0, UINT64_MAX);
            CHECK(!code_dependency_publish(v, DATA, stale, &d));
            CHECK(code_source_read(v, DATA, &low, &high, &version).kind == IA64_STEP_COMPLETE && low == 43);
            code_notify_cpu_write(DATA, 0);
            CHECK(code_dependency_publish(v, DATA, version, &d));
        }
    }
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
}
static void test_budget_prefix_regions(struct vcpu *v, struct vcpu *ref)
{
    for (enum engine_kind engine = ENGINE_TIER0; engine <= ENGINE_LOCKSTEP; ++engine) {
        code_cache_flush();
        for (unsigned n = 0; n < 8; ++n)
            bundle(BASE + 16 * n, n == 7 ? 0x11 : 1, nop, add(8,8,9),
                n == 7 ? validator_branch(BASE + 112, BASE) : add(9,9,8));
        restart(v, engine, BASE);
        validator_run(v, 24); /* Stage, then seal the 24-instruction region. */
        validator_run(v, 24);
        for (unsigned budget = 1; budget <= 49; ++budget) {
            v->s.ip = BASE; v->s.slot = 0;
            v->s.gr[8] = 1; v->s.gr[9] = 2;
            v->s.nat[8] = budget % 2; v->s.nat[9] = budget % 3 == 0;
            frame_compare(v, ref, budget);
        }
        /* The mailbox is consumed once; a subsequent run can progress. */
        v->irq_kick = 1;
        uint64_t retired = v->retired;
        CHECK(engine_run(v, 24).reason == VCPU_EXIT_BUDGET && v->retired == retired && !v->irq_kick);
        vcpu_request_exit(v, VCPU_EXIT_HALT);
        CHECK(engine_run(v, 24).reason == VCPU_EXIT_HALT && v->retired == retired && !v->requested_exit);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == retired + 1);
    }
}
static void test_staged_pointer_sizing(struct vcpu *v)
{
    CHECK(tier0_arena_ensure());
    Ia64Instruction high[3] = {
        {.opcode = IA64_OP_ADDS, .valid = true, .address = BASE, .slot = 0,
         .operands.integer = {.destination = 8, .source2 = 8, .immediate = 1}},
        {.opcode = IA64_OP_LD8, .valid = true, .address = BASE, .slot = 1,
         .operands.memory = {.destination = 9, .base = 10}},
        {.opcode = IA64_OP_NOP, .valid = true, .address = BASE, .slot = 2},
    };
    /* Reproduce caller/arena pointer classes without relying on ASLR to
     * put the ordinary shim arena below 4 GiB. MAP_32BIT is AMD64 Linux. */
    Ia64Instruction *low = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(low != MAP_FAILED && (uintptr_t)low <= UINT32_MAX);
    CHECK((uintptr_t)high > UINT32_MAX);
    memcpy(low, high, sizeof(high));
    restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC;
    uint8_t bytes[4096];
    for (unsigned tracing = 0; tracing < 2; ++tracing) {
        struct tier0_region r = {.valid = true, .count = 3, .insns = low,
            .slots = {1,1,1}, .key.tracing = tracing};
        struct x86_buffer measured = {.capacity = SIZE_MAX};
        tier0_emit(&measured, &r, &v->s);
        CHECK(!measured.failed && measured.size <= sizeof(bytes));
        r.insns = high;
        struct x86_buffer written = {.data = bytes, .capacity = measured.size};
        tier0_emit(&written, &r, &v->s);
        CHECK(!written.failed && written.size == measured.size);
        /* Exercise tier0_stage's actual copy, equality check and RX entry. */
        struct tier0_region *staged = tier0_region_allocate(); CHECK(staged != NULL);
        *staged = (struct tier0_region){.valid = true, .count = 3,
            .slots = {1,1,1}, .key.tracing = tracing};
        CHECK(tier0_stage(staged, low, &v->s));
        CHECK((uintptr_t)staged->insns > UINT32_MAX && staged->bytes == measured.size);
        tier0_seal(v, staged->chunk);
        v->s.gr[8] = 5; v->s.gr[10] = DATA + 128;
        v->retired = 17;
        struct tier0_code_span span = {0};
        struct tier0_execution e = {.region = staged, .stats = tier0_local_stats(), .span = &span};
        tier0_arena.executing = true;
        ((void (*)(struct vcpu *, uint64_t, struct tier0_execution *, const struct tier0_homes *))
            staged->entry)(v, 3, &e, NULL);
        tier0_arena.executing = false;
        CHECK(!e.stop && !e.fault && v->retired == 20 && v->s.gr[8] == 6);
        CHECK(v->s.ip == BASE + 16 && v->s.slot == 0);
        tier0_region_retire(staged, false); tier0_reclaim();
    }
    CHECK(!munmap(low, 4096));
}
static void test_native_code_tracking(struct vcpu *v)
{
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_NONE; ++mode)
        for (unsigned provisional = 0; provisional < 2; ++provisional) {
            if (provisional && mode == CODE_VALIDITY_NONE) continue; /* No snapshots in NONE. */
            CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
            bundle(DATA, 1, nop, add(8,9,10), nop);
            restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC;
            Ia64Instruction i = {.opcode = IA64_OP_ST8, .valid = true, .address = BASE,
                .operands.memory = {.source = 9, .base = 10}};
            struct tier0_region *r = tier0_region_allocate(); CHECK(r != NULL);
            *r = (struct tier0_region){.valid = true, .count = 1, .slots = {1}};
            CHECK(tier0_stage(r, &i, &v->s)); tier0_seal(v, r->chunk);
            uint64_t low, high, version;
            const struct ia64_bundle *decoded;
            struct code_dependency d = {.invalidate = stay_retire, .mask = {1}};
            if (provisional) {
                CHECK(code_source_read(v, DATA, &low, &high, &version).kind == IA64_STEP_COMPLETE);
                CHECK(code_page_has_snapshot(DATA) && !code_page_has_dependencies(DATA));
            } else {
                CHECK(code_get_bundle(v, DATA, &decoded).kind == IA64_STEP_COMPLETE);
                CHECK(decoded->insn[1].operands.integer.destination == 8);
            }
            /* No translation of the destination page exists: the native
             * physical guard must detect just the decode or formation. */
            CHECK(!tier0_page_has_code(DATA) && code_page_has_code(DATA));
            v->s.gr[9] = 1 | (nop << 5) | (add(11,9,10) << 46);
            v->s.gr[10] = DATA;
            CHECK(ia64_physical_region(v, DATA, 8) != NULL);
            struct tier0_code_span span = {0};
            struct tier0_execution e = {.region = r, .stats = tier0_local_stats(), .span = &span};
            tier0_arena.executing = true;
            ((void (*)(struct vcpu *, uint64_t, struct tier0_execution *, const struct tier0_homes *))
                r->entry)(v, 1, &e, NULL);
            tier0_arena.executing = false;
            CHECK(!e.stop && !e.fault && v->retired == 1);
            if (provisional) CHECK(!code_dependency_publish(v, DATA, version, &d));
            struct code_cache_stats before, after; code_cache_get_stats(&before);
            CHECK(code_get_bundle(v, DATA, &decoded).kind == IA64_STEP_COMPLETE);
            CHECK(decoded->insn[1].operands.integer.destination == 11);
            code_cache_get_stats(&after); CHECK(after.decodes == before.decodes + 1);
            tier0_region_retire(r, false); tier0_reclaim();
        }
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
}
static void test_native_memory_guards(struct vcpu *v, struct vcpu *ref)
{
    /* Make each alignment guard necessary on its own: guest misalignment
     * with aligned backing, then an aligned guest with misaligned backing. */
    const uint64_t alignment_addresses[] = {DEVICE + 32769, DEVICE + 16384};
    for (unsigned at = 0; at < IA64VMM_ARRAY_SIZE(alignment_addresses); ++at)
        for (unsigned store_op = 0; store_op < 2; ++store_op)
            for (unsigned width = 1; width < 4; ++width) {
                restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC;
                v->s.gr[9] = 42; v->s.gr[10] = alignment_addresses[at];
                CHECK(ia64_physical_region(v, v->s.gr[10], 1u << width) != NULL);
                Ia64Instruction i = {.valid = true, .unit = IA64_UNIT_M, .address = BASE,
                    .opcode = (store_op ? IA64_OP_ST1 : IA64_OP_LD1) + width,
                    .imm_base_update = true,
                    .operands.memory = {.destination = 8, .source = 9, .base = 10, .immediate = 8}};
                uint8_t aligned_before[sizeof(alignment_backing)], device_before[sizeof(device_bytes)];
                memcpy(aligned_before, alignment_backing, sizeof(alignment_backing));
                memcpy(device_before, device_bytes, sizeof(device_bytes));
                equivalence(v, ref, &i);
                CHECK(!memcmp(aligned_before, alignment_backing, sizeof(alignment_backing)) &&
                    !memcmp(device_before, device_bytes, sizeof(device_bytes)));
            }
    const uint64_t addresses[] = {DATA + 128, BASE + 128, DATA + 4095,
        (UINT64_C(1) << (IA64_IMPL_PA_MSB + 1)) | 1, DEVICE, DEVICE + 4096,
        DEVICE + 16384, DEVICE + 24576, BASE + sizeof(ram) - 1, 0, (UINT64_C(1) << 63) | (DATA + 128)};
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_NONE; ++mode) {
        CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
        for (unsigned store_op = 0; store_op < 2; ++store_op)
            for (unsigned width = 0; width < 4; ++width)
                for (unsigned trial = 0; trial < 240; ++trial) {
                    randomized(v);
                    v->cfg.engine = trial & 2 ? ENGINE_LOCKSTEP : ENGINE_TIER0;
                    v->s.psr = IA64_PSR_IC;
                    unsigned base = trial & 1 ? 40 : 10, src = trial & 1 ? 41 : 9;
                    unsigned dst = trial & 1 ? 42 : 8;
                    /* Wrapping/rotating homes and increment aliases are
                     * architectural inputs, rather than encoder fixtures. */
                    if (trial % 7 == 0) dst = src;
                    if (trial % 13 == 0) dst = base;
                    if (trial % 17 == 0) base = 0;
                    if (trial % 19 == 0) { base = 127; v->s.cfm = 8; }
                    ia64_gr_write(&v->s, src, trial & 4 ? UINT64_MAX - 7 : 8, trial % 5 == 0);
                    ia64_gr_write(&v->s, base, addresses[trial % IA64VMM_ARRAY_SIZE(addresses)], trial % 11 == 0);
                    (void)ia64_physical_region(v, ia64_gr_read(&v->s, base) & ~(UINT64_C(1) << 63), 1u << width);
                    if (trial % 23 == 0) v->physical_region = NULL;
                    if (trial % 29 == 0) v->memory_hook = no_region_hook;
                    Ia64Instruction i = {.valid = true, .unit = IA64_UNIT_M, .address = BASE, .slot = 1,
                        .opcode = (store_op ? IA64_OP_ST1 : IA64_OP_LD1) + width,
                        .qp = trial % 4, .imm_base_update = trial % 3 == 1, .reg_base_update = trial % 3 == 2,
                        .operands.memory = {.destination = dst, .source = src, .base = base, .immediate = -8}};
                    static uint8_t original[sizeof(ram)], expected[sizeof(ram)];
                    memcpy(original, ram, sizeof(ram));
                    *ref = *v;
                    struct ia64_result wanted = ia64_execute(ref, &i); expect_advance(ref, &i, &wanted);
                    memcpy(expected, ram, sizeof(ram)); memcpy(ram, original, sizeof(ram));
                    struct ia64_result got = tier0_test_instruction(v, &i);
                    CHECK(got.kind == wanted.kind && got.fault == wanted.fault && got.exit == wanted.exit &&
                        (got.kind == IA64_STEP_COMPLETE || got.info == wanted.info));
                    CHECK(!memcmp(&v->s, &ref->s, sizeof(v->s)) && v->retired == ref->retired);
                    CHECK(!memcmp(expected, ram, sizeof(ram)));
                    v->memory_hook = NULL;
                }
    }
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
}
static void test_stacked_operand_use(struct vcpu *v)
{
    restart(v, ENGINE_TIER0, BASE);
    v->s.cfm = 96;
    Ia64Instruction i = {.valid = true, .opcode = IA64_OP_ADDS, .address = BASE, .slot = 1,
        .operands.integer = {.destination = 8, .source1 = 127, .source2 = 9, .immediate = 1}};
    struct tier0_region r = {.valid = true, .count = 1, .insns = &i, .slots = {1}};
    struct x86_buffer b = {.capacity = SIZE_MAX};
    tier0_emit(&b, &r, &v->s);
    CHECK(!b.failed && !r.gr_used[0] && !r.gr_used[1]);
    i.opcode = IA64_OP_MOVL; i.operands.integer.source2 = 126; r.slots[0] = 2;
    tier0_emit(&b, &r, &v->s);
    CHECK(!b.failed && !r.gr_used[0] && !r.gr_used[1]);
}
static void test_decoded_cache_conflicts(struct vcpu *v)
{
    const uint64_t a = BASE, b = BASE + 0x40000, c = BASE + 0x60000;
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_NONE; ++mode) {
        CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
        bundle(a, 1, nop, add(8,9,10), nop);
        bundle(b, 1, nop, add(11,12,13), nop);
        bundle(c, 1, nop, add(14,15,16), nop);
        restart(v, ENGINE_TIER0, BASE);
        const struct ia64_bundle *decoded;
        struct code_cache_stats before, after; code_cache_get_stats(&before);
        for (unsigned n = 0; n < 100; ++n) {
            CHECK(code_get_bundle(v, n & 1 ? b : a, &decoded).kind == IA64_STEP_COMPLETE);
            CHECK(decoded->insn[1].operands.integer.destination == (n & 1 ? 11 : 8));
        }
        code_cache_get_stats(&after);
        CHECK(after.decodes == before.decodes + 2 && after.hits == before.hits + 98);
        CHECK(code_page_has_code(a) && code_page_has_code(b));
        before = after;
        bundle(a, 1, nop, add(20,9,10), nop);
        CHECK(code_get_bundle(v, b, &decoded).kind == IA64_STEP_COMPLETE &&
            decoded->insn[1].operands.integer.destination == 11);
        CHECK(code_get_bundle(v, a, &decoded).kind == IA64_STEP_COMPLETE &&
            decoded->insn[1].operands.integer.destination == 20);
        code_cache_get_stats(&after);
        CHECK(after.decodes == before.decodes + 1 && after.hits == before.hits + 1);
        /* A third colliding page evicts the least recently used way.
         * Its old dependency must detach before reuse by the other page. */
        CHECK(code_get_bundle(v, c, &decoded).kind == IA64_STEP_COMPLETE);
        CHECK(code_get_bundle(v, a, &decoded).kind == IA64_STEP_COMPLETE);
        before = after; code_cache_get_stats(&after);
        CHECK(after.decodes == before.decodes + 1 && after.hits == before.hits + 1);
        code_notify_cpu_write(b, 16);
        CHECK(code_get_bundle(v, c, &decoded).kind == IA64_STEP_COMPLETE &&
            decoded->insn[1].operands.integer.destination == 14);
    }
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
}
static void test_x86_immediate_encoder(void)
{
    void *code = hal_alloc_pages(1, HAL_PAGE_EXEC); CHECK(code != NULL);
    const uint64_t values[] = {0,127,128,UINT32_MAX,UINT64_C(0x80000000),UINT64_C(0x100000000),
        (uint64_t)-128,(uint64_t)-129,(uint64_t)(int64_t)INT32_MIN,UINT64_MAX};
    struct x86_buffer b;
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(values); ++n) {
        b = (struct x86_buffer){.data = code, .capacity = 4096};
        x86_mov_imm(&b, X_RAX, values[n]); x86_u8(&b, 0xc3);
        CHECK(b.size == (values[n] <= UINT32_MAX ? 6 :
            values[n] == (uint64_t)(int64_t)(int32_t)values[n] ? 8 : 11));
        hal_code_set_executable(code, 1);
        CHECK(((uint64_t (*)(void))code)() == values[n]);
        hal_code_set_writable(code, 1);
        b = (struct x86_buffer){.data = code, .capacity = 4096};
        x86_movabs(&b, X_RAX, values[n]); x86_u8(&b, 0xc3);
        CHECK(!b.failed && b.size == 11);
        hal_code_set_executable(code, 1);
        CHECK(((uint64_t (*)(void))code)() == values[n]);
        hal_code_set_writable(code, 1);
        b = (struct x86_buffer){.data = code, .capacity = 4096};
        x86_call(&b, values[n]);
        CHECK(!b.failed && b.size == 13 && ((uint8_t *)code)[0] == 0x49 &&
            ((uint8_t *)code)[1] == 0xbb);
        uint64_t target; memcpy(&target, (uint8_t *)code + 2, 8);
        CHECK(target == values[n]);
    }
    const int32_t immediates[] = {-129,-128,-1,0,1,127,128,INT32_MIN,INT32_MAX};
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(immediates); ++n) {
        b = (struct x86_buffer){.data = code, .capacity = 4096};
        x86_mov(&b, X_RAX, X_RDI); x86_imm(&b, 0, X_RAX, immediates[n]); x86_u8(&b, 0xc3);
        hal_code_set_executable(code, 1);
        uint64_t arg = UINT64_C(0x1122334455667788);
        CHECK(((uint64_t (*)(uint64_t))code)(arg) == arg + (uint64_t)(int64_t)immediates[n]);
        hal_code_set_writable(code, 1);
    }
    uint8_t data[512]; uint8_t *base = data + 256;
    const int32_t disps[] = {-129,-128,-1,0,1,127,128};
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(disps); ++n) {
        uint64_t wanted = random64(); memcpy(base + disps[n], &wanted, 8);
        b = (struct x86_buffer){.data = code, .capacity = 4096};
        x86_push(&b, X_R13); x86_mov(&b, X_R13, X_RDI);
        x86_load(&b, X_RAX, X_R13, disps[n], 8); x86_pop(&b, X_R13); x86_u8(&b, 0xc3);
        hal_code_set_executable(code, 1);
        CHECK(((uint64_t (*)(void *))code)(base) == wanted);
        hal_code_set_writable(code, 1);
    }
    for (unsigned scale = 0; scale < 4; ++scale) {
        b = (struct x86_buffer){.data = code, .capacity = 4096};
        x86_lea(&b, X_RAX, X_RDI, X_RSI, scale); x86_u8(&b, 0xc3);
        hal_code_set_executable(code, 1);
        CHECK(((uint64_t (*)(uint64_t,uint64_t))code)(UINT64_MAX, UINT64_MAX) ==
            UINT64_MAX + (UINT64_MAX << scale));
        hal_code_set_writable(code, 1);
    }
    hal_free_pages(code, 1);
}

static void test_stat_variants(struct vcpu *v)
{
    CHECK(IA64VMM_TIER0_STATS);
    bundle(BASE, 0x11, nop, add(8,9,10), validator_branch(BASE, BASE));
    const uint64_t psrs[] = {0,IA64_PSR_IC,IA64_PSR_I,IA64_PSR_I | IA64_PSR_IC,IA64_PSR_MFL,IA64_PSR_MFH};
    restart(v, ENGINE_TIER0, BASE);
    for (unsigned n = 0; n < 16; ++n)
        for (unsigned p = 0; p < IA64VMM_ARRAY_SIZE(psrs); ++p) {
            v->s.ip = BASE; v->s.slot = 0; v->s.cfm = n; v->s.psr = psrs[p];
            validator_run(v, 3);
        }
    log_set_level(LOG_INFO); tier0_dump_stats();
    vcpu_destroy(v); code_cache_flush(); tier0_destroy();
}

static void test_store_to_code_bundle(struct vcpu *v)
{
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_NONE; ++mode) {
        CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
        bundle(BASE, 0x11, store(3,9,10) | 1, nop, validator_branch(BASE, BASE));
        bundle(BASE + 256, 0x11, nop, add(8,20,21), validator_branch(BASE + 256, BASE + 256));
        restart(v, ENGINE_TIER0, BASE); v->s.pr = 1;
        v->s.gr[20] = 7; v->s.gr[21] = 8;
        v->s.ip = BASE + 256; validator_run(v, 6); CHECK(v->s.gr[8] == 15);
        v->s.ip = BASE; validator_run(v, 6); /* Warm without writing (qp false). */
        v->s.gr[9] = 0x11 | (nop << 5) | (add(11,20,21) << 46);
        v->s.gr[10] = BASE + 256; v->s.pr |= 2;
        validator_run(v, 3);
        v->s.ip = BASE + 256; v->s.gr[8] = 99; v->s.gr[11] = 0;
        validator_run(v, 3);
        CHECK(v->s.gr[8] == 99 && v->s.gr[11] == 15);
    }
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
}
static uint64_t helper_exit_retired;
static uint64_t helper_exit_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    (void)opaque; (void)offset; CHECK(size == 8 && v->retired == helper_exit_retired);
    vcpu_request_exit(v, VCPU_EXIT_HALT); return 123;
}
static const struct gm_mmio_ops helper_exit_ops = {.read = helper_exit_read};
static void test_native_helper_exits(struct vcpu *v)
{
    code_cache_flush(); restart(v, ENGINE_TIER0, BASE);
    v->s.psr = IA64_PSR_IC;
    Ia64Instruction insns[3] = {
        {.opcode = IA64_OP_ADDS, .valid = true, .address = BASE, .slot = 0,
         .operands.integer = {.destination = 8, .source2 = 8, .immediate = 1}},
        {.opcode = IA64_OP_LD8, .valid = true, .address = BASE, .slot = 1,
         .operands.memory = {.destination = 9, .base = 10}},
        {.opcode = IA64_OP_NOP, .valid = true, .address = BASE, .slot = 2},
    };
    struct tier0_region *r = tier0_region_allocate(); CHECK(r != NULL);
    *r = (struct tier0_region){.valid = true, .count = 3, .slots = {1,1,1}};
    CHECK(tier0_stage(r, insns, &v->s)); tier0_seal(v, r->chunk);
    void (*fn)(struct vcpu *,uint64_t,struct tier0_execution *,const struct tier0_homes *) = r->entry;
    /* Enter the generated ABI directly so the outer checkpoint cannot
     * consume the request before the first native instruction retires. */
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC;
        v->retired = 17; v->s.gr[8] = 5; v->s.gr[10] = DEVICE + 28672;
        struct tier0_execution e = {.region = r, .stats = tier0_local_stats()};
        if (!scenario) vcpu_request_exit(v, VCPU_EXIT_HALT);
        if (scenario == 3) v->s.gr[10] = UINT64_C(1) << (IA64_IMPL_PA_MSB + 1);
        helper_exit_retired = 18;
        tier0_arena.executing = true;
        fn(v, scenario == 2 ? 1 : 3, &e, NULL);
        tier0_arena.executing = false;
        CHECK(v->s.gr[8] == 6 && v->last_successful_bundle == BASE);
        if (scenario < 2) {
            CHECK(e.stop && e.out.reason == VCPU_EXIT_HALT && !v->requested_exit);
            CHECK(v->retired == 18 + scenario && v->s.slot == 1 + scenario && v->s.ip == BASE);
        } else {
            CHECK(!e.stop && v->retired == 18 && v->s.ip == BASE && v->s.slot == 1);
            CHECK((scenario == 3) == e.fault);
            if (scenario == 3) CHECK(e.result.fault == IA64_FAULT_UNIMPL_DATA);
        }
    }
    tier0_region_retire(r, false); tier0_reclaim();
}
