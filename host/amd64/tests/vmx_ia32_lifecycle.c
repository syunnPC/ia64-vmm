/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the native lifecycle with a VMCS/CPU transport fixture. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/ia32_native.h"
#include "host/amd64/vmx_ia32.h"
#include "ia64vmm/hal.h"

static unsigned failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x);                                \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
struct vmx_gprs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
};
struct amd64_virtualization_cpu {
    unsigned refs, leases;
    bool pending;
};
struct amd64_ia32_fp_owner {
    unsigned prepared;
};
uint64_t amd64_hhdm;
static struct amd64_virtualization_cpu owner;
static uint64_t vmcs[0x7000], ticks = 100, timer;
static unsigned allocations, attempts, fail_at, entries, flushes, clears, dispatches;
static bool irq_blocked, fp_restored, bad_clear;
static uint64_t tsc_aux = 0x12345678;
static int entry_status;
static uint32_t native_ecx;

static void amd64_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c,
                         uint32_t *d)
{
    CHECK(leaf == 1 && sub == 0);
    *a = *b = *d = 0;
    *c = native_ecx;
}

unsigned hal_cpu_index(void)
{
    return 0;
}
uint64_t hal_counter(void)
{
    return ticks++;
}
uint64_t hal_counter_hz(void)
{
    return 1000000;
}
void hal_timer_arm(uint64_t deadline)
{
    CHECK(irq_blocked);
    timer = deadline;
}
void hal_timer_cancel(void)
{
    timer = 0;
}
void *hal_alloc_pages(size_t pages, unsigned flags)
{
    if (++attempts == fail_at)
        return NULL;
    void *p = aligned_alloc(4096, pages * 4096);
    if (!p)
        abort();
    if (flags & HAL_PAGE_ZERO)
        memset(p, 0, pages * 4096);
    ++allocations;
    return p;
}
void hal_free_pages(void *p, size_t pages)
{
    (void)pages;
    CHECK(allocations);
    --allocations;
    free(p);
}
_Noreturn void hal_panic(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}
static uint64_t physical(const void *p)
{
    return (uintptr_t)p;
}
static bool vmx_clear(uint64_t pa)
{
    CHECK(pa);
    ++clears;
    return !bad_clear;
}
static bool vmx_ptrld(uint64_t pa)
{
    return pa != 0;
}
static bool vmx_write(uint64_t field, uint64_t value)
{
    CHECK(field < 0x7000);
    vmcs[field] = value;
    return true;
}
static uint64_t vmx_read(uint64_t field)
{
    CHECK(field < 0x7000);
    return vmcs[field];
}
static uint64_t vmread_fn(void *ctx, uint32_t field)
{
    (void)ctx;
    return vmx_read(field);
}
static bool write_fields(const struct vmx_field *f, unsigned n, uint32_t *failed)
{
    (void)failed;
    for (unsigned i = 0; i < n; ++i)
        vmx_write(f[i].encoding, f[i].value);
    return true;
}
static void host_state(struct vmx_host *host)
{
    memset(host, 0, sizeof(*host));
}
static bool probe(struct vmx_caps *c, const char **why)
{
    (void)why;
    memset(c, 0, sizeof(*c));
    c->revision = 1;
    c->maxphyaddr = 48;
    c->pin_msr = c->proc_msr = c->proc2_msr = c->exit_msr = c->entry_msr =
        UINT64_C(0xffffffff00000000);
    c->cr0_fixed0 = 0x80000021;
    c->cr0_fixed1 = UINT32_MAX;
    c->cr4_fixed0 = 0x2000;
    c->cr4_fixed1 = UINT32_MAX;
    c->ept_vpid = UINT64_C(1) << 25;
    return true;
}
bool amd64_virtualization_acquire(unsigned cpu, enum ia32_backend_kind backend,
                                  enum amd64_virtualization_user user,
                                  struct amd64_virtualization_cpu **out,
                                  struct ia32_setup_error *error)
{
    (void)error;
    CHECK(cpu == 0 && backend == IA32_BACKEND_VMX && user == AMD64_VIRT_IA32);
    ++owner.refs;
    *out = &owner;
    return true;
}
void amd64_virtualization_release(struct amd64_virtualization_cpu *c,
                                  enum amd64_virtualization_user u)
{
    CHECK(c == &owner && u == AMD64_VIRT_IA32 && owner.refs);
    --owner.refs;
}
bool amd64_virtualization_tag_acquire(struct amd64_virtualization_cpu *c,
                                      enum amd64_virtualization_user user,
                                      struct amd64_virtualization_tag *tag,
                                      struct ia32_setup_error *error)
{
    (void)error;
    CHECK(c == &owner && user == AMD64_VIRT_IA32);
    ++owner.leases;
    *tag = (struct amd64_virtualization_tag){0, owner.leases};
    owner.pending = true;
    return true;
}
void amd64_virtualization_tag_release(struct amd64_virtualization_cpu *c,
                                      enum amd64_virtualization_user user,
                                      const struct amd64_virtualization_tag *tag)
{
    CHECK(c == &owner && user == AMD64_VIRT_IA32);
    if (tag->generation) {
        CHECK(owner.leases);
        --owner.leases;
    }
}
bool amd64_virtualization_tag_invalidate(struct amd64_virtualization_cpu *c,
                                         enum amd64_virtualization_user user,
                                         const struct amd64_virtualization_tag *tag)
{
    CHECK(c == &owner && user == AMD64_VIRT_IA32 && tag->generation);
    owner.pending = true;
    return true;
}
bool amd64_virtualization_tag_prepare(struct amd64_virtualization_cpu *c,
                                      enum amd64_virtualization_user user,
                                      const struct amd64_virtualization_tag *tag, uint64_t eptp,
                                      uint8_t *tlb)
{
    CHECK(c == &owner && user == AMD64_VIRT_IA32 && tag->generation && eptp);
    *tlb = 0;
    if (owner.pending) {
        ++flushes;
        owner.pending = false;
    }
    return true;
}
bool amd64_ia32_fp_requirements(struct amd64_ia32_fp_requirements *r,
                                struct ia32_setup_error *error)
{
    (void)error;
    memset(r, 0, sizeof(*r));
    return true;
}
bool amd64_ia32_fp_create(struct amd64_ia32_fp_owner **out, struct ia32_setup_error *error)
{
    (void)error;
    *out = hal_alloc_pages(1, HAL_PAGE_ZERO);
    return *out != NULL;
}
void amd64_ia32_fp_destroy(struct amd64_ia32_fp_owner *p)
{
    hal_free_pages(p, 1);
}
bool amd64_ia32_fp_prepare(struct amd64_ia32_fp_owner *p, const struct ia32_fp_image *fp,
                           struct ia32_setup_error *error)
{
    (void)fp;
    (void)error;
    ++p->prepared;
    return true;
}
bool amd64_ia32_fp_host_admit(struct amd64_ia32_fp_owner *p, struct ia32_setup_error *error)
{
    (void)p;
    (void)error;
    CHECK(irq_blocked);
    return true;
}
void amd64_ia32_fp_complete(struct amd64_ia32_fp_owner *p, struct ia32_fp_image *fp)
{
    (void)p;
    (void)fp;
    CHECK(fp_restored || !entries);
}
static uint64_t vmx_native_irq_save(void)
{
    irq_blocked = true;
    return 0x202;
}
static void vmx_native_irq_restore(uint64_t flags, bool dispatch, bool nmi)
{
    CHECK(flags == 0x202);
    (void)nmi;
    if (dispatch) {
        CHECK(fp_restored);
        CHECK(tsc_aux == 0x12345678);
        ++dispatches;
    }
    irq_blocked = false;
}
static bool vmx_native_tsc_aux_supported(void)
{
    return true;
}
static uint64_t vmx_native_tsc_aux_enter(bool supported)
{
    CHECK(irq_blocked && supported);
    uint64_t saved = tsc_aux;
    tsc_aux = 0;
    return saved;
}
static void vmx_native_tsc_aux_leave(bool supported, uint64_t saved)
{
    CHECK(irq_blocked && supported);
    tsc_aux = saved;
}

#include "host/amd64/vmx_ia32.inc"

int amd64_vmx_ia32_vmrun(struct vmx_gprs *g, int launched, const struct amd64_xstate_pair *fp,
                         uint64_t *debug)
{
    (void)launched;
    CHECK(irq_blocked && timer && !owner.pending && fp->native == 1);
    CHECK(debug && fp->host && fp->guest);
    CHECK(tsc_aux == 0);
    ++entries;
    fp_restored = true;
    if (!entry_status) {
        g->rax = 0xdeadbeef;
        vmcs[0x681e] = 0x3456;
        vmcs[0x4402] = 10;
        vmcs[0x440c] = 2;
        vmcs[0x4824] = 2;
        vmcs[0x6822] = 0x4000;
    }
    return entry_status;
}
static struct ia32_native_state initial(void)
{
    struct ia32_native_state s = {0};
    s.cs = (struct ia32_segment){.limit = UINT32_MAX, .selector = 0x8, .attrib = 0xc9b};
    s.ss = s.es = s.ds = s.fs = s.gs =
        (struct ia32_segment){.limit = UINT32_MAX, .selector = 0x10, .attrib = 0xc93};
    s.ldtr.unusable = true;
    s.tr = (struct ia32_segment){.limit = 0x67, .selector = 0x18, .attrib = 0x8b};
    s.cflg = UINT64_C(1) << 41;
    s.eflag = 0x202;
    s.dr6 = 0xffff0ff0;
    s.dr7 = 0x400;
    s.cflg |= 0x8; /* CR0.TS must reach hardware unchanged. */
    s.kr0 = 0x12345678;
    s.kr1 = 0x9876;
    s.kr2 = 0x7777;
    return s;
}
int main(void)
{
    const struct ia32_backend_ops *ops = &amd64_vmx_ia32_ops;
    struct ia32_setup_error error;
    struct ia32_capabilities caps;
    CHECK(ops->probe(0, &caps, &error) && !caps.native_features);
    native_ecx = 1;
    CHECK(ops->probe(0, &caps, &error) && caps.native_features == IA32_NATIVE_SSE3);
    native_ecx = 1u << 23;
    CHECK(ops->probe(0, &caps, &error) && caps.native_features == IA32_NATIVE_POPCNT);
    native_ecx = UINT32_MAX;
    CHECK(ops->probe(0, &caps, &error) &&
          caps.native_features == (IA32_NATIVE_SSE3 | IA32_NATIVE_POPCNT));
    /* Every allocation failure must unwind its pages and CPU leases. */
    for (unsigned fail = 1; fail <= 6; ++fail) {
        attempts = 0;
        fail_at = fail;
        struct ia32_container *c = (void *)1;
        CHECK(!ops->create(0, &c, &error) && !c);
        CHECK(!allocations && !owner.refs && !owner.leases);
    }
    fail_at = 0;
    struct ia32_container *c = NULL;
    CHECK(ops->create(0, &c, &error) && c);
    struct ia32_native_state s = initial(), out;
    CHECK(!ops->export(c, &out, &error));
    CHECK(ops->import(c, &s, IA32_DIRTY_ALL, &error));
    CHECK(vmcs[0x6800] == 0x39 && vmcs[0x6804] == 0x2600);
    CHECK(vmcs[0x4004] == UINT32_MAX && !vmcs[0x4016]);
    CHECK(ops->export(c, &out, &error) && !memcmp(&s, &out, sizeof(s)));
    s.cpl = 3;
    CHECK(ops->import(c, &s, IA32_DIRTY_SEGMENTS, &error));
    CHECK(ops->export(c, &out, &error) && out.cpl == s.cpl);
    struct ia32_run_usage usage;
    struct ia32_run_limit limit = {.deadline = 1000, .instructions = 1};
    struct ia32_native_exit e = ops->enter(c, &out, &limit, &usage);
    CHECK(e.reason == IA32_EXIT_UNIMPLEMENTED && !entries && !usage.instructions_valid);
    limit.instructions = 0;
    limit.step = true;
    struct ia32_mapping map = {.token = {0x1000}, .hpa = 0x1000, .rights = 7};
    CHECK(ops->map(c, &map, &error) && owner.pending);
    e = ops->enter(c, &out, &limit, &usage);
    CHECK(e.reason == IA32_EXIT_CPUID && entries == 1 && flushes == 1);
    CHECK((vmcs[0x4002] & (1u << 27)) && out.gpr[0] == 0xdeadbeef && out.eip.value == 0x3456);
    CHECK(out.interruptibility == 2 && out.pending_debug == 0x4000 && out.kr0 == 0x12345678);
    CHECK(!usage.instructions_valid && !usage.instructions && dispatches == 1 && !timer &&
          !irq_blocked);
    CHECK(ops->unmap(c, map.token, 4096, &error) && owner.pending);
    entry_status = 1;
    limit.step = false;
    e = ops->enter(c, &out, &limit, &usage);
    CHECK(e.reason == IA32_EXIT_ENTRY_FAILURE && entries == 2 && flushes == 2);
    CHECK(!(vmcs[0x4002] & (1u << 27)) && out.gpr[0] == 0xdeadbeef);
    CHECK(ops->reset(c, &error) && owner.pending && owner.leases == 1);
    CHECK(!ops->export(c, &out, &error));
    ops->destroy(c);
    CHECK(!allocations && !owner.refs && !owner.leases && clears >= 3);
    if (failures)
        return 1;
    puts(
        "test-vmx-ia32-lifecycle: unwind, import/export, bounded entry, invalidation and reset passed");
    return 0;
}
