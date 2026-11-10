/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native contract lifecycle with real ownership/leases and mocked VM entry.
 * AMD APM Vol.2 15.16/15.25: NPT rights and next-entry invalidation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/ia32_svm.h"
#include "host/amd64/virtualization_hw.h"
#include "ia64vmm/hal.h"
#include "tests/host/test.h"
uint64_t amd64_hhdm;
static unsigned cpu, allocations, attempts, fail_at, entries, enables, disables;
static bool locked, reject_entry, host_x87_inuse;
static uint8_t expected_flush = 1;
static uint64_t now = 100, timer;
static uint8_t *last_vmcb;
unsigned hal_cpu_index(void)
{
    return cpu;
}
uint64_t hal_counter(void)
{
    return now;
}
uint64_t hal_counter_hz(void)
{
    return 1000000;
}
void hal_timer_arm(uint64_t deadline)
{
    CHECK(locked); /* A host IRQ must not consume the bound before entry. */
    timer = deadline;
}
void hal_timer_cancel(void)
{
    timer = 0;
}
void *hal_alloc_pages(size_t pages, unsigned flags)
{
    (void)flags;
    if (++attempts == fail_at)
        return NULL;
    void *p = aligned_alloc(4096, pages * 4096);
    if (p) {
        memset(p, 0, pages * 4096);
        ++allocations;
    }
    return p;
}
void hal_free_pages(void *p, size_t pages)
{
    (void)pages;
    --allocations;
    free(p);
}
bool amd64_virtualization_hw_enable(enum ia32_backend_kind kind, struct amd64_virtualization_hw *h,
                                    const char **why)
{
    (void)kind;
    (void)why;
    h->page = hal_alloc_pages(1, 0);
    if (!h->page) {
        *why = "test hardware allocation";
        return false;
    }
    h->tags = 8;
    ++enables;
    return true;
}
void amd64_virtualization_hw_disable(enum ia32_backend_kind k, struct amd64_virtualization_hw *h)
{
    (void)k;
    hal_free_pages(h->page, 1);
    ++disables;
}
bool amd64_virtualization_hw_invalidate(const struct amd64_virtualization_hw *h, uint32_t tag,
                                        uint64_t root)
{
    (void)h;
    (void)tag;
    (void)root;
    CHECK(false);
    return false;
}
bool amd64_svm_ia32_requirements(struct amd64_svm_ia32_requirements *r, struct ia32_setup_error *e)
{
    (void)e;
    *r = (struct amd64_svm_ia32_requirements){.physical_bits = 52,
                                              .caps = {.backend = IA32_BACKEND_SVM,
                                                       .host_cpu = cpu,
                                                       .nested_paging = true,
                                                       .nested_invalidate = true,
                                                       .bounded_exit = true,
                                                       .legacy_fp = true,
                                                       .nrip = true,
                                                       .decode_assists = true},
                                              .pat_flags = {0, 0x18, 0x98}};
    return true;
}
bool amd64_ia32_fp_requirements(struct amd64_ia32_fp_requirements *r, struct ia32_setup_error *e)
{
    (void)e;
    *r = (struct amd64_ia32_fp_requirements){.bytes = 832,
                                             .alignment = 64,
                                             .xcr0 = 7,
                                             .xsave = true,
                                             .host_x87_initial_only = true,
                                             .xgetbv1 = true};
    return true;
}
uint64_t amd64_ia32_fp_host_inuse(void)
{
    return host_x87_inuse ? 1 : 0;
}
uint64_t amd64_svm_ia32_lock(void)
{
    CHECK(!locked);
    locked = true;
    return 512;
}
void amd64_svm_ia32_unlock(uint64_t flags)
{
    CHECK(locked && flags == 512);
    locked = false;
}
static uint64_t rd(const void *p, unsigned off)
{
    uint64_t x;
    memcpy(&x, (const uint8_t *)p + off, 8);
    return x;
}
static void wr(void *p, unsigned off, uint64_t x)
{
    memcpy((uint8_t *)p + off, &x, 8);
}
void amd64_svm_ia32_run(uint8_t *v, uint8_t *host, struct amd64_svm_ia32_gprs *g,
                        struct amd64_ia32_fp_owner *fp, struct ia32_native_state *state,
                        bool tsc_aux)
{
    (void)host;
    (void)fp;
    (void)tsc_aux;
    CHECK(locked && timer == 1000 && v[0x5c] == expected_flush);
    /* Fault induction must hold on every entry, including after host
     * exits without another import. MSR writes and #GP/#UD stay trapped. */
    CHECK(!rd(v, VMCB_SYSENTER_CS) && !rd(v, VMCB_SYSENTER_ESP) && !rd(v, VMCB_SYSENTER_EIP));
    CHECK((v[VMCB_INTERCEPT_EXC] & (1u << 6)) && (v[VMCB_INTERCEPT_EXC + 1] & (1u << 5)) &&
          (v[VMCB_INTERCEPT_MISC1 + 3] & (1u << 4)));
    ++entries;
    last_vmcb = v;
    bool step = (rd(v, VMCB_RFLAGS) & 0x100) != 0;
    wr(v, 0x70, reject_entry ? UINT64_MAX : step ? 0x41 : 0x72);
    if (!reject_entry) {
        if (step)
            wr(v, VMCB_DR6, rd(v, VMCB_DR6) | (1u << 14));
        wr(v, 0x5f8, 0x80000001);
        wr(v, 0x578, rd(v, 0x578) + 2);
        amd64_svm_ia32_get(v, g, state);
    }
    now += 7;
}
static uint64_t *leaf(uint64_t token)
{
    uint64_t *p = (void *)(uintptr_t)rd(last_vmcb, 0xb0);
    for (unsigned shift = 39; shift > 12; shift -= 9)
        p = (void *)(uintptr_t)(p[(token >> shift) & 511] & NPT_ADDR);
    return &p[(token >> 12) & 511];
}
static struct ia32_native_state good(void)
{
    struct ia32_native_state n = {
        .cpl = 3, .eflag = 0x202, .cflg = UINT64_C(1) << 41, .dr6 = 0xffff0ff0, .dr7 = 0x400};
    n.cs = (struct ia32_segment){.limit = 0xffffffff, .attrib = 0xcfb, .selector = 0x23};
    n.ss = n.es = n.ds = n.fs = n.gs =
        (struct ia32_segment){.limit = 0xffffffff, .attrib = 0xcf3, .selector = 0x2b};
    n.ldtr = (struct ia32_segment){.limit = 0xffff, .attrib = 0x82};
    n.tr = (struct ia32_segment){.limit = 0x67, .attrib = 0x8b};
    n.fp.bytes[0] = 0x7f;
    n.fp.bytes[1] = 3;
    n.fp.bytes[24] = 0x80;
    n.fp.bytes[25] = 0x1f;
    return n;
}
int main(void)
{
    const struct ia32_backend_ops *ops = &amd64_svm_ia32_ops;
    struct ia32_container *a, *b;
    struct ia32_setup_error error;
    struct ia32_capabilities caps;
    CHECK(ops->probe(0, &caps, &error) && caps.backend == IA32_BACKEND_SVM && !caps.mtf);
    CHECK(!ops->create(1, &a, &error) && !a && !allocations);
    for (unsigned at = 1; at < 20; ++at) {
        attempts = 0;
        fail_at = at;
        bool ok = ops->create(0, &a, &error);
        if (ok)
            ops->destroy(a);
        else
            CHECK(!a);
        CHECK(!allocations && enables == disables);
        if (ok)
            break;
    }
    fail_at = 0;
    CHECK(ops->create(0, &a, &error) && ops->create(0, &b, &error));
    struct ia32_native_state n = good(), output = n;
    CHECK(!ops->import(a, &n, IA32_DIRTY_GPR, &error));
    CHECK(ops->import(a, &n, IA32_DIRTY_ALL, &error));
    struct ia32_generations gen = {1, 2, 3, 4};
    CHECK(ops->invalidate(a, &gen, &error));
    struct ia32_mapping m = {.token = {0x12345000},
                             .hpa = 0x23456000,
                             .rights = IA32_MAP_READ | IA32_MAP_EXEC,
                             .memory_type = IA32_MEMORY_WB,
                             .generations = gen};
    CHECK(ops->map(a, &m, &error));
    struct ia32_run_limit limit = {.deadline = 1000};
    struct ia32_run_usage use;
    host_x87_inuse = true;
    struct ia32_native_state initial_output = output;
    struct ia32_native_exit refused = ops->enter(a, &output, &limit, &use);
    CHECK(refused.reason == IA32_EXIT_UNIMPLEMENTED && !refused.dirty && !entries &&
          !use.residence_ticks);
    CHECK(!memcmp(&initial_output, &output, sizeof(output)) && !locked && !timer);
    host_x87_inuse = false;
    struct ia32_native_exit e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_CPUID && entries == 1 && use.residence_ticks == 7 &&
          !use.instructions_valid);
    CHECK(output.gpr[IA32_EAX] == 0x80000001 && output.eip.value == 2 &&
          !memcmp(&e.generations, &gen, sizeof(gen)));
    CHECK(*leaf(m.token.value) == (m.hpa | 5));
    expected_flush = 0;
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_CPUID);
    for (unsigned rights = 0; rights < 8; ++rights) {
        m.rights = rights;
        uint64_t before = *leaf(m.token.value);
        bool ok = ops->map(a, &m, &error);
        CHECK(ok == ((rights & IA32_MAP_READ) != 0));
        if (!ok)
            CHECK(*leaf(m.token.value) == before);
    }
    m.rights = IA32_MAP_READ | IA32_MAP_WRITE;
    m.memory_type = IA32_MEMORY_WC;
    CHECK(ops->map(a, &m, &error));
    CHECK(*leaf(m.token.value) == (m.hpa | 0x9f | (UINT64_C(1) << 63)));
    cpu = 1;
    CHECK(!ops->map(a, &m, &error));
    cpu = 0;
    expected_flush = 1;
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_CPUID);
    expected_flush = 0;
    CHECK(ops->unmap(a, m.token, 4096, &error) && !*leaf(m.token.value));
    expected_flush = 1;
    reject_entry = true;
    struct ia32_native_state before = output;
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_ENTRY_FAILURE && !memcmp(&before, &output, sizeof(before)));
    reject_entry = false;
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_CPUID);
    unsigned prior = entries;
    before = output;
    limit.step = true;
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_UNIMPLEMENTED && entries == prior);
    limit.svm_step_safe = true;
    expected_flush = 0;
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_STEP && entries == prior + 1);
    CHECK(!(output.eflag & 0x100) && !(output.dr6 & (1u << 14)));
    CHECK(e.hardware_eflag & 0x100);
    CHECK(e.prior_eflag_valid && !(e.prior_eflag & 0x100));
    CHECK(e.phase == IA32_PHASE_UNKNOWN && !use.instructions_valid);
    ++prior;
    n = output;
    n.dr6 |= 1;
    CHECK(ops->import(a, &n, IA32_DIRTY_DEBUG, &error));
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_STEP && (output.dr6 & 1));
    ++prior;
    before = output;
    n = output;
    n.interrupt_shadow = 1;
    CHECK(ops->import(a, &n, IA32_DIRTY_SHADOW, &error));
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_UNIMPLEMENTED && entries == prior);
    n.interrupt_shadow = 0;
    CHECK(ops->import(a, &n, IA32_DIRTY_SHADOW, &error));
    limit.step = false;
    limit.instructions = 1;
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_UNIMPLEMENTED && entries == prior);
    limit.instructions = 0;
    limit.deadline = now;
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_DEADLINE && entries == prior && !use.residence_ticks);
    CHECK(!memcmp(&before, &output, sizeof(before)));
    CHECK(ops->reset(a, &error));
    limit.deadline = 1000;
    e = ops->enter(a, &output, &limit, &use);
    CHECK(e.reason == IA32_EXIT_UNIMPLEMENTED && entries == prior);
    ops->destroy(a);
    CHECK(allocations && enables > disables);
    ops->destroy(b);
    CHECK(!allocations && enables == disables && !locked && !timer);
    puts(
        "native SVM backend: lifecycle, exact mappings, verified private step, deadline/count stops and "
        "failed-entry flush passed");
    return 0;
}
