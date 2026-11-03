/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real clone constructors with an attached native sentinel.
 * Include the checker to inspect its private shadow without a product API. */
#ifdef IA32_TEST_TIER1
#include "engine/tier1/checked.c"
static struct t1_check workspace;
struct t1_stats t1_stats;
#else
#include "engine/tier0/lockstep.c"
static struct tier0_check workspace;
#endif
/* Clone-only fixture: no SMP execution or diagnostic sink is linked. */
bool ia64_smp_active(void)
{
    return false;
}
void vmm_log(enum log_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
static bool refuse_allocation;
void *hal_alloc_pages(size_t pages, unsigned flags)
{
    (void)flags;
    return refuse_allocation || pages > (sizeof(workspace) + 4095) / 4096 ? NULL : &workspace;
}
const struct gm_region *gm_lookup(gpa_t gpa)
{
    (void)gpa;
    return NULL;
}
void code_notify_cpu_write(gpa_t gpa, size_t bytes)
{
    (void)gpa;
    (void)bytes;
}
int vmm_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    (void)buf;
    (void)size;
    (void)fmt;
    (void)ap;
    return 0;
}
int main(void)
{
    static struct vcpu v;
    static unsigned sentinel;
    v.native = (struct ia32_vcpu_private *)(void *)&sentinel;
    v.s.gr[8] = UINT64_C(0x1234567887654321);
#ifdef IA32_TEST_TIER1
    static struct t1_vcpu tv;
    static struct t1_region region;
    tv.v = &v;
    v.upper = &tv;
    refuse_allocation = true;
    if (t1_ck_begin(&tv, &region) || tv.check)
        return 1;
    refuse_allocation = false;
    for (unsigned n = 0; n < 2; ++n) {
        if (!t1_ck_begin(&tv, &region) || tv.check != &workspace)
            return 2;
        if (tv.check->shadow.native || tv.check->shadow.s.gr[8] != v.s.gr[8])
            return 3;
        /* Poison a reused workspace too; each new clone must clear it. */
        tv.check->shadow.native = v.native;
    }
#else
    refuse_allocation = true;
    if (tier0_check_begin(&v) || v.check_workspace)
        return 1;
    refuse_allocation = false;
    for (unsigned n = 0; n < 2; ++n) {
        struct tier0_check *c = tier0_check_begin(&v);
        if (c != &workspace)
            return 2;
        if (c->shadow.native || c->shadow.s.gr[8] != v.s.gr[8])
            return 3;
        tier0_check_destroy(c);
        c->shadow.native = v.native;
    }
#endif
    return v.native == (struct ia32_vcpu_private *)(void *)&sentinel ? 0 : 4;
}
