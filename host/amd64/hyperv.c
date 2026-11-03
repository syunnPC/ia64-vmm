/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Microsoft TLFS: hypercall-interface, inter-partition-communication,
 * HV_MESSAGE, HvCallPostMessage and HvCallSignalEvent. */
#include "host/amd64/amd64.h"
#include "host/amd64/hyperv.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

enum {
    MSR_GUEST_OS_ID = 0x40000000,
    MSR_HYPERCALL = 0x40000001,
    MSR_VP_INDEX = 0x40000002,
    MSR_REFERENCE = 0x40000020,
    MSR_TSC_FREQUENCY = 0x40000022,
    MSR_SCONTROL = 0x40000080,
    MSR_SIEFP = 0x40000082,
    MSR_SIMP = 0x40000083,
    MSR_EOM = 0x40000084,
    MSR_SINT0 = 0x40000090
};
static struct {
    uint8_t *call, *messages, *events, *input;
    uint64_t input_pa, old_control, old_simp, old_siefp, old_sint;
    unsigned cpu;
    uint32_t vp;
    bool live, used;
} hv;

static bool features(uint32_t *features)
{
    uint32_t a, b, c, d;
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 31)))
        return false;
    amd64_cpuid(0x40000000, 0, &a, &b, &c, &d);
    if (a < 0x40000003)
        return false;
    amd64_cpuid(0x40000001, 0, &a, &b, &c, &d);
    if (a != 0x31237648)
        return false; /* "Hv#1" */
    amd64_cpuid(0x40000003, 0, &a, &b, &c, &d);
    *features = a;
    return true;
}
bool amd64_hyperv_detect(void)
{
    uint32_t f, a, b, c, d;
    if (!features(&f) || (f & 0x64) != 0x64)
        return false;
    amd64_cpuid(0x40000003, 0, &a, &b, &c, &d);
    /* SynIC, hypercalls, VP index, PostMessages and SignalEvents are the
     * transport prerequisites. VMBus contact must still confirm service. */
    return (b & 0x30) == 0x30;
}
bool amd64_hyperv_reference_available(void)
{
    uint32_t f;
    return features(&f) && (f & (1u << 1));
}
uint64_t amd64_hyperv_reference(void)
{
    return amd64_rdmsr(MSR_REFERENCE);
}
uint64_t amd64_hyperv_tsc_hz(void)
{
    uint32_t f, a, b, c, d;
    if (!features(&f) || !(f & (1u << 11)))
        return 0;
    amd64_cpuid(0x40000003, 0, &a, &b, &c, &d);
    return d & (1u << 8) ? amd64_rdmsr(MSR_TSC_FREQUENCY) : 0;
}
static bool owner(void)
{
    return hv.live && hal_cpu_index() == hv.cpu;
}
static bool physical(const void *p, uint64_t *pa)
{
    return amd64_memory_physical((uintptr_t)p, pa) && !(*pa & 4095);
}
/* TLFS x64 convention: RCX control, RDX input GPA, R8 output GPA, RAX
 * result. Reserve the Microsoft x64 volatile registers for the call stub. */
static uint16_t call(uint64_t control, uint64_t input)
{
    uint64_t result;
    register uint64_t output __asm__("r8") = 0;
    __asm__ volatile("call *%[page]"
                     : "=a"(result), "+c"(control), "+d"(input), "+r"(output)
                     : [page] "r"(hv.call)
                     : "r9", "r10", "r11", "cc", "memory");
    return (uint16_t)result;
}
bool amd64_hyperv_start(void)
{
    if (hv.live)
        return owner();
    /* Do not take over another guest runtime's active hypercall page or
     * SynIC state. */
    if (hv.used || !amd64_hyperv_detect())
        return false;
    if (amd64_rdmsr(MSR_HYPERCALL) || amd64_rdmsr(MSR_GUEST_OS_ID)) {
        LOGE("Hyper-V firmware left an active hypercall interface after ExitBootServices");
        return false;
    }
    hv.used = true;
    hv.cpu = hal_cpu_index();
    hv.old_control = amd64_rdmsr(MSR_SCONTROL);
    hv.old_simp = amd64_rdmsr(MSR_SIMP);
    hv.old_siefp = amd64_rdmsr(MSR_SIEFP);
    hv.old_sint = amd64_rdmsr(MSR_SINT0 + AMD64_HYPERV_SINT);
    if ((hv.old_simp | hv.old_siefp) & 1) {
        LOGE("Hyper-V firmware left active SynIC pages after ExitBootServices");
        return false;
    }
    hv.call = hal_alloc_pages(1, HAL_PAGE_ZERO | HAL_PAGE_EXEC);
    hv.messages = hal_alloc_pages(1, HAL_PAGE_ZERO);
    hv.events = hal_alloc_pages(1, HAL_PAGE_ZERO);
    hv.input = hal_alloc_pages(1, HAL_PAGE_ZERO);
    uint64_t call_pa, messages_pa, events_pa;
    if (!hv.call || !hv.messages || !hv.events || !hv.input || !physical(hv.call, &call_pa) ||
        !physical(hv.messages, &messages_pa) || !physical(hv.events, &events_pa) ||
        !physical(hv.input, &hv.input_pa))
        return false;
    /* TLFS open-source OS identity: vendor 0, OS 0 (undefined), version 1. */
    amd64_wrmsr(MSR_GUEST_OS_ID, UINT64_C(0x8000000000010000));
    amd64_wrmsr(MSR_HYPERCALL, call_pa | 1);
    if (!(amd64_rdmsr(MSR_HYPERCALL) & 1))
        return false;
    hal_code_set_executable(hv.call, 1);
    hv.vp = (uint32_t)amd64_rdmsr(MSR_VP_INDEX);
    amd64_wrmsr(MSR_SIMP, messages_pa | 1);
    amd64_wrmsr(MSR_SIEFP, events_pa | 1);
    /* An unmasked SINT is needed for HvCallSignalEvent. Use ordinary APIC
     * EOI, not deprecated SynIC auto-EOI. The IRQ handler never owns data. */
    amd64_wrmsr(MSR_SINT0 + AMD64_HYPERV_SINT, AMD64_HYPERV_VECTOR);
    amd64_wrmsr(MSR_SCONTROL, hv.old_control | 1);
    hv.live = true;
    return true;
}
uint32_t amd64_hyperv_vp(void)
{
    return hv.vp;
}
uint16_t amd64_hyperv_post(uint32_t connection, const void *data, unsigned bytes)
{
    if (!owner() || !connection || bytes > 240 || !data)
        return 5;
    /* HV_INPUT_POST_MESSAGE: connection ID, reserved, message type, size,
     * then up to 240 payload bytes. */
    memset(hv.input, 0, 256);
    uint32_t fields[4] = {connection, 0, 1, bytes};
    memcpy(hv.input, fields, sizeof(fields));
    memcpy(hv.input + 16, data, bytes);
    return call(0x5c, hv.input_pa);
}
uint16_t amd64_hyperv_signal(uint32_t connection)
{
    if (!owner() || !connection)
        return 5;
    /* HV_INPUT_SIGNAL_EVENT: connection ID, 16-bit flag number (zero),
     * 16-bit reserved. Use memory convention; no XMM state is involved. */
    uint64_t input = connection;
    memcpy(hv.input, &input, sizeof(input));
    return call(0x5d, hv.input_pa);
}
int amd64_hyperv_message(void *out, unsigned capacity, unsigned *bytes)
{
    if (!owner())
        return -1;
    uint8_t *slot = hv.messages + (size_t)AMD64_HYPERV_SINT * 256;
    uint32_t type = __atomic_load_n((uint32_t *)slot, __ATOMIC_ACQUIRE);
    if (!type)
        return 0;
    unsigned size = *(volatile uint8_t *)(slot + 4);
    bool valid = type == 1 && size <= 240 && size <= capacity;
    if (valid) {
        memcpy(out, slot + 16, size);
        *bytes = size;
    }
    __atomic_store_n((uint32_t *)slot, 0, __ATOMIC_RELEASE);
    /* TLFS: test pending only after emptying the message slot. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (*(volatile uint8_t *)(slot + 5) & 1)
        amd64_wrmsr(MSR_EOM, 0);
    return valid ? 1 : -1;
}
void amd64_hyperv_events(void)
{
    if (!owner())
        return;
    uint64_t *flags = (uint64_t *)(hv.events + (size_t)AMD64_HYPERV_SINT * 256);
    for (unsigned i = 0; i < 32; i++)
        __atomic_exchange_n(flags + i, 0, __ATOMIC_ACQ_REL);
}
void amd64_hyperv_stop(void)
{
    if (!owner())
        return;
    amd64_wrmsr(MSR_SCONTROL, hv.old_control);
    amd64_wrmsr(MSR_SINT0 + AMD64_HYPERV_SINT, hv.old_sint);
    amd64_wrmsr(MSR_SIMP, hv.old_simp);
    amd64_wrmsr(MSR_SIEFP, hv.old_siefp);
    amd64_wrmsr(MSR_HYPERCALL, 0);
    amd64_wrmsr(MSR_GUEST_OS_ID, 0);
    hv.live = false;
}
