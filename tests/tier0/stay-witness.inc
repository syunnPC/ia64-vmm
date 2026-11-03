/* SPDX-License-Identifier: GPL-2.0-or-later */
/* An admission witness never begins a stay or certifies an unexamined
 * bundle. Every event that ends an existing stay must reject its witness. */
static void witness_zero(const void *object, size_t size)
{
    const unsigned char *bytes = object;
    for (size_t i = 0; i < size; ++i)
        CHECK(!bytes[i]);
}

static void witness_fields(const struct vcpu *v, const struct code_dependency *d,
                           const struct code_fetch *fetch, const struct code_stay_witness *w)
{
    CHECK(w->owner == v && w->page_word == &d->page && w->page == d->page);
    CHECK(w->ready_word && *w->ready_word && w->epoch_word && *w->epoch_word == w->epoch);
    CHECK(w->generation_word && *w->generation_word == w->generation);
    CHECK(w->admission_word && w->admission && *w->admission_word == w->admission);
    CHECK(w->token == v->code_entry_generation && w->iseq == v->mmu_iseq);
    CHECK(!((w->fetch.psr ^ fetch->psr) & ~CODE_PSR_RUNTIME) &&
          w->fetch.rr == fetch->rr && w->fetch.pta == fetch->pta && w->fetch.pte == fetch->pte);
    CHECK(w->clock && w->entered && *w->entered && *w->entered <= *w->clock);
    CHECK(w->entry_tick);
}

static bool witness_capture(const struct vcpu *v, const struct code_dependency *d,
                            const struct code_fetch *fetch, struct code_stay_witness *out)
{
    memset(out, 0xa5, sizeof(*out));
    bool accepted = code_stay_witness_take(v, d, fetch, out);
    if (accepted)
        witness_fields(v, d, fetch, out);
    else
        witness_zero(out, sizeof(*out));
    return accepted;
}

static bool witness_group_capture(const struct vcpu *v, const struct code_dependency *deps,
                                  unsigned n, const struct code_fetch *fetch,
                                  struct code_stay_witness_group *out)
{
    memset(out, 0xa5, sizeof(*out));
    bool accepted = code_stay_witness_group_take(v, deps, n, fetch, out);
    if (accepted) {
        CHECK(out->n == n);
        for (unsigned i = 0; i < n; ++i)
            witness_fields(v, &deps[i], fetch, &out->page[i]);
        for (unsigned i = n; i < CODE_STAY_WITNESS_PAGES; ++i)
            witness_zero(&out->page[i], sizeof(out->page[i]));
    } else
        witness_zero(out, sizeof(*out));
    return accepted;
}

static unsigned witness_retired;
static void witness_retire(struct code_dependency *d)
{
    (void)d;
    ++witness_retired;
}

static void witness_dependency(struct vcpu *v, struct code_dependency *d, gpa_t page,
                               uint64_t mask)
{
    uint64_t low, high, version = 0;
    for (unsigned bit = 0; bit < 64; ++bit)
        if ((mask >> bit) & 1)
            CHECK(code_source_read(v, page + 16 * bit, &low, &high, &version).kind == IA64_STEP_COMPLETE);
    *d = (struct code_dependency){.invalidate = witness_retire, .mask = {mask}};
    CHECK(code_dependency_publish(v, page, version, d));
    code_dependency_attach(page, d);
}

static struct {
    struct vcpu *v;
    struct code_dependency *deps;
    struct code_fetch fetch;
    enum code_test_stage stage;
    gpa_t captured, changed;
    unsigned event;
    bool armed;
} witness_race;

static void witness_capture_event(gpa_t page, enum code_test_stage stage)
{
    if (!witness_race.armed || stage != witness_race.stage || page != witness_race.captured)
        return;
    witness_race.armed = false;
    struct vcpu *v = witness_race.v;
    switch (witness_race.event) {
    case 0:
        code_notify_cpu_write(witness_race.changed + 32, 16);
        break;
    case 1:
        code_epoch_bump(CODE_DEVICE_STORAGE);
        break;
    case 2:
        ia64_mmu_changed(v, IA64_MMU_TC, 0, 0);
        break;
    case 3: {
        static struct vcpu other;
        other = *v;
        CHECK(code_dependencies_enter(&other, witness_race.deps, 2, &witness_race.fetch));
        break;
    }
    case 4:
        code_dependency_remove(&witness_race.deps[(witness_race.changed - BASE) / 4096]);
        break;
    case 5:
        v->code_entry_generation = 0;
        break;
    case 6:
        code_validity_flush();
        break;
    }
}

static void test_witness_capture_events(struct vcpu *v)
{
    unsigned cases = 0;
    for (enum code_validity_mode selected = CODE_VALIDITY_COMPARE_ALWAYS;
         selected <= CODE_VALIDITY_EPOCH_COMPARE; ++selected)
        for (unsigned phase = 0; phase < 6; ++phase)
            for (unsigned event = 0; event < 7; ++event) {
                CHECK(code_validity_configure(selected, CODE_SNAPSHOT_DEFAULT_BYTES));
                restart(v, ENGINE_TIER0, BASE);
                struct code_dependency deps[2] = {0};
                for (unsigned i = 0; i < 2; ++i) {
                    gpa_t page = BASE + i * 4096;
                    bundle(page, 0x11, nop, nop, validator_branch(page, page));
                    witness_dependency(v, &deps[i], page, 1);
                }
                const struct code_fetch fetch = {
                    .psr = IA64_PSR_IT, .rr = 1, .pta = 2, .pte = 0x100661};
                code_register_vcpu(v);
                CHECK(code_dependencies_enter(v, deps, 2, &fetch));
                witness_race.v = v;
                witness_race.deps = deps;
                witness_race.fetch = fetch;
                witness_race.stage = phase < 4 ? CODE_TEST_WITNESS : CODE_TEST_WITNESS_GROUP;
                witness_race.captured = BASE + (phase == 2 || phase == 3 ? 4096 : 0);
                witness_race.changed = BASE + (phase == 2 || phase == 4 ? 4096 : 0);
                witness_race.event = event;
                witness_race.armed = true;
                code_validity_set_test_hook(witness_capture_event);
                if (!phase) {
                    struct code_stay_witness witness;
                    CHECK(!witness_capture(v, deps, &fetch, &witness));
                } else {
                    struct code_stay_witness_group group;
                    CHECK(!witness_group_capture(v, deps, 2, &fetch, &group));
                }
                code_validity_set_test_hook(NULL);
                CHECK(!witness_race.armed);
                for (unsigned i = 0; i < 2; ++i)
                    code_dependency_remove(&deps[i]);
                code_unregister_vcpu(v);
                ++cases;
            }
    printf("code witness capture: %u single/member/group event races and complete failure clearing\n",
           cases);
}

static void witness_group_recycled(struct vcpu *v, enum code_validity_mode selected)
{
    CHECK(code_validity_configure(selected, 3 * 4096));
    restart(v, ENGINE_TIER0, BASE);
    for (unsigned i = 0; i < 3; ++i) {
        gpa_t page = BASE + i * 4096;
        bundle(page, 0x11, nop, nop, validator_branch(page, page));
    }
    struct code_dependency d[3] = {0};
    witness_dependency(v, &d[0], BASE, 1);
    witness_dependency(v, &d[1], BASE + 4096, 1);
    const struct code_fetch fetch = {.psr = IA64_PSR_IC};
    code_register_vcpu(v);
    CHECK(code_dependencies_enter(v, d, 2, &fetch));
    struct code_stay_witness_group old, renewed;
    CHECK(witness_group_capture(v, d, 2, &fetch, &old));
    uint64_t low, high, version;
    struct ia64_result result = code_source_read(v, BASE + 8192, &low, &high, &version);
    CHECK(result.kind == IA64_STEP_EXIT && result.exit == VCPU_EXIT_BUDGET);
    CHECK(!code_stay_witness_group_reenter(v, &old, &fetch));
    code_quiescent(v);
    witness_dependency(v, &d[2], BASE + 8192, 1);
    unsigned reused = d[2].page == old.page[0].page ? 0 : 1;
    CHECK(d[2].page == old.page[reused].page && !d[reused].page);
    witness_dependency(v, &d[reused], BASE + 8192, 1);
    CHECK(d[reused].page == old.page[reused].page &&
          __atomic_load_n(old.page[reused].generation_word, __ATOMIC_ACQUIRE) !=
              old.page[reused].generation);
    CHECK(!code_stay_witness_group_reenter(v, &old, &fetch));
    CHECK(code_dependencies_enter(v, d, 2, &fetch));
    CHECK(witness_group_capture(v, d, 2, &fetch, &renewed));
    CHECK(!code_stay_witness_group_reenter(v, &old, &fetch));
    CHECK(code_stay_witness_group_reenter(v, &renewed, &fetch));
    for (unsigned i = 0; i < 3; ++i)
        code_dependency_remove(&d[i]);
    code_unregister_vcpu(v);
}

/* A group certifies only comparisons already made in one stay. Its second
 * page need not have been an entry page, but its whole mask must be covered.
 * Reentry records both dependencies, in order, even when they alias. */
static void test_stay_witness_groups(struct vcpu *v)
{
    static struct vcpu other;
    unsigned cases = 0;
    const unsigned policies = CODE_VALIDITY_EPOCH_COMPARE - CODE_VALIDITY_COMPARE_ALWAYS + 1;
    for (unsigned variant = 0; variant < 2 * policies; ++variant) {
        enum code_validity_mode selected = CODE_VALIDITY_COMPARE_ALWAYS + variant / 2;
        bool virt = variant & 1;
        const struct code_fetch fetch = {
            .psr = IA64_PSR_IC | (virt ? IA64_PSR_IT : 0),
            .rr = virt ? 0x10130 : 0, .pta = virt ? 60 : 0, .pte = virt ? 0x100661 : 0};
        for (unsigned event = 0; event < 11; ++event) {
            CHECK(code_validity_configure(selected, CODE_SNAPSHOT_DEFAULT_BYTES));
            restart(v, ENGINE_TIER0, BASE);
            struct code_dependency d[6] = {0}, partial[2] = {0};
            for (unsigned page = 0; page < 6; ++page) {
                gpa_t address = BASE + 4096 * page;
                bundle(address, 0x11, nop, nop, validator_branch(address, address));
                bundle(address + 16, 0x11, nop, nop, validator_branch(address + 16, address));
                witness_dependency(v, &d[page], address, 1);
            }
            witness_dependency(v, &partial[0], BASE, 1);
            witness_dependency(v, &partial[1], BASE + 4096, 2);
            struct code_stay_witness_group group, rejected;
            CHECK(!witness_group_capture(v, d, 2, &fetch, &rejected));
            code_register_vcpu(v);
            CHECK(!witness_group_capture(v, d, 2, &fetch, &rejected));
            CHECK(!rejected.n && !rejected.page[0].owner && !rejected.page[1].owner);
            CHECK(!code_stay_witness_group_reenter(v, &rejected, &fetch));
            CHECK(code_dependencies_enter(v, d, 2, &fetch));
            struct code_stay_witness single;
            CHECK(witness_capture(v, &d[0], &fetch, &single));
            CHECK(!witness_capture(v, &d[1], &fetch, &single));
            struct code_validity_stats before, after;
            struct code_stay_stats sbefore, safter;
            code_validity_get_stats(&before);
            code_stay_get_stats(&sbefore);
            CHECK(witness_group_capture(v, d, 2, &fetch, &group));
            CHECK(group.n == 2 && group.page[0].clock == group.page[1].clock);
            CHECK(group.page[0].owner == v && group.page[1].owner == v &&
                  group.page[0].token == group.page[1].token &&
                  group.page[0].iseq == group.page[1].iseq &&
                  group.page[0].epoch == group.page[1].epoch);
            uint64_t clock_before = *group.page[0].clock;
            CHECK(!witness_group_capture(v, partial, 2, &fetch, &rejected));
            CHECK(!rejected.n && !rejected.page[0].owner && !rejected.page[1].owner);
            CHECK(!witness_group_capture(v, d, 0, &fetch, &rejected));
            CHECK(!witness_group_capture(v, d, 3, &fetch, &rejected));
            CHECK(!witness_group_capture(v, d, 2, NULL, &rejected));
            struct code_fetch translated = fetch;
            translated.psr ^= IA64_PSR_IT;
            CHECK(!witness_group_capture(v, d, 2, &translated, &rejected));
            CHECK(!code_stay_witness_group_reenter(v, &group, &translated));
            for (unsigned field = 0; field < 4; ++field) {
                struct code_fetch changed = fetch;
                if (field == 0) changed.psr ^= IA64_PSR_IC;
                if (field == 1) ++changed.rr;
                if (field == 2) ++changed.pta;
                if (field == 3) ++changed.pte;
                CHECK(!witness_group_capture(v, d, 2, &changed, &rejected));
                CHECK(!code_stay_witness_group_reenter(v, &group, &changed));
            }
            CHECK(*group.page[0].clock == clock_before);
            code_validity_get_stats(&after);
            code_stay_get_stats(&safter);
            CHECK(after.comparisons == before.comparisons &&
                  safter.page == sbefore.page && safter.reset == sbefore.reset);
            for (unsigned i = 0; i < 2; ++i)
                __atomic_store_n(group.page[i].entry_tick, 0, __ATOMIC_RELAXED);
            CHECK(code_stay_witness_group_reenter(v, &group, &fetch));
            CHECK(*group.page[0].clock == clock_before + 2 &&
                  *group.page[0].entered == clock_before + 1 &&
                  *group.page[1].entered == clock_before + 2);
            CHECK(code_dependencies_enter(v, d, 2, &fetch));
            CHECK(*group.page[0].entered == clock_before + 3 &&
                  *group.page[1].entered == clock_before + 4);
            for (unsigned i = 0; i < 2; ++i)
                CHECK(__atomic_load_n(group.page[i].entry_tick, __ATOMIC_RELAXED) == 1);
            code_quiescent(v);
            CHECK(code_stay_witness_group_reenter(v, &group, &fetch));
            other = *v;
            CHECK(!code_stay_witness_group_reenter(&other, &group, &fetch));
            struct code_fetch runtime = fetch;
            runtime.psr ^= IA64_PSR_I;
            CHECK(code_stay_witness_group_reenter(v, &group, &runtime));
            witness_retired = 0;
            switch (event) {
            case 0:
                code_notify_cpu_write(BASE + 4096 + 32, 16);
                CHECK(d[1].page && !witness_retired);
                break;
            case 1: {
                bool acquired = code_vmm_write_begin(BASE + 4096, 16);
                dma_bundle(BASE + 4096, 0x11, nop, add(8, 9, 0),
                           validator_branch(BASE + 4096, BASE + 4096));
                code_cpu_write_end(acquired);
                CHECK(!d[1].page && witness_retired == 1);
                break;
            }
            case 2:
                for (unsigned index = 2; index < 6; ++index) {
                    CHECK(code_dependency_enter(v, &d[0], &fetch));
                    CHECK(code_dependency_enter(v, &d[index], &fetch));
                }
                CHECK(witness_capture(v, &d[0], &fetch, &single));
                break;
            case 3:
                code_epoch_bump(CODE_DEVICE_STORAGE);
                break;
            case 4:
                code_dependency_remove(&d[1]);
                break;
            case 5:
                code_validity_flush();
                code_quiescent(v);
                break;
            case 6:
                code_dependency_remove(&d[1]);
                code_quiescent(v);
                witness_dependency(v, &d[1], BASE + 4096, 1);
                CHECK(code_dependencies_enter(v, d, 2, &fetch));
                break;
            case 7:
                v->code_entry_generation = 0;
                break;
            case 8:
                ia64_mmu_changed(v, IA64_MMU_TC, 0, 0);
                break;
            case 9:
                CHECK(code_dependencies_enter(&other, d, 2, &fetch));
                break;
            case 10:
                dma_bundle(BASE + 4096, 0x11, nop, add(8, 9, 0),
                           validator_branch(BASE + 4096, BASE + 4096));
                CHECK(code_stay_witness_group_reenter(v, &group, &fetch));
                CHECK(code_dependencies_enter(v, d, 2, &fetch));
                code_epoch_bump(CODE_DEVICE_STORAGE);
                break;
            }
            clock_before = *group.page[0].clock;
            uint64_t entered[2] = {*group.page[0].entered, *group.page[1].entered};
            for (unsigned i = 0; i < 2; ++i)
                __atomic_store_n(group.page[i].entry_tick, 0, __ATOMIC_RELAXED);
            CHECK(!code_stay_witness_group_reenter(v, &group, &fetch));
            CHECK(*group.page[0].clock == clock_before &&
                  *group.page[0].entered == entered[0] && *group.page[1].entered == entered[1]);
            for (unsigned i = 0; i < 2; ++i)
                CHECK(!__atomic_load_n(group.page[i].entry_tick, __ATOMIC_RELAXED));
            if (event != 6) {
                CHECK(!witness_group_capture(v, d, 2, &fetch, &rejected));
                CHECK(!rejected.n && !rejected.page[0].owner && !rejected.page[1].owner);
            } else
                CHECK(witness_group_capture(v, d, 2, &fetch, &rejected));
            if (event == 10)
                CHECK(!code_dependencies_enter(v, d, 2, &fetch) && witness_retired == 1);
            for (unsigned index = 0; index < 6; ++index)
                code_dependency_remove(&d[index]);
            for (unsigned index = 0; index < 2; ++index)
                code_dependency_remove(&partial[index]);
            code_unregister_vcpu(v);
            ++cases;
        }
        CHECK(code_validity_configure(selected, CODE_SNAPSHOT_DEFAULT_BYTES));
        restart(v, ENGINE_TIER0, BASE);
        bundle(BASE, 0x11, nop, nop, validator_branch(BASE, BASE));
        bundle(BASE + 16, 0x11, nop, nop, validator_branch(BASE + 16, BASE));
        struct code_dependency alias[2];
        witness_dependency(v, &alias[0], BASE, 1);
        witness_dependency(v, &alias[1], BASE, 2);
        code_register_vcpu(v);
        CHECK(code_dependency_enter(v, &alias[0], &fetch));
        CHECK(code_dependency_enter(v, &alias[1], &fetch));
        struct code_stay_witness_group group;
        CHECK(witness_group_capture(v, alias, 2, &fetch, &group));
        CHECK(group.page[0].entered == group.page[1].entered);
        uint64_t clock_before = *group.page[0].clock;
        CHECK(code_stay_witness_group_reenter(v, &group, &fetch));
        CHECK(*group.page[0].clock == clock_before + 2 &&
              *group.page[0].entered == clock_before + 2);
        CHECK(code_dependencies_enter(v, alias, 2, &fetch));
        CHECK(*group.page[0].clock == clock_before + 4 &&
              *group.page[0].entered == clock_before + 4);
        CHECK(witness_group_capture(v, alias, 1, &fetch, &group));
        CHECK(code_stay_witness_group_reenter(v, &group, &fetch));
        CHECK(*group.page[0].clock == clock_before + 5);
        code_dependency_remove(&alias[0]);
        code_dependency_remove(&alias[1]);
        code_unregister_vcpu(v);
        witness_group_recycled(v, selected);
    }
    printf("code stay witness groups: %u two-page physical/virtual policy/event cases, mask/fetch, recency, aliases and quiescence\n",
           cases);
}

static void test_stay_witnesses(struct vcpu *v)
{
    static struct vcpu other;
    unsigned cases = 0;
    for (enum code_validity_mode selected = CODE_VALIDITY_COMPARE_ALWAYS;
         selected <= CODE_VALIDITY_EPOCH_COMPARE; ++selected)
        for (unsigned event = 0; event < 11; ++event) {
            CHECK(code_validity_configure(selected, CODE_SNAPSHOT_DEFAULT_BYTES));
            restart(v, ENGINE_TIER0, BASE);
            struct code_dependency d[6] = {0};
            uint64_t low, high, version;
            for (unsigned page = 0; page < 5; ++page) {
                gpa_t address = BASE + 4096 * page;
                bundle(address, 0x11, nop, nop, validator_branch(address, address));
                bundle(address + 16, 0x11, nop, nop, validator_branch(address + 16, address));
                CHECK(code_source_read(v, address, &low, &high, &version).kind == IA64_STEP_COMPLETE);
                unsigned index = page ? page + 1 : 0;
                d[index] = (struct code_dependency){.invalidate = witness_retire, .mask = {1}};
                CHECK(code_dependency_publish(v, address, version, &d[index]));
                code_dependency_attach(address, &d[index]);
                if (!page) {
                    CHECK(code_source_read(v, address + 16, &low, &high, &version).kind == IA64_STEP_COMPLETE);
                    d[1] = (struct code_dependency){.invalidate = witness_retire, .mask = {2}};
                    CHECK(code_dependency_publish(v, address + 16, version, &d[1]));
                    code_dependency_attach(address, &d[1]);
                }
            }
            const struct code_fetch fetch = {
                .psr = IA64_PSR_IT, .rr = 1, .pta = 2, .pte = 0x100661
            };
            struct code_stay_witness witness, rejected;
            CHECK(!witness_capture(v, &d[0], &fetch, &rejected));
            code_register_vcpu(v);
            CHECK(!witness_capture(v, &d[0], NULL, &rejected));
            CHECK(!witness_capture(v, &d[0], &fetch, &rejected));
            CHECK(!rejected.owner);
            CHECK(!code_stay_witness_reenter(v, &rejected, &fetch));
            CHECK(code_dependency_enter(v, &d[0], &fetch));
            CHECK(witness_capture(v, &d[0], &fetch, &witness));
            code_unregister_vcpu(v);
            CHECK(!witness_capture(v, &d[0], &fetch, &rejected));
            code_register_vcpu(v);
            CHECK(witness_capture(v, &d[0], &fetch, &witness));
            CHECK(!witness_capture(v, &d[1], &fetch, &rejected));
            struct code_validity_stats before, after;
            code_validity_get_stats(&before);
            uint64_t clock_before = *witness.clock;
            CHECK(code_stay_witness_reenter(v, &witness, &fetch));
            CHECK(*witness.clock == clock_before + 1 && *witness.entered == *witness.clock &&
                  __atomic_load_n(witness.entry_tick, __ATOMIC_RELAXED) == 1);
            CHECK(code_dependency_enter(v, &d[0], &fetch));
            code_validity_get_stats(&after);
            CHECK(after.comparisons == before.comparisons);
            other = *v;
            CHECK(!code_stay_witness_reenter(&other, &witness, &fetch));
            struct code_fetch runtime = fetch;
            runtime.psr ^= IA64_PSR_I;
            CHECK(code_stay_witness_reenter(v, &witness, &runtime));
            for (unsigned field = 0; field < 4; ++field) {
                struct code_fetch changed = fetch;
                if (field == 0) changed.psr ^= IA64_PSR_IT;
                if (field == 1) ++changed.rr;
                if (field == 2) ++changed.pta;
                if (field == 3) changed.pte ^= 2;
                CHECK(!witness_capture(v, &d[0], &changed, &rejected));
                CHECK(!code_stay_witness_reenter(v, &witness, &changed));
            }
            witness_retired = 0;
            switch (event) {
            case 0:
                v->code_entry_generation = 0;
                break;
            case 1:
                ia64_mmu_changed(v, IA64_MMU_TC, 0, 0);
                break;
            case 2:
                code_epoch_bump(CODE_DEVICE_STORAGE);
                break;
            case 3:
                code_notify_cpu_write(BASE + 32, 16);
                CHECK(d[0].page && !witness_retired);
                break;
            case 4:
                code_notify_vmm_write(BASE, 16);
                CHECK(!d[0].page && witness_retired == 1);
                break;
            case 5:
                code_dependency_remove(&d[0]);
                break;
            case 6: {
                struct code_fetch remapped = fetch;
                remapped.pte ^= 2;
                CHECK(code_dependency_enter(v, &d[0], &remapped));
                break;
            }
            case 7:
                for (unsigned index = 2; index < 6; ++index)
                    CHECK(code_dependency_enter(v, &d[index], &fetch));
                break;
            case 8:
                CHECK(code_dependency_enter(&other, &d[0], &fetch));
                break;
            case 9:
                /* Registered executors retain the metadata words after flush. */
                code_validity_flush();
                break;
            case 10:
                /* Unannounced DMA within a stay follows the existing policy;
                 * ending that stay must expose the changed source bytes. */
                dma_bundle(BASE, 0x11, nop, add(8, 9, 0), validator_branch(BASE, BASE));
                CHECK(code_stay_witness_reenter(v, &witness, &fetch));
                CHECK(code_dependency_enter(v, &d[0], &fetch));
                code_epoch_bump(CODE_DEVICE_STORAGE);
                break;
            }
            CHECK(!code_stay_witness_reenter(v, &witness, &fetch));
            CHECK(!witness_capture(v, &d[0], &fetch, &rejected));
            CHECK(!rejected.owner);
            if (event == 10)
                CHECK(!code_dependency_enter(v, &d[0], &fetch) && witness_retired == 1);
            for (unsigned index = 0; index < 6; ++index)
                code_dependency_remove(&d[index]);
            code_unregister_vcpu(v);
            ++cases;
        }
    test_stay_witness_groups(v);
    test_witness_capture_events(v);
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    printf("code stay witness: %u policy/event cases, fetch/mask admission, owner, epoch, MMU, writes, retirement and DMA\n",
           cases);
}
