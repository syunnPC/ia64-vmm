/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/virtualization_hw.h"
#include "ia64vmm/bootinfo.h"
#include "ia64vmm/hal.h"

struct tag_lease {
    struct tag_lease *next;
    struct amd64_virtualization_tag tag;
    enum amd64_virtualization_user user;
    uint64_t root;
    bool pending, root_valid;
};
struct amd64_virtualization_cpu {
    unsigned cpu, refs[2];
    enum ia32_backend_kind backend;
    struct amd64_virtualization_hw hw;
    struct tag_lease *leases;
};
static struct {
    struct amd64_virtualization_cpu *owner;
    uint64_t generation;
} cpus[BOOT_MAX_CPUS];
_Static_assert(sizeof(struct amd64_virtualization_cpu) <= 4096, "CPU ownership page");
_Static_assert(sizeof(struct tag_lease) <= 4096, "tag lease page");

static bool fail(struct ia32_setup_error *error, const char *detail)
{
    if (error)
        *error = (struct ia32_setup_error){
            .reason = IA32_EXIT_ENTRY_FAILURE, .host_cpu = hal_cpu_index(), .detail = detail};
    return false;
}
static bool owned(const struct amd64_virtualization_cpu *c, enum amd64_virtualization_user user)
{
    unsigned cpu = hal_cpu_index();
    return (unsigned)user < 2 && cpu < BOOT_MAX_CPUS && c && cpus[cpu].owner == c && c->refs[user];
}
bool amd64_virtualization_acquire(unsigned cpu, enum ia32_backend_kind backend,
                                  enum amd64_virtualization_user user,
                                  struct amd64_virtualization_cpu **out,
                                  struct ia32_setup_error *error)
{
    *out = NULL;
    if (cpu != hal_cpu_index() || cpu >= BOOT_MAX_CPUS || (unsigned)user >= 2 ||
        (backend != IA32_BACKEND_SVM && backend != IA32_BACKEND_VMX))
        return fail(error, "virtualization owner CPU, backend or user mismatch");
    struct amd64_virtualization_cpu *c = cpus[cpu].owner;
    if (c) {
        if (c->backend != backend || c->refs[user] == UINT32_MAX)
            return fail(error, "incompatible virtualization owner");
    } else {
        c = hal_alloc_pages(1, HAL_PAGE_ZERO);
        if (!c)
            return fail(error, "virtualization owner allocation failed");
        c->cpu = cpu;
        c->backend = backend;
        const char *why;
        if (!amd64_virtualization_hw_enable(backend, &c->hw, &why)) {
            hal_free_pages(c, 1);
            return fail(error, why);
        }
        cpus[cpu].owner = c;
    }
    ++c->refs[user];
    *out = c;
    return true;
}
void amd64_virtualization_release(struct amd64_virtualization_cpu *c,
                                  enum amd64_virtualization_user user)
{
    if (!owned(c, user))
        return;
    /* A final user reference cannot outlive its private control structures. */
    if (c->refs[user] == 1)
        for (struct tag_lease *l = c->leases; l; l = l->next)
            if (l->user == user)
                return;
    --c->refs[user];
    if (c->refs[0] || c->refs[1])
        return;
    amd64_virtualization_hw_disable(c->backend, &c->hw);
    cpus[c->cpu].owner = NULL;
    hal_free_pages(c, 1);
}
static bool tag_used(const struct amd64_virtualization_cpu *c, uint32_t id)
{
    for (const struct tag_lease *l = c->leases; l; l = l->next)
        if (l->tag.id == id)
            return true;
    return false;
}
bool amd64_virtualization_tag_acquire(struct amd64_virtualization_cpu *c,
                                      enum amd64_virtualization_user user,
                                      struct amd64_virtualization_tag *out,
                                      struct ia32_setup_error *error)
{
    *out = (struct amd64_virtualization_tag){0};
    if (!owned(c, user))
        return fail(error, "tag owner CPU or user mismatch");
    uint32_t id = 0;
    if (c->backend == IA32_BACKEND_SVM) {
        id = user == AMD64_VIRT_VBIOS ? 1 : 2;
        while (id < c->hw.tags && tag_used(c, id)) {
            if (user == AMD64_VIRT_VBIOS)
                return fail(error, "VBIOS ASID already leased");
            ++id;
        }
        if (id >= c->hw.tags)
            return fail(error, "ASID leases exhausted");
    }
    /* Do not wrap the public lease identity and accept stale handles. */
    if (cpus[c->cpu].generation == UINT64_MAX)
        return fail(error, "tag generations exhausted");
    struct tag_lease *l = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (!l)
        return fail(error, "tag lease allocation failed");
    l->tag = (struct amd64_virtualization_tag){id, ++cpus[c->cpu].generation};
    l->user = user;
    l->pending = true;
    l->next = c->leases;
    c->leases = l;
    *out = l->tag;
    return true;
}
static struct tag_lease **find(struct amd64_virtualization_cpu *c,
                               enum amd64_virtualization_user user,
                               const struct amd64_virtualization_tag *tag)
{
    struct tag_lease **l = &c->leases;
    while (*l && ((*l)->tag.id != tag->id || (*l)->tag.generation != tag->generation ||
                  (*l)->user != user))
        l = &(*l)->next;
    return l;
}
void amd64_virtualization_tag_release(struct amd64_virtualization_cpu *c,
                                      enum amd64_virtualization_user user,
                                      const struct amd64_virtualization_tag *tag)
{
    if (!owned(c, user) || !tag)
        return;
    struct tag_lease **p = find(c, user, tag), *l = *p;
    if (!l)
        return;
    *p = l->next;
    hal_free_pages(l, 1);
}
bool amd64_virtualization_tag_prepare(struct amd64_virtualization_cpu *c,
                                      enum amd64_virtualization_user user,
                                      const struct amd64_virtualization_tag *tag, uint64_t eptp,
                                      uint8_t *svm_tlb_control)
{
    if (!owned(c, user) || !tag || !svm_tlb_control)
        return false;
    struct tag_lease *l = *find(c, user, tag);
    if (!l)
        return false;
    if (!l->root_valid || l->root != eptp) {
        l->root = eptp;
        l->root_valid = true;
        l->pending = true;
    }
    *svm_tlb_control = 0;
    if (!l->pending)
        return true;
    /* APM Vol.2 §15.5: SVM invalidation is consumed by the next valid VMRUN. */
    if (c->backend == IA32_BACKEND_SVM) {
        *svm_tlb_control = 1;
        return true;
    }
    if (!amd64_virtualization_hw_invalidate(&c->hw, tag->id, eptp))
        return false;
    l->pending = false;
    return true;
}
void amd64_virtualization_tag_entered(struct amd64_virtualization_cpu *c,
                                      enum amd64_virtualization_user user,
                                      const struct amd64_virtualization_tag *tag, bool entered)
{
    if (!owned(c, user) || !tag || !entered || c->backend != IA32_BACKEND_SVM)
        return;
    struct tag_lease *l = *find(c, user, tag);
    if (l)
        l->pending = false;
}
