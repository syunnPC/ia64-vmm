/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Executes production FP brackets without virtualization or C while guest
 * FP is loaded. This does not qualify hardware VMRUN/IRQ ordering. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/ia32_svm.h"
#include "host/amd64/amd64.h"
#include "host/amd64/simd.h"
#include "ia64vmm/hal.h"
#include "tests/host/test.h"
static unsigned allocations;
unsigned hal_cpu_index(void)
{
    return 0;
}
void *hal_alloc_pages(size_t n, unsigned flags)
{
    (void)flags;
    void *p = aligned_alloc(4096, n * 4096);
    if (p) {
        memset(p, 0, n * 4096);
        ++allocations;
    }
    return p;
}
void hal_free_pages(void *p, size_t n)
{
    (void)n;
    --allocations;
    free(p);
}
bool amd64_ia32_fp_requirements(struct amd64_ia32_fp_requirements *r, struct ia32_setup_error *e)
{
    (void)e;
    uint32_t a, b, c, d;
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    CHECK(c & (1u << 27));
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    amd64_cpuid(0xd, 0, &a, &b, &c, &d);
    *r = (struct amd64_ia32_fp_requirements){
        .bytes = b, .alignment = 64, .xcr0 = (uint64_t)hi << 32 | lo, .xsave = true};
    amd64_cpuid(0xd, 1, &a, &b, &c, &d);
    r->xgetbv1 = (a & 4) != 0;
    amd64_cpuid(0, 0, &a, &b, &c, &d);
    r->host_x87_initial_only = b == UINT32_C(0x68747541) || b == UINT32_C(0x6f677948);
    return true;
}
void test_ia32_fp_switch(struct amd64_ia32_fp_owner *, struct ia32_fp_image *, void *, void *);
static void wr(uint8_t *b, unsigned off, uint64_t value, unsigned n)
{
    memcpy(b + off, &value, n);
}
static bool same_host(const uint8_t *a, const uint8_t *b, size_t bytes)
{
    /* XRSTOR may recognize an explicitly stored zero component as initial;
     * XSTATE_BV is init tracking, while the saved component values agree. */
    return !memcmp(a, b, 512) && !memcmp(a + 520, b + 520, bytes - 520);
}
int main(void)
{
    struct amd64_ia32_fp_owner *owner;
    struct ia32_setup_error error;
    struct amd64_ia32_fp_requirements r;
    CHECK(amd64_ia32_fp_requirements(&r, &error));
    if (!amd64_ia32_fp_create(&owner, &error)) {
        CHECK(error.reason == IA32_EXIT_UNIMPLEMENTED);
        puts(
            "native FP assembly: host initial-state indication unavailable; execution unsupported");
        return 0;
    }
    if (!amd64_ia32_fp_host_admit(owner, &error)) {
        CHECK(error.reason == IA32_EXIT_UNIMPLEMENTED);
        amd64_ia32_fp_destroy(owner);
        CHECK(!allocations);
        puts("native FP assembly: host x87 is noninitial; execution unsupported");
        return 0;
    }
    size_t n = (r.bytes + 4095) / 4096;
    uint8_t *before = hal_alloc_pages(n, 0), *after = hal_alloc_pages(n, 0);
    /* Boot may own an arbitrary x87 state. Initializing through XRSTOR
     * must establish both architectural reset and its init indication. */
    __asm__ volatile("xsave64 %0"
                     : "=m"(*(uint8_t (*)[576])before)
                     : "a"((uint32_t)r.xcr0), "d"((uint32_t)(r.xcr0 >> 32))
                     : "memory");
    amd64_xstate_init_x87();
    bool initial_x87 = !r.xgetbv1 || !(amd64_ia32_fp_host_inuse() & 1);
    __asm__ volatile("xrstor64 %0" ::"m"(*(uint8_t (*)[576])before), "a"((uint32_t)r.xcr0),
                     "d"((uint32_t)(r.xcr0 >> 32))
                     : "memory");
    CHECK(initial_x87);
    struct ia32_fp_image fp = {0};
    wr(fp.bytes, 0, 0x37f, 2);
    wr(fp.bytes, 2, 5 << 11, 2);
    fp.bytes[4] = 0xad;
    wr(fp.bytes, 6, 0x345, 2);
    wr(fp.bytes, 8, 0xabcdef01, 4);
    wr(fp.bytes, 12, 0x23, 2);
    wr(fp.bytes, 16, 0x12345678, 4);
    wr(fp.bytes, 20, 0x2b, 2);
    wr(fp.bytes, 24, 0x1f80, 4);
    wr(fp.bytes, 28, 0xffbf, 4);
    for (unsigned i = 0; i < 8; ++i) {
        wr(fp.bytes, 32 + 16 * i, UINT64_C(0x8000000000000000) + i, 8);
        wr(fp.bytes, 40 + 16 * i, 0x3fff + i, 2);
        wr(fp.bytes, 160 + 16 * i, UINT64_C(0xabcdef0123456789) + i, 8);
        wr(fp.bytes, 168 + 16 * i, UINT64_C(0x9876543210fedcba) + i, 8);
    }
    struct ia32_fp_image expected = fp;
    CHECK(amd64_ia32_fp_prepare(owner, &fp, &error));
    CHECK(amd64_ia32_fp_host_admit(owner, &error));
    test_ia32_fp_switch(owner, &fp, before, after);
    amd64_ia32_fp_complete(owner, &fp);
    CHECK(!memcmp(&fp, &expected, sizeof(fp)));
    CHECK(same_host(before, after, r.bytes));
    /* Install pending unmasked #MF without executing a waiting host x87 op. */
    wr(fp.bytes, 0, 0x37e, 2);
    wr(fp.bytes, 2, (5 << 11) | 0x8081, 2);
    expected = fp;
    memset(before, 0, n * 4096);
    memset(after, 0, n * 4096);
    CHECK(amd64_ia32_fp_prepare(owner, &fp, &error));
    CHECK(amd64_ia32_fp_host_admit(owner, &error));
    test_ia32_fp_switch(owner, &fp, before, after);
    amd64_ia32_fp_complete(owner, &fp);
    CHECK(!memcmp(&fp, &expected, sizeof(fp)) && same_host(before, after, r.bytes));
    amd64_ia32_fp_destroy(owner);
    hal_free_pages(before, n);
    hal_free_pages(after, n);
    CHECK(!allocations);
    puts(
        "native FP assembly: legacy pointers, TOP/tags, XMM halves, pending MF and host xstate passed");
    return 0;
}
