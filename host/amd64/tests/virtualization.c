/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <stdio.h>
#include "host/amd64/virtualization_hw.h"
#include "ia64vmm/hal.h"
#include "tests/host/test.h"

static unsigned cpu, allocations, attempts, fail_at, enables, disables, invalidations;
static bool hw_fail, invalidate_fail;
unsigned hal_cpu_index(void)
{
    return cpu;
}
void *hal_alloc_pages(size_t n, unsigned flags)
{
    (void)flags;
    if (++attempts == fail_at)
        return NULL;
    void *p = calloc(n, 4096);
    if (p)
        ++allocations;
    return p;
}
void hal_free_pages(void *p, size_t n)
{
    (void)n;
    --allocations;
    free(p);
}
bool amd64_virtualization_hw_enable(enum ia32_backend_kind b, struct amd64_virtualization_hw *h,
                                    const char **why)
{
    if (hw_fail) {
        *why = "hardware refusal";
        return false;
    }
    h->page = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (!h->page) {
        *why = "hardware page allocation failed";
        return false;
    }
    ++enables;
    h->tags = b == IA32_BACKEND_SVM ? 4 : 65536;
    return true;
}
void amd64_virtualization_hw_disable(enum ia32_backend_kind b, struct amd64_virtualization_hw *h)
{
    (void)b;
    hal_free_pages(h->page, 1);
    ++disables;
}
bool amd64_virtualization_hw_invalidate(const struct amd64_virtualization_hw *h, uint32_t id,
                                        uint64_t root)
{
    (void)h;
    (void)id;
    (void)root;
    ++invalidations;
    return !invalidate_fail;
}

int main(void)
{
    struct amd64_virtualization_cpu *a, *b, *bad;
    struct amd64_virtualization_tag t[4], reused;
    struct ia32_setup_error error;
    uint8_t tlb;
    CHECK(!amd64_virtualization_acquire(1, IA32_BACKEND_SVM, AMD64_VIRT_IA32, &bad, &error) &&
          !bad);
    CHECK(!allocations);
    hw_fail = true;
    CHECK(!amd64_virtualization_acquire(0, IA32_BACKEND_SVM, AMD64_VIRT_IA32, &bad, &error) &&
          !bad);
    CHECK(!allocations);
    hw_fail = false;
    for (unsigned failure = 1; failure <= 2; ++failure) {
        attempts = 0;
        fail_at = failure;
        CHECK(!amd64_virtualization_acquire(0, IA32_BACKEND_SVM, AMD64_VIRT_IA32, &bad, &error) &&
              !bad);
        CHECK(!allocations && enables == disables);
    }
    fail_at = 0;
    CHECK(amd64_virtualization_acquire(0, IA32_BACKEND_SVM, AMD64_VIRT_VBIOS, &a, &error));
    CHECK(amd64_virtualization_acquire(0, IA32_BACKEND_SVM, AMD64_VIRT_IA32, &b, &error) && a == b);
    CHECK(enables == 1);
    fail_at = attempts + 1;
    CHECK(!amd64_virtualization_tag_acquire(b, AMD64_VIRT_IA32, &reused, &error));
    CHECK(!reused.id && !reused.generation && allocations == 2);
    fail_at = 0;
    CHECK(!amd64_virtualization_acquire(0, IA32_BACKEND_VMX, AMD64_VIRT_IA32, &bad, &error) &&
          !bad);
    CHECK(amd64_virtualization_tag_acquire(a, AMD64_VIRT_VBIOS, &t[0], &error) && t[0].id == 1);
    CHECK(!amd64_virtualization_tag_acquire(a, AMD64_VIRT_VBIOS, &reused, &error));
    amd64_virtualization_release(a, AMD64_VIRT_VBIOS);
    CHECK(!disables);
    CHECK(amd64_virtualization_tag_acquire(b, AMD64_VIRT_IA32, &t[1], &error) && t[1].id == 2);
    CHECK(amd64_virtualization_tag_acquire(b, AMD64_VIRT_IA32, &t[2], &error) && t[2].id == 3);
    CHECK(!amd64_virtualization_tag_acquire(b, AMD64_VIRT_IA32, &reused, &error));
    CHECK(amd64_virtualization_tag_prepare(a, AMD64_VIRT_VBIOS, &t[0], 0, &tlb) && tlb == 1);
    amd64_virtualization_tag_entered(a, AMD64_VIRT_VBIOS, &t[0], false);
    CHECK(amd64_virtualization_tag_prepare(a, AMD64_VIRT_VBIOS, &t[0], 0, &tlb) && tlb == 1);
    amd64_virtualization_tag_entered(a, AMD64_VIRT_VBIOS, &t[0], true);
    CHECK(amd64_virtualization_tag_prepare(a, AMD64_VIRT_VBIOS, &t[0], 0, &tlb) && !tlb);
    CHECK(amd64_virtualization_tag_prepare(a, AMD64_VIRT_VBIOS, &t[0], 0x1000, &tlb) && tlb == 1);
    cpu = 1;
    CHECK(!amd64_virtualization_tag_prepare(a, AMD64_VIRT_VBIOS, &t[0], 0, &tlb));
    CHECK(!amd64_virtualization_tag_acquire(b, AMD64_VIRT_IA32, &reused, &error));
    amd64_virtualization_tag_release(a, AMD64_VIRT_VBIOS, &t[0]);
    amd64_virtualization_release(a, AMD64_VIRT_VBIOS);
    CHECK(!disables);
    CHECK(amd64_virtualization_acquire(1, IA32_BACKEND_SVM, AMD64_VIRT_IA32, &bad, &error) &&
          bad != a);
    CHECK(amd64_virtualization_tag_acquire(bad, AMD64_VIRT_IA32, &t[3], &error) && t[3].id == 2);
    amd64_virtualization_tag_release(bad, AMD64_VIRT_IA32, &t[3]);
    amd64_virtualization_release(bad, AMD64_VIRT_IA32);
    CHECK(disables == 1);
    cpu = 0;
    amd64_virtualization_tag_release(b, AMD64_VIRT_IA32, &t[1]);
    CHECK(amd64_virtualization_tag_acquire(b, AMD64_VIRT_IA32, &reused, &error));
    CHECK(reused.id == t[1].id && reused.generation != t[1].generation);
    amd64_virtualization_tag_release(b, AMD64_VIRT_IA32, &t[1]);
    CHECK(!amd64_virtualization_tag_prepare(b, AMD64_VIRT_IA32, &t[1], 0, &tlb));
    CHECK(!amd64_virtualization_tag_prepare(b, AMD64_VIRT_VBIOS, &reused, 0, &tlb));
    CHECK(amd64_virtualization_tag_prepare(b, AMD64_VIRT_IA32, &reused, 0, &tlb) && tlb == 1);
    amd64_virtualization_tag_release(b, AMD64_VIRT_IA32, &reused);
    amd64_virtualization_tag_release(b, AMD64_VIRT_IA32, &t[2]);
    amd64_virtualization_release(b, AMD64_VIRT_IA32);
    CHECK(disables == 1);
    amd64_virtualization_tag_release(a, AMD64_VIRT_VBIOS, &t[0]);
    amd64_virtualization_release(a, AMD64_VIRT_VBIOS);
    CHECK(disables == 2 && !allocations);
    CHECK(amd64_virtualization_acquire(0, IA32_BACKEND_VMX, AMD64_VIRT_VBIOS, &a, &error));
    CHECK(amd64_virtualization_tag_acquire(a, AMD64_VIRT_VBIOS, &t[0], &error) && !t[0].id);
    invalidate_fail = true;
    CHECK(!amd64_virtualization_tag_prepare(a, AMD64_VIRT_VBIOS, &t[0], 0x101e, &tlb));
    invalidate_fail = false;
    CHECK(amd64_virtualization_tag_prepare(a, AMD64_VIRT_VBIOS, &t[0], 0x101e, &tlb));
    CHECK(invalidations == 2);
    CHECK(amd64_virtualization_tag_prepare(a, AMD64_VIRT_VBIOS, &t[0], 0x101e, &tlb));
    CHECK(invalidations == 2);
    CHECK(amd64_virtualization_tag_prepare(a, AMD64_VIRT_VBIOS, &t[0], 0x201e, &tlb));
    unsigned before_invalidate = invalidations;
    CHECK(amd64_virtualization_tag_invalidate(a, AMD64_VIRT_VBIOS, &t[0]));
    CHECK(invalidations == before_invalidate);
    CHECK(amd64_virtualization_tag_prepare(a, AMD64_VIRT_VBIOS, &t[0], 0x201e, &tlb));
    CHECK(invalidations == before_invalidate + 1);
    CHECK(invalidations == 4);
    /* Zero VPID leases still have distinct identities and share VMXON. */
    CHECK(amd64_virtualization_acquire(0, IA32_BACKEND_VMX, AMD64_VIRT_VBIOS, &b, &error) &&
          a == b);
    CHECK(amd64_virtualization_tag_acquire(b, AMD64_VIRT_VBIOS, &t[1], &error));
    CHECK(!t[1].id && t[1].generation != t[0].generation);
    amd64_virtualization_tag_release(b, AMD64_VIRT_VBIOS, &t[1]);
    amd64_virtualization_release(b, AMD64_VIRT_VBIOS);
    amd64_virtualization_tag_release(a, AMD64_VIRT_VBIOS, &t[0]);
    amd64_virtualization_release(a, AMD64_VIRT_VBIOS);
    CHECK(!allocations && enables == disables);
    puts(
        "virtualization ownership: shared lifecycle, pinning, lease exhaustion/reuse and failed invalidation passed");
    return 0;
}
