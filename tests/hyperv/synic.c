/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Expectations from Microsoft TLFS feature-discovery, hypercall-interface,
 * inter-partition-communication and HV_MESSAGE. No privileged instructions. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host/amd64/amd64.h"
#include "host/amd64/hyperv.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
static void test_cpuid(uint32_t, uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
static uint64_t test_rdmsr(uint32_t);
static void test_wrmsr(uint32_t, uint64_t);
#define amd64_cpuid test_cpuid
#define amd64_rdmsr test_rdmsr
#define amd64_wrmsr test_wrmsr
#include "host/amd64/hyperv.c"
#undef amd64_cpuid
#undef amd64_rdmsr
#undef amd64_wrmsr

static _Alignas(4096) uint8_t pages[4 * 4096];
static uint64_t msrs[0x100];
static unsigned allocated, feature_mask, hypercall_mask, misc_mask, cpu, executable, eoms, errors;
static bool present, interface;
static uint64_t captured[3] __attribute__((used));
static __attribute__((naked)) void hypercall_stub(void)
{
    __asm__("mov %rcx, captured(%rip)\n"
            "mov %rdx, captured+8(%rip)\n"
            "mov %r8, captured+16(%rip)\n"
            "mov $0x12340013, %eax\nret");
}
static void test_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c,
                       uint32_t *d)
{
    assert(sub == 0);
    *a = *b = *c = *d = 0;
    if (leaf == 1)
        *c = present ? 1u << 31 : 0;
    else if (leaf == 0x40000000)
        *a = 0x40000003;
    else if (leaf == 0x40000001)
        *a = interface ? 0x31237648 : 0;
    else if (leaf == 0x40000003) {
        *a = feature_mask;
        *b = hypercall_mask;
        *d = misc_mask;
    } else
        assert(false);
}
static uint64_t test_rdmsr(uint32_t msr)
{
    assert(msr >= 0x40000000 && msr < 0x40000100);
    return msrs[msr - 0x40000000];
}
static void test_wrmsr(uint32_t msr, uint64_t value)
{
    assert(msr >= 0x40000000 && msr < 0x40000100);
    msrs[msr - 0x40000000] = value;
    if (msr == 0x40000084) {
        assert(value == 0);
        eoms++;
    }
}
void vmm_log(enum log_level level, const char *fmt, ...)
{
    assert(level == LOG_ERROR);
    (void)fmt;
    errors++;
}
unsigned hal_cpu_index(void)
{
    return cpu;
}
void *hal_alloc_pages(size_t count, unsigned flags)
{
    assert(count == 1 && allocated < 4 && (flags & HAL_PAGE_ZERO));
    if (!allocated)
        assert(flags & HAL_PAGE_EXEC);
    else
        assert(!(flags & HAL_PAGE_EXEC));
    return pages + 4096 * allocated++;
}
void hal_code_set_executable(void *p, size_t count)
{
    assert(p == pages && count == 1);
    executable++;
}
bool amd64_memory_physical(uint64_t va, uint64_t *pa)
{
    assert(va >= (uintptr_t)pages && va < (uintptr_t)pages + sizeof(pages));
    *pa = va - (uintptr_t)pages + 0x200000;
    return true;
}
static void reset(void)
{
    memset(&hv, 0, sizeof(hv));
    memset(msrs, 0, sizeof(msrs));
    memset(pages, 0, sizeof(pages));
    present = interface = true;
    feature_mask = 0x64;
    hypercall_mask = 0x30;
    misc_mask = 1u << 8;
    allocated = cpu = executable = eoms = errors = 0;
    msrs[0x80] = 1; /* SynIC control may remain enabled after firmware cleanup. */
    msrs[0x92] = 1u << 16;
    msrs[2] = 9;
}
int main(void)
{
    reset();
    present = false;
    assert(!amd64_hyperv_detect() && !amd64_hyperv_start());
    reset();
    interface = false;
    assert(!amd64_hyperv_detect() && !amd64_hyperv_start());
    for (unsigned bit = 0; bit < 7; bit++)
        if (0x64 & (1u << bit)) {
            reset();
            feature_mask &= ~(1u << bit);
            assert(!amd64_hyperv_detect() && !amd64_hyperv_start() && !allocated);
        }
    reset();
    hypercall_mask = 0x20;
    assert(!amd64_hyperv_detect() && !amd64_hyperv_start() && !allocated);
    reset();
    hypercall_mask = 0x10;
    assert(!amd64_hyperv_detect() && !amd64_hyperv_start() && !allocated);
    reset();
    feature_mask = 0x68;
    assert(!amd64_hyperv_start()); /* Timer is not SynIC. */
    reset();
    assert(!amd64_hyperv_reference_available());
    feature_mask = 2;
    hypercall_mask = 0; /* Clock-only Hv#1 enlightenment. */
    assert(!amd64_hyperv_detect());
    msrs[0x20] = UINT64_C(1234567890123);
    assert(amd64_hyperv_reference_available() &&
           amd64_hyperv_reference() == UINT64_C(1234567890123));
    msrs[0x22] = UINT64_C(2600000000);
    assert(amd64_hyperv_tsc_hz() == 0);
    feature_mask |= 1u << 11;
    assert(amd64_hyperv_tsc_hz() == UINT64_C(2600000000));
    misc_mask = 0;
    assert(amd64_hyperv_tsc_hz() == 0);
    reset();
    msrs[1] = 1;
    assert(!amd64_hyperv_start() && errors == 1 && !allocated);
    reset();
    msrs[0x83] = 0x100001;
    assert(!amd64_hyperv_start() && errors == 1 && !allocated);
    reset();
    assert(amd64_hyperv_detect() && amd64_hyperv_start() && allocated == 4 && executable == 1);
    assert(msrs[0] == UINT64_C(0x8000000000010000) && msrs[1] == 0x200001);
    assert(msrs[0x83] == 0x201001 && msrs[0x82] == 0x202001);
    assert(msrs[0x92] == AMD64_HYPERV_VECTOR && msrs[0x80] == 1 && amd64_hyperv_vp() == 9);
    hv.call = (uint8_t *)(uintptr_t)hypercall_stub;
    const uint8_t posted[] = {14, 0, 0, 0, 0, 0, 0, 0};
    assert(amd64_hyperv_post(0x123, posted, sizeof(posted)) == 19);
    assert(captured[0] == 0x5c && captured[1] == 0x203000 && captured[2] == 0);
    const uint8_t post_header[] = {0x23, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0, 0};
    assert(memcmp(pages + 12288, post_header, 16) == 0);
    assert(memcmp(pages + 12304, posted, 8) == 0);
    assert(amd64_hyperv_signal(0x123) == 19);
    assert(captured[0] == 0x5d && captured[1] == 0x203000 && captured[2] == 0);
    const uint8_t event_input[] = {0x23, 1, 0, 0, 0, 0, 0, 0};
    assert(memcmp(pages + 12288, event_input, 8) == 0);
    assert(amd64_hyperv_post(1, posted, 241) == 5);
    uint8_t *slot = pages + 4096 + 2 * 256;
    uint8_t payload[240], output[240];
    unsigned bytes;
    for (unsigned i = 0; i < sizeof(payload); i++)
        payload[i] = i;
    assert(amd64_hyperv_message(output, sizeof(output), &bytes) == 0);
    slot[0] = 1;
    slot[4] = 240;
    slot[5] = 1;
    memcpy(slot + 16, payload, 240);
    assert(amd64_hyperv_message(output, sizeof(output), &bytes) == 1 && bytes == 240);
    assert(memcmp(output, payload, 240) == 0 && slot[0] == 0 && eoms == 1);
    slot[0] = 1;
    slot[4] = 241;
    slot[5] = 0;
    assert(amd64_hyperv_message(output, sizeof(output), &bytes) == -1 && slot[0] == 0 && eoms == 1);
    slot[0] = 1;
    slot[4] = 16;
    assert(amd64_hyperv_message(output, 15, &bytes) == -1 && slot[0] == 0);
    slot[0] = 2;
    slot[4] = 16;
    assert(amd64_hyperv_message(output, sizeof(output), &bytes) == -1 && slot[0] == 0);
    uint8_t *events = pages + 8192;
    memset(events, 0xff, 4096);
    amd64_hyperv_events();
    for (unsigned i = 0; i < 4096; i++)
        assert(events[i] == (i >= 512 && i < 768 ? 0 : 255));
    cpu = 1;
    assert(!amd64_hyperv_start() && amd64_hyperv_message(output, sizeof(output), &bytes) == -1);
    amd64_hyperv_stop();
    assert(msrs[1]);
    cpu = 0;
    amd64_hyperv_stop();
    assert(msrs[0] == 0 && msrs[1] == 0 && msrs[0x83] == 0 && msrs[0x82] == 0);
    assert(msrs[0x92] == (1u << 16) && msrs[0x80] == 1 && allocated == 4);
    assert(!amd64_hyperv_start());
    puts("Hyper-V feature and SynIC tests passed");
    return 0;
}
