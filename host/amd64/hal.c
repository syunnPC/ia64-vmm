/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Amd64/Time/Amd64TscClock.cpp
 */
#include "host/amd64/amd64.h"
#include "host/amd64/hyperv.h"
#include "host/amd64/smp.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"
#include "core/log_internal.h"
#if !defined(IA64VMM_AMD64_HOST_ONLY)
#include "host/amd64/plog_ahci.h"
#include "host/amd64/plog_ehci.h"
#include "host/amd64/plog_nvme.h"
#include "host/amd64/plog_hyperv.h"
#include "host/amd64/hyperv_keyboard.h"
#include "host/amd64/vmbus.h"
#include "persist/persist.h"
#endif
#include "core/vm.h"
#include "host/amd64/calibration.h"
#include "host/fb/fb_console.h"

static bool serial_present, debug_exit, force_halt;
static unsigned serial_burst = 1, serial_credit, serial_lock;
static uint64_t tsc_hz;
static const char *tsc_source = "unavailable";
static bool invariant_tsc;
uint32_t hal_port_in(uint16_t port, unsigned size)
{
    uint32_t value;
    switch (size) {
    case 1: {
        uint8_t v;
        __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
        value = v;
        break;
    }
    case 2: {
        uint16_t v;
        __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port));
        value = v;
        break;
    }
    case 4:
        __asm__ volatile("inl %1,%0" : "=a"(value) : "Nd"(port));
        break;
    default:
        hal_panic("invalid port input width");
    }
    return value;
}
void hal_port_out(uint16_t port, uint32_t value, unsigned size)
{
    switch (size) {
    case 1:
        __asm__ volatile("outb %0,%1" ::"a"((uint8_t)value), "Nd"(port));
        break;
    case 2:
        __asm__ volatile("outw %0,%1" ::"a"((uint16_t)value), "Nd"(port));
        break;
    case 4:
        __asm__ volatile("outl %0,%1" ::"a"(value), "Nd"(port));
        break;
    default:
        hal_panic("invalid port output width");
    }
}
void amd64_serial_init(void)
{
    uint8_t old = hal_port_in(0x3ff, 1);
    hal_port_out(0x3ff, 0x5a, 1);
    bool first = hal_port_in(0x3ff, 1) == 0x5a;
    hal_port_out(0x3ff, 0xa5, 1);
    serial_present = first && hal_port_in(0x3ff, 1) == 0xa5;
    hal_port_out(0x3ff, old, 1);
    if (!serial_present)
        return;
    hal_port_out(0x3f9, 0, 1);
    hal_port_out(0x3fb, 0x80, 1);
    hal_port_out(0x3f8, 1, 1);
    hal_port_out(0x3f9, 0, 1);
    hal_port_out(0x3fb, 3, 1);
    hal_port_out(0x3fa, 0xc7, 1);
    hal_port_out(0x3fc, 3, 1);
    /* IIR bits 7:6 = 11: 16550A with a working 16-byte transmit FIFO. */
    serial_burst = (hal_port_in(0x3fa, 1) & 0xc0) == 0xc0 ? 16 : 1;
}
static void serial_byte(char c)
{
    /* LSR.THRE means the transmit FIFO is empty, so up to serial_burst
     * bytes fit without polling again. Every port access is a VM exit under
     * a hypervisor (about 1 ms each under WHPX), so this matters. */
    if (serial_credit == 0) {
        /* A missing/stuck UART must not prevent a panic or shutdown. */
        for (unsigned tries = 0;; ++tries) {
            if (hal_port_in(0x3fd, 1) & 0x20)
                break;
            if (tries == 100000)
                return;
            __asm__ volatile("pause");
        }
        serial_credit = serial_burst;
    }
    hal_port_out(0x3f8, (uint8_t)c, 1);
    --serial_credit;
}
void hal_debug_write(const char *s, size_t n)
{
    if (!serial_present)
        return;
    /* Normal mirrors have one management writer. Raw panic output may
     * interrupt that writer: prefer dropping a mirror over waiting on it. */
    if (__atomic_exchange_n(&serial_lock, 1, __ATOMIC_ACQUIRE))
        return;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\n')
            serial_byte('\r');
        serial_byte(s[i]);
    }
    __atomic_store_n(&serial_lock, 0, __ATOMIC_RELEASE);
}

/* Mode 3's 80x25 text buffer at
 * B8000h, mapped UC like every device window (PAT[3]). */
bool hal_console_vga_text(void)
{
    uint8_t *buffer = amd64_map_mmio(0xb8000, 4096);
    if (!buffer)
        return false;
    fb_console_text((volatile uint16_t *)buffer, 80, 25);
    return true;
}

bool amd64_option(const char *cmdline, const char *option)
{
    size_t n = strlen(option);
    while (*cmdline) {
        while (*cmdline == ' ' || *cmdline == '\t')
            ++cmdline;
        const char *end = cmdline;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        if ((size_t)(end - cmdline) == n && !strncmp(cmdline, option, n))
            return true;
        cmdline = end;
    }
    return false;
}
void amd64_shutdown_exit_options(const char *cmdline)
{
    debug_exit = amd64_option(cmdline, "exit=isa-debug-exit");
    force_halt = amd64_option(cmdline, "exit=halt");
}
void amd64_shutdown_init(const struct boot_info *bi)
{
    amd64_shutdown_exit_options(bi->cmdline);
    amd64_acpi_init(bi);
    if (bi->mem_range_count)
        amd64_pci_init(bi);
}

uint64_t hal_counter(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return ((uint64_t)hi << 32) | lo;
}
uint64_t hal_counter_hz(void)
{
    return tsc_hz;
}
const char *amd64_time_source(void)
{
    return tsc_source;
}
void amd64_time_init(const struct boot_info *bi)
{
    uint32_t a, b, c, d, max;
    amd64_cpuid(0x80000000, 0, &a, &b, &c, &d);
    max = a;
    if (max >= 0x80000007)
        amd64_cpuid(0x80000007, 0, &a, &b, &c, &d);
    invariant_tsc = max >= 0x80000007 && (d & (1u << 8));
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    if (!invariant_tsc && !(c & (1u << 31)))
        hal_panic("invariant TSC is required on physical hardware");
    if (invariant_tsc)
        LOGI("invariant TSC verified by CPUID.80000007H:EDX[8]");
    else
        LOGW("virtual TSC lacks invariant CPUID guarantee; cross-CPU brackets checked at boot");
    const char *source = NULL;
    amd64_cpuid(0, 0, &a, &b, &c, &d);
    max = a;
    if (max >= 0x15) {
        amd64_cpuid(0x15, 0, &a, &b, &c, &d);
        if (a && b && c) {
            tsc_hz = amd64_calibration_hz(b, c, a);
            source = "CPUID 0x15";
        }
    }
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    if (!tsc_hz && (c & (1u << 31))) {
        amd64_cpuid(0x40000000, 0, &a, &b, &c, &d);
        if (a >= 0x40000010 && a < 0x40010000) {
            amd64_cpuid(0x40000010, 0, &a, &b, &c, &d);
            if (a) {
                tsc_hz = (uint64_t)a * 1000;
                source = "hypervisor CPUID 0x40000010";
            }
        }
    }
    if (!tsc_hz) {
        tsc_hz = amd64_hyperv_tsc_hz();
        if (tsc_hz)
            source = "Hyper-V TSC frequency MSR";
    }
    if (!tsc_hz && amd64_hyperv_reference_available()) {
        uint64_t before = amd64_hyperv_reference(), start = hal_counter(), end = before;
        /* 20 ms against the partition's 100 ns reference counter. */
        for (unsigned attempt = 0; attempt < 10000000 && end - before < 200000; ++attempt)
            end = amd64_hyperv_reference();
        uint64_t elapsed = hal_counter() - start;
        if (end - before >= 200000) {
            tsc_hz = amd64_calibration_hz(elapsed, 10000000, end - before);
            if (tsc_hz)
                source = "Hyper-V reference counter";
        }
    }
    if (!tsc_hz)
        tsc_hz = amd64_calibrate_tsc(bi, &source);
    if (!tsc_hz)
        hal_panic("TSC calibration failed: CPUID, PIT, ACPI PM timer and HPET unavailable/gated");
    tsc_source = source;
    LOGI("TSC frequency=%llu Hz source=%s", (unsigned long long)tsc_hz, source);
}

void amd64_time_verify_cpu(void)
{
    uint32_t a, b, c, d;
    amd64_cpuid(0x80000000, 0, &a, &b, &c, &d);
    bool supported = a >= 0x80000007;
    if (supported)
        amd64_cpuid(0x80000007, 0, &a, &b, &c, &d);
    if ((supported && (d & (1u << 8)) != 0) != invariant_tsc)
        hal_panic("inconsistent invariant TSC capability across CPUs");
}

/* A complete request is one atomic word. There is no partially published
 * state or sender lock for a management CPU (or nested panic) to wait on. */
static uint64_t shutdown_mailbox;
bool hal_shutdown_requested(enum hal_shutdown_kind *kind, int *code)
{
    uint64_t request = __atomic_load_n(&shutdown_mailbox, __ATOMIC_ACQUIRE);
    if (!request)
        return false;
    *kind = (enum hal_shutdown_kind)((uint32_t)request - 1);
    *code = (int)(uint32_t)(request >> 32);
    return true;
}

void hal_input_poll(void)
{
#if !defined(IA64VMM_AMD64_HOST_ONLY)
    amd64_hyperv_keyboard_poll();
#endif
}

void hal_shutdown(enum hal_shutdown_kind kind, int exit_code)
{
    if (amd64_cpus && hal_cpu_index() != 0) {
        uint64_t expected = 0;
        uint64_t request = ((uint64_t)(uint32_t)exit_code << 32) | ((unsigned)kind + 1);
        __atomic_compare_exchange_n(&shutdown_mailbox, &expected, request, false, __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED);
        hal_cpu_request(0, HAL_CPU_SHUTDOWN);
        __asm__ volatile("cli" ::: "memory");
        __atomic_store_n(&amd64_current_cpu()->stopped, 1, __ATOMIC_RELEASE);
        for (;;)
            __asm__ volatile("hlt");
    }
    if (!amd64_smp_stop()) {
        kind = HAL_SHUTDOWN_HALT;
        exit_code = VMM_EXIT_FAILURE;
    }
    log_drain();
    log_flush_guest_text();
    amd64_final_summary(exit_code, force_halt || exit_code != 0 || amd64_pause_enabled());
    __asm__ volatile("cli" ::: "memory");
    /* Keep the guest's final GOP image on successful shutdown (P-03).
     * Reclaim the display for diagnostics on failures. */
    if (exit_code == VMM_EXIT_SUCCESS)
        vmm_log_fb_enable(false);
    else
        fb_console_reclaim();
    log_flush_guest_text();
    LOGI("VMM shutdown kind=%u exit_code=%d", (unsigned)kind, exit_code);
#if !defined(IA64VMM_AMD64_HOST_ONLY)
    if (exit_code == VMM_EXIT_SUCCESS && !log_in_panic()) {
        plog_poll(true);
        if (plog_save_ok())
            log_media_clean();
    } else
        plog_failure();
    amd64_plog_nvme_shutdown();
    amd64_plog_ahci_shutdown(amd64_power_removed(kind, force_halt, debug_exit));
    amd64_plog_ehci_shutdown();
    amd64_hyperv_keyboard_stop();
    amd64_plog_hyperv_shutdown();
    amd64_vmbus_stop();
#endif
    if (!force_halt && debug_exit)
        hal_port_out(0xf4, (uint32_t)exit_code, 4);
    if (!force_halt && !debug_exit) {
        if (kind == HAL_SHUTDOWN_POWER_OFF)
            amd64_acpi_poweroff();
        if (kind == HAL_SHUTDOWN_RESET) {
            hal_port_out(0xcf9, 2, 1);
            hal_port_out(0xcf9, 6, 1);
            for (unsigned i = 0; i < 100000; ++i)
                if (!(hal_port_in(0x64, 1) & 2)) {
                    hal_port_out(0x64, 0xfe, 1);
                    break;
                }
        }
    }
    LOGI("VMM halted exit_code=%d", exit_code);
    for (;;)
        __asm__ volatile("cli; hlt");
}
void hal_panic(const char *msg)
{
    __asm__ volatile("cli" ::: "memory");
    log_panic_begin();
    if (!amd64_cpus || hal_cpu_index() == 0) {
        amd64_smp_stop();
        fb_console_reclaim();
    }
    uint64_t cr0, cr2, cr3, cr4, rsp, rbp, flags;
    __asm__ volatile("mov %%cr0,%0; mov %%cr2,%1; mov %%cr3,%2; mov %%cr4,%3"
                     : "=r"(cr0), "=r"(cr2), "=r"(cr3), "=r"(cr4));
    __asm__ volatile("mov %%rsp,%0; mov %%rbp,%1; pushfq; pop %2"
                     : "=r"(rsp), "=r"(rbp), "=r"(flags));
    LOGE("panic: %s", msg);
    LOGE("host CR0=%016llx CR2=%016llx CR3=%016llx CR4=%016llx", (unsigned long long)cr0,
         (unsigned long long)cr2, (unsigned long long)cr3, (unsigned long long)cr4);
    LOGE("host RSP=%016llx RBP=%016llx RFLAGS=%016llx", (unsigned long long)rsp,
         (unsigned long long)rbp, (unsigned long long)flags);
    hal_shutdown(HAL_SHUTDOWN_HALT, VMM_EXIT_FAILURE);
}

/* Interrupt guard: preserve IF rather than unconditionally enabling IRQs. */
uint64_t hal_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags)::"memory");
    return flags;
}
void hal_irq_restore(uint64_t flags)
{
    if (flags & (UINT64_C(1) << 9))
        __asm__ volatile("sti" ::: "memory");
}
