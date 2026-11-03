/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE /* sched_setaffinity, CPU_SET */
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "ia64vmm/bootinfo.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"
#include "ia64vmm/x86rm.h"
#include "core/log_internal.h"
#include "core/vm.h"
#include "tests/hal/hal_linux.h"
#include "ia64vmm/host_isa.h"
#include "ia64vmm/host_simd.h"
#include "ia64vmm/host_wake.h"

static pthread_once_t host_isa_once = PTHREAD_ONCE_INIT;
static unsigned detected_host_isa, common_host_isa, host_isa_mask = HOST_ISA_ALL,
                                                    common_host_simd = HOST_SIMD_ALL;
static void probe_host_isa(void)
{
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed))
        hal_panic("host ISA affinity query failed");
    struct host_isa_cpu_report reports[CPU_SETSIZE];
    unsigned count = 0;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed))
            continue;
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(cpu, &one);
        if (sched_setaffinity(0, sizeof(one), &one))
            hal_panic("host ISA CPU probe failed");
        reports[count++] = (struct host_isa_cpu_report){host_isa_probe(host_isa_cpuid), true};
        common_host_simd &= host_simd_probe(host_isa_cpuid, host_simd_xcr0);
    }
    if (sched_setaffinity(0, sizeof(allowed), &allowed))
        hal_panic("host ISA affinity restore failed");
    if (host_isa_resolve("", reports, count, &detected_host_isa) != 1)
        hal_panic("host ISA requires every CPU report");
    common_host_isa = detected_host_isa;
    const char *mask = getenv("IA64VMM_HOST_ISA_MASK");
    if (mask) {
        char cmdline[64];
        int n = snprintf(cmdline, sizeof(cmdline), "host_isa_mask=%s", mask);
        if (!*mask || strspn(mask, "0123456789") != strlen(mask) || n < 0 ||
            (size_t)n >= sizeof(cmdline) ||
            host_isa_resolve(cmdline, reports, count, &common_host_isa) != 1 ||
            !host_isa_parse_mask(cmdline, &host_isa_mask))
            hal_panic("invalid IA64VMM_HOST_ISA_MASK (decimal 0..15)");
    }
}
unsigned hal_host_isa(void)
{
    pthread_once(&host_isa_once, probe_host_isa);
    return common_host_isa;
}
unsigned hal_host_simd(void)
{
    pthread_once(&host_isa_once, probe_host_isa);
    return common_host_simd;
}
unsigned hal_linux_host_isa_mask(unsigned mask)
{
    pthread_once(&host_isa_once, probe_host_isa);
    unsigned old = host_isa_mask;
    host_isa_mask = mask & HOST_ISA_ALL;
    common_host_isa = detected_host_isa & host_isa_mask;
    return old;
}

void hal_checkpoint(const char *name)
{
    LOGI("checkpoint=%s (hosted: no pause)", name);
}
void hal_debug_write(const char *s, size_t n)
{
    while (n) {
        ssize_t written = write(STDERR_FILENO, s, n);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        s += written;
        n -= (size_t)written;
    }
}

void hal_panic(const char *msg)
{
    /* One message from any thread; a concurrent second panic
     * waits for the first to exit the process. A panic nested in this
     * thread's own shutdown exits at once instead of waiting for itself. */
    static unsigned panicking;
    static _Thread_local bool panic_owner;
    if (__atomic_exchange_n(&panicking, 1, __ATOMIC_ACQ_REL)) {
        if (panic_owner)
            _exit(VMM_EXIT_FAILURE);
        for (;;)
            pause();
    }
    panic_owner = true;
    hal_debug_write("[panic] ", 8);
    hal_debug_write(msg, strlen(msg));
    hal_debug_write("\n", 1);
    hal_shutdown(HAL_SHUTDOWN_HALT, VMM_EXIT_FAILURE);
}

void *hal_alloc_pages(size_t npages, unsigned flags)
{
    (void)flags;
    if (!npages || npages > SIZE_MAX / IA64VMM_PAGE_SIZE)
        return NULL;
    void *p = mmap(NULL, npages * IA64VMM_PAGE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    /* Anonymous mappings are zeroed even when HAL_PAGE_ZERO is absent. */
    return p == MAP_FAILED ? NULL : p;
}

void hal_free_pages(void *p, size_t npages)
{
    if (p && npages && npages <= SIZE_MAX / IA64VMM_PAGE_SIZE)
        if (munmap(p, npages * IA64VMM_PAGE_SIZE))
            hal_panic("munmap failed");
}

bool hal_memory_get_stats(struct hal_memory_stats *out)
{
    (void)out;
    return false;
}

static void code_protect(void *p, size_t npages, int prot)
{
    if (!npages || npages > SIZE_MAX / IA64VMM_PAGE_SIZE ||
        mprotect(p, npages * IA64VMM_PAGE_SIZE, prot))
        hal_panic("code page mprotect failed");
}

void hal_code_set_writable(void *p, size_t npages)
{
    code_protect(p, npages, PROT_READ | PROT_WRITE);
}

void hal_code_set_executable(void *p, size_t npages)
{
    code_protect(p, npages, PROT_READ | PROT_EXEC);
}

uint64_t hal_counter(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        hal_panic("clock_gettime failed");
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

uint64_t hal_counter_hz(void)
{
    return UINT64_C(1000000000);
}

/* Host CPUs; single-thread mode uses CPU
 * 0's only. The mailbox protocol is the bare-metal one (ia64vmm/host_wake.h).
 * A halted CPU sleeps on its interrupt count, which only an interrupt (a
 * post that host_mailbox_post says must interrupt) advances. A monitoring
 * CPU sleeps on its mailbox word with the value it armed: a store after
 * arming changes it, and a store while armed also wakes the futex. */
struct linux_cpu {
    struct host_mailbox mailbox;
    unsigned interrupts, armed;
    unsigned busy, stop;
    hal_cpu_work_fn work;
    void *work_arg;
    bool created, pinned; /* created: CPU 0 only */
    unsigned pin;
};
static struct linux_cpu cpus[HAL_LINUX_MAX_CPUS];
static unsigned cpu_count = 1, vcpu_total = 1, helper_total;
static bool threaded;
static _Thread_local unsigned cpu_index;
static _Thread_local uint64_t timer_deadline = UINT64_MAX;
static _Thread_local unsigned armed_value;

/* IA64VMM_HOSTED_IDLE=halt (default) or monitor: how idle CPUs wait. */
static pthread_once_t idle_once = PTHREAD_ONCE_INIT;
static unsigned idle_state;
static void read_idle_mode(void)
{
    const char *mode = getenv("IA64VMM_HOSTED_IDLE");
    if (!mode || !strcmp(mode, "halt"))
        idle_state = HOST_IDLE_HALT;
    else if (!strcmp(mode, "monitor"))
        idle_state = HOST_IDLE_MONITOR;
    else
        hal_panic("invalid IA64VMM_HOSTED_IDLE (halt or monitor)");
}
unsigned hal_linux_idle_state(void)
{
    pthread_once(&idle_once, read_idle_mode);
    return idle_state;
}

static bool pin_thread(unsigned linux_cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(linux_cpu, &set);
    return !sched_setaffinity(0, sizeof(set), &set);
}

const char *hal_linux_configure(bool thread_mode, unsigned vcpus, unsigned helpers,
                                const unsigned *pins, unsigned pin_count)
{
    if (!vcpus || vcpus > IA64VMM_MAX_CPUS)
        return "vcpus must be 1..256";
    if (helpers > HAL_LINUX_MAX_HELPERS)
        return "helpers must be 0..64";
    if (!thread_mode && (vcpus != 1 || helpers))
        return "more than one host CPU needs threaded mode";
    /* boot_info.cpus[] describes every host CPU (host_cpu_count, host/linux/main.c). */
    if (thread_mode && 1 + vcpus + helpers > BOOT_MAX_CPUS)
        return "1 + vcpus + helpers exceeds the 256 host CPUs of boot_info (BOOT_MAX_CPUS)";
    /* Probe the whole permitted set before pinning the management thread
     * or starting pinned executors; their local affinities are subsets. */
    (void)hal_host_isa();
    unsigned count = thread_mode ? 1 + vcpus + helpers : 1;
    if (pins) {
        cpu_set_t allowed;
        if (pin_count != count)
            return "--pin needs one Linux CPU per host CPU";
        if (sched_getaffinity(0, sizeof(allowed), &allowed))
            return "cannot read the CPU affinity";
        for (unsigned i = 0; i < count; ++i)
            if (pins[i] >= CPU_SETSIZE || !CPU_ISSET(pins[i], &allowed))
                return "--pin names a Linux CPU outside this process's affinity";
        for (unsigned i = 0; i < count; ++i) {
            cpus[i].pinned = true;
            cpus[i].pin = pins[i];
        }
        if (!pin_thread(pins[0]))
            return "cannot pin the main thread";
    }
    threaded = thread_mode;
    cpu_count = count;
    vcpu_total = vcpus;
    helper_total = helpers;
    return NULL;
}

void hal_timer_arm(uint64_t deadline)
{
    timer_deadline = deadline;
}
void hal_timer_cancel(void)
{
    timer_deadline = UINT64_MAX;
}
unsigned hal_cpu_index(void)
{
    return cpu_index;
}
unsigned hal_vcpu_cpu(unsigned vcpu)
{
    if (!threaded)
        return vcpu ? UINT32_MAX : 0;
    return vcpu < vcpu_total ? vcpu + 1 : UINT32_MAX;
}
unsigned hal_vcpu_count(void)
{
    return vcpu_total;
}
unsigned hal_helper_count(void)
{
    return helper_total;
}
unsigned hal_helper_cpu(unsigned i)
{
    return i < helper_total ? 1 + vcpu_total + i : UINT32_MAX;
}
unsigned hal_helper_rank(unsigned i)
{
    return i < helper_total ? 1 : 0;
}

/* The loop of a non-zero CPU, as amd64_cpu_loop (host/amd64/smp.c). */
static void *cpu_thread(void *arg)
{
    struct linux_cpu *cpu = arg;
    cpu_index = (unsigned)(cpu - cpus);
    if (cpu->pinned && !pin_thread(cpu->pin))
        hal_panic("cannot pin a host CPU thread");
    for (;;) {
        if (__atomic_load_n(&cpu->stop, __ATOMIC_ACQUIRE)) {
            hal_timer_cancel();
            for (;;)
                pause();
        }
        hal_cpu_poll();
        hal_cpu_work_fn work = __atomic_exchange_n(&cpu->work, NULL, __ATOMIC_ACQ_REL);
        if (work) {
            work(cpu->work_arg);
            hal_timer_cancel();
            __atomic_store_n(&cpu->busy, 0, __ATOMIC_RELEASE);
            hal_cpu_request(0, HAL_CPU_WAKE);
        } else
            hal_idle_until(UINT64_MAX);
    }
}

bool hal_cpu_start(unsigned index, hal_cpu_work_fn work, void *arg)
{
    if (cpu_index != 0 || index == 0 || index >= cpu_count || !work ||
        __atomic_load_n(&cpus[index].stop, __ATOMIC_ACQUIRE))
        return false;
    struct linux_cpu *cpu = &cpus[index];
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&cpu->busy, &expected, 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return false;
    cpu->work_arg = arg;
    __atomic_store_n(&cpu->work, work, __ATOMIC_RELEASE);
    if (!cpu->created) {
        pthread_attr_t attr;
        pthread_t thread;
        if (!pthread_attr_init(&attr)) {
            cpu->created = !pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) &&
                           !pthread_create(&thread, &attr, cpu_thread, cpu);
            pthread_attr_destroy(&attr);
        }
        if (!cpu->created) {
            __atomic_store_n(&cpu->work, NULL, __ATOMIC_RELAXED);
            __atomic_store_n(&cpu->busy, 0, __ATOMIC_RELEASE);
            return false;
        }
    }
    hal_cpu_request(index, HAL_CPU_WAKE);
    return true;
}
bool hal_cpu_busy(unsigned index)
{
    return index < cpu_count && __atomic_load_n(&cpus[index].busy, __ATOMIC_ACQUIRE);
}
void hal_cpu_request(unsigned index, unsigned requests)
{
    if (index >= cpu_count)
        return;
    struct linux_cpu *cpu = &cpus[index];
    if (requests & HAL_CPU_STOP)
        __atomic_store_n(&cpu->stop, 1, __ATOMIC_RELEASE);
    bool interrupt = host_mailbox_post(&cpu->mailbox, requests);
    /* The store reaches an armed monitor: either the idler armed after it
     * (and its check sees the bits) or this load sees it armed. */
    if (__atomic_load_n(&cpu->armed, __ATOMIC_SEQ_CST))
        syscall(SYS_futex, &cpu->mailbox.requests, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    if (interrupt) {
        __atomic_fetch_add(&cpu->interrupts, 1, __ATOMIC_SEQ_CST);
        syscall(SYS_futex, &cpu->interrupts, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    }
}
unsigned hal_linux_interrupts(unsigned index)
{
    return index < cpu_count ? __atomic_load_n(&cpus[index].interrupts, __ATOMIC_ACQUIRE) : 0;
}
void hal_cpu_wake(unsigned index)
{
    hal_cpu_request(index, HAL_CPU_WAKE);
}
void hal_cpu_sample(unsigned index)
{
    (void)index;
}
bool hal_text_contains(uint64_t address)
{
    (void)address;
    return false;
}
void hal_cpu_interrupts(unsigned index, uint64_t *timer, uint64_t *ipi)
{
    (void)index;
    *timer = *ipi = 0;
}
unsigned hal_cpu_poll(void)
{
    return __atomic_exchange_n(&cpus[cpu_index].mailbox.requests, 0, __ATOMIC_ACQ_REL);
}
void hal_input_poll(void) {}
extern void hal_linux_monitor_event(void) __attribute__((weak));
static void monitor(const unsigned *word)
{
    if (hal_linux_monitor_event)
        hal_linux_monitor_event();
    __atomic_store_n(&cpus[cpu_index].armed, 1, __ATOMIC_SEQ_CST);
    armed_value = __atomic_load_n(word, __ATOMIC_SEQ_CST);
}
void hal_idle_until(uint64_t deadline)
{
    struct linux_cpu *cpu = &cpus[cpu_index];
    unsigned state = hal_linux_idle_state();
    hal_timer_arm(deadline);
    /* As interrupts masked from here: a later interrupt is still pending
     * when the CPU halts. */
    unsigned interrupts = __atomic_load_n(&cpu->interrupts, __ATOMIC_SEQ_CST);
    /* A stopped CPU never sleeps, as on bare metal (host/amd64/apic.c). */
    if (host_idle_enter(&cpu->mailbox, state, monitor) && timer_deadline > hal_counter() &&
        !__atomic_load_n(&cpu->stop, __ATOMIC_ACQUIRE)) {
        struct timespec ts = {
            .tv_sec = (time_t)(timer_deadline / UINT64_C(1000000000)),
            .tv_nsec = (long)(timer_deadline % UINT64_C(1000000000)),
        };
        /* FUTEX_WAIT_BITSET uses an absolute CLOCK_MONOTONIC deadline.
         * Testing the word and sleeping are atomic, including wake-before-wait. */
        bool monitoring = state == HOST_IDLE_MONITOR;
        int result = syscall(SYS_futex, monitoring ? &cpu->mailbox.requests : &cpu->interrupts,
                             FUTEX_WAIT_BITSET_PRIVATE, monitoring ? armed_value : interrupts,
                             deadline == UINT64_MAX ? NULL : &ts, NULL, FUTEX_BITSET_MATCH_ANY);
        if (result && errno != EINTR && errno != EAGAIN && errno != ETIMEDOUT)
            hal_panic("futex idle wait failed");
    }
    __atomic_store_n(&cpu->armed, 0, __ATOMIC_SEQ_CST);
    host_idle_leave(&cpu->mailbox);
    hal_timer_cancel();
}

uint32_t hal_port_in(uint16_t port, unsigned size)
{
    LOGD("port in %04x size=%u", (unsigned)port, size);
    return size == 1 ? UINT32_C(0xff) : size == 2 ? UINT32_C(0xffff) : UINT32_MAX;
}

void hal_port_out(uint16_t port, uint32_t value, unsigned size)
{
    LOGD("port out %04x size=%u value=%08x", (unsigned)port, size, value);
}

bool hal_shutdown_requested(enum hal_shutdown_kind *kind, int *code)
{
    (void)kind;
    (void)code;
    return false;
}

void hal_shutdown(enum hal_shutdown_kind kind, int exit_code)
{
    (void)kind;
    log_flush_guest_text();
    fflush(NULL);
    exit(exit_code);
}

uint64_t hal_wallclock_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) || ts.tv_sec <= 0)
        return 0;
    return (uint64_t)ts.tv_sec;
}

/* x86 real-mode container (ia64vmm/x86rm.h): the hosted HAL has no
 * hardware virtualization, so display=vbios reports "no hardware
 * virtualization" and the VMM continues as without it. */
const struct x86rm_info *hal_x86rm_info(void)
{
    static const struct x86rm_info info = {"hardware virtualization", {0, 0, 0}};
    return &info;
}
bool hal_x86rm_probe(const char **why)
{
    *why = "the hosted HAL has no hardware virtualization";
    return false;
}
bool hal_x86rm_create(const struct x86rm_config *cfg, struct x86rm **out, const char **why)
{
    (void)cfg;
    *out = NULL;
    return hal_x86rm_probe(why);
}
bool hal_x86rm_reclaim_create(const struct x86rm_config *cfg, struct x86rm **out, const char **why)
{
    (void)cfg;
    *out = NULL;
    *why = "the hosted HAL has no hardware virtualization";
    return false;
}
bool hal_x86rm_run(struct x86rm *rm, struct x86rm_regs *regs, struct x86rm_exit *exit)
{
    (void)rm;
    (void)regs;
    (void)exit;
    return false;
}
void hal_x86rm_destroy(struct x86rm *rm)
{
    (void)rm;
}
bool hal_console_vga_text(void)
{
    return false;
}

/* Unit tests have no hardware interrupt delivery. */
uint64_t hal_irq_save(void)
{
    return 0;
}
void hal_irq_restore(uint64_t flags)
{
    (void)flags;
}
