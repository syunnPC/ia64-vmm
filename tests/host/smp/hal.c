/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The threaded hosted HAL with real threads, as the host/amd64 foundation
 * tests check host/amd64/smp.c per CPU. The HAL
 * mode is process-wide, so this is its own binary; the panic case runs this
 * binary again with the argument --panic-child. Linked with --wrap=syscall:
 * the lost-wakeup test places its request between the receiver's mailbox
 * check and the FUTEX_WAIT that follows it. Arguments select tests (default:
 * all); each test creates the CPU threads it uses on their first start.
 * IA64VMM_HOSTED_IDLE selects how idle CPUs wait (halted or monitoring,
 * ia64vmm/host_wake.h); the Makefile runs both.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <pthread.h>
#include <unistd.h>
#include "ia64vmm/hal.h"
#include "core/vm.h"
#include "tests/hal/hal_linux.h"
#include "ia64vmm/host_wake.h"

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#include "watchdog.h"

#define VCPUS 4u
#define HELPERS 4u
#define CPUS (1u + VCPUS + HELPERS)
#define MS UINT64_C(1000000) /* hosted hal_counter() ticks are nanoseconds */

static uint64_t now(void)
{
    return hal_counter();
}
static void relax(void)
{
    __builtin_ia32_pause();
}
#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
/* The process alarm bounds a stuck handshake; scheduling latency is not
 * part of the HAL contract. */
#define WAIT_FOR(cond)                                                                             \
    do {                                                                                           \
        while (!(cond)) {                                                                          \
            relax();                                                                               \
            sched_yield();                                                                         \
        }                                                                                          \
        watchdog_progress();                                                                       \
    } while (0)

/* Scheduler state of one thread of this process: 'S' while it sleeps in
 * FUTEX_WAIT. */
static char thread_state(pid_t tid)
{
    char path[64], text[512];
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int)tid);
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    CHECK(n > 0);
    text[n] = 0;
    const char *end = strrchr(text, ')');
    CHECK(end && end[1] == ' ');
    return end[2];
}
static void wait_sleeping(pid_t tid)
{
    while (thread_state(tid) != 'S')
        sched_yield();
}

/* --- syscall interposition for the lost-wakeup window ------------------ */
/* Freeze only the pre-wait clock read. Even if this thread is preempted
 * past its deadline, the HAL must exercise the real absolute futex wait.
 * The two barriers put CPU 0's far timer between each worker's timer arm
 * and its deadline use, making a shared timer deterministic to detect. */
static pthread_barrier_t timer_barrier;
static _Thread_local bool timer_check, timer_frozen;
static _Thread_local struct timespec timer_origin;
static _Thread_local uint64_t timer_expected;
static _Thread_local unsigned timer_waits;
int __real_clock_gettime(clockid_t clock, struct timespec *ts);
int __wrap_clock_gettime(clockid_t clock, struct timespec *ts);
int __wrap_clock_gettime(clockid_t clock, struct timespec *ts)
{
    if (clock == CLOCK_MONOTONIC && timer_frozen) {
        int r = pthread_barrier_wait(&timer_barrier);
        CHECK(!r || r == PTHREAD_BARRIER_SERIAL_THREAD);
        r = pthread_barrier_wait(&timer_barrier);
        CHECK(!r || r == PTHREAD_BARRIER_SERIAL_THREAD);
        /* All hal_cpu_start calls returned before management joined the
         * barrier, so even a late startup wake is now safe to drain. */
        CHECK(!(hal_cpu_poll() & ~HAL_CPU_WAKE));
        *ts = timer_origin;
        timer_frozen = false;
        return 0;
    }
    return __real_clock_gettime(clock, ts);
}

long __real_syscall(long number, ...);
static void (*futex_wait_hook)(void);
static void (*startup_wake_hook)(void);
static _Thread_local bool stop_idle_check;
static unsigned hook_cpu = UINT32_MAX;
long __wrap_syscall(long number, ...)
{
    va_list ap;
    long a[6];
    va_start(ap, number);
    for (unsigned i = 0; i < 6; ++i)
        a[i] = va_arg(ap, long);
    va_end(ap);
    if (number == SYS_futex && ((int)a[1] & FUTEX_CMD_MASK) == FUTEX_WAIT_BITSET &&
        hal_cpu_index() == LOAD(&hook_cpu))
        futex_wait_hook();
    if (number == SYS_futex && ((int)a[1] & FUTEX_CMD_MASK) == FUTEX_WAIT_BITSET)
        CHECK(!stop_idle_check);
    bool timed =
        timer_check && number == SYS_futex && ((int)a[1] & FUTEX_CMD_MASK) == FUTEX_WAIT_BITSET;
    if (timed) {
        const struct timespec *ts = (const struct timespec *)a[3];
        CHECK((int)a[1] == FUTEX_WAIT_BITSET_PRIVATE && ts &&
              (uint32_t)a[5] == FUTEX_BITSET_MATCH_ANY);
        CHECK((uint64_t)ts->tv_sec * UINT64_C(1000000000) + (uint64_t)ts->tv_nsec ==
              timer_expected);
    }
    long result = __real_syscall(number, a[0], a[1], a[2], a[3], a[4], a[5]);
    if (number == SYS_futex && (int)a[1] == FUTEX_WAKE_PRIVATE && hal_cpu_index() == 0 &&
        startup_wake_hook)
        startup_wake_hook();
    if (timed) {
        CHECK(result == -1 && errno == ETIMEDOUT);
        ++timer_waits;
    }
    return result;
}

/* --- configuration ----------------------------------------------------- */
static void configuration_test(void)
{
    CHECK(hal_cpu_index() == 0);
    CHECK(hal_vcpu_count() == VCPUS);
    for (unsigned i = 0; i < VCPUS; ++i)
        CHECK(hal_vcpu_cpu(i) == i + 1);
    CHECK(hal_vcpu_cpu(VCPUS) == UINT32_MAX && hal_vcpu_cpu(UINT32_MAX) == UINT32_MAX);
    CHECK(hal_helper_count() == HELPERS);
    for (unsigned i = 0; i < HELPERS; ++i)
        CHECK(hal_helper_cpu(i) == 1 + VCPUS + i && hal_helper_rank(i) == 1);
    CHECK(hal_helper_cpu(HELPERS) == UINT32_MAX && hal_helper_rank(HELPERS) == 0);
}

/* --- per-CPU mailbox delivery (amd64_ipi_selftest per thread) ----------- */
#define PROBE_BIT (1u << 17)
struct probe {
    unsigned index, done, startup_seen;
    uint64_t sample;
};
static struct probe *starting_probe;
static unsigned starting_cpu;
/* The start callback may run before hal_cpu_start publishes its wake.
 * After the real futex wake, hold management until the callback consumes
 * the startup mail, including when its CPU thread already existed. The HAL
 * contract does not reserve HAL_CPU_WAKE for the request timed below. */
static void startup_wake(void)
{
    startup_wake_hook = NULL;
    WAIT_FOR(LOAD(&starting_probe->done));
    hal_cpu_request(starting_cpu, HAL_CPU_WAKE);
    WAIT_FOR(LOAD(&starting_probe->startup_seen));
}
static void probe(void *arg)
{
    struct probe *p = arg;
    p->index = hal_cpu_index();
    STORE(&p->done, 1);
    /* Management requests only after this readiness handshake. */
    for (;;) {
        unsigned bits = hal_cpu_poll();
        p->sample = now();
        if (bits & HAL_CPU_WAKE)
            STORE(&p->startup_seen, 1);
        if (bits & PROBE_BIT)
            break;
        hal_idle_until(UINT64_MAX);
    }
    STORE(&p->done, 2);
}
/* Callbacks run through this trampoline, which records each CPU's thread
 * and checks that one thread runs every callback of a CPU. Refusal checks
 * call hal_cpu_start directly. */
static pid_t cpu_tid[CPUS];
static struct {
    hal_cpu_work_fn fn;
    void *arg;
} slot[CPUS];
static void trampoline(void *arg)
{
    unsigned cpu = (unsigned)(uintptr_t)arg;
    pid_t tid = gettid(), seen = LOAD(&cpu_tid[cpu]);
    CHECK(!seen || seen == tid);
    STORE(&cpu_tid[cpu], tid);
    slot[cpu].fn(slot[cpu].arg);
}
static bool start(unsigned cpu, hal_cpu_work_fn fn, void *arg)
{
    CHECK(cpu < CPUS && !hal_cpu_busy(cpu));
    slot[cpu].fn = fn;
    slot[cpu].arg = arg;
    return hal_cpu_start(cpu, trampoline, (void *)(uintptr_t)cpu);
}
/* Management context: idle on CPU 0's mailbox until cpu is no longer busy;
 * completion requests HAL_CPU_WAKE on CPU 0. */
static void wait_idle(unsigned cpu)
{
    while (hal_cpu_busy(cpu)) {
        hal_cpu_poll();
        if (!hal_cpu_busy(cpu))
            break;
        hal_idle_until(UINT64_MAX);
    }
}
/* Every completion has also delivered its HAL_CPU_WAKE to CPU 0 (each CPU
 * thread is back asleep in its loop), and CPU 0's mailbox is drained. */
static void quiesce(void)
{
    for (unsigned cpu = 1; cpu < CPUS; ++cpu) {
        wait_idle(cpu);
        if (LOAD(&cpu_tid[cpu]))
            wait_sleeping(cpu_tid[cpu]);
    }
    hal_cpu_poll();
}
static void ipi_test(void)
{
    quiesce();
    for (unsigned cpu = 1; cpu < CPUS; ++cpu) {
        struct probe p = {0};
        starting_probe = &p;
        starting_cpu = cpu;
        startup_wake_hook = startup_wake;
        CHECK(start(cpu, probe, &p));
        startup_wake_hook = NULL;
        WAIT_FOR(LOAD(&p.done) >= 1);
        CHECK(p.index == cpu);
        uint64_t before = now();
        hal_cpu_request(cpu, PROBE_BIT);
        WAIT_FOR(LOAD(&p.done) == 2);
        wait_idle(cpu);
        uint64_t after = now();
        CHECK(p.sample >= before && p.sample <= after);
        for (unsigned j = 1; j < cpu; ++j)
            CHECK(cpu_tid[j] != cpu_tid[cpu]);
    }
}

/* --- start, busy, refusal ---------------------------------------------- */
struct holder {
    unsigned started, release, nested;
};
static void noop(void *arg)
{
    (void)arg;
}
static void hold(void *arg)
{
    struct holder *h = arg;
    /* Only CPU 0 may start callbacks. */
    h->nested = hal_cpu_start(VCPUS + 1, noop, NULL) + 1;
    STORE(&h->started, 1);
    while (!LOAD(&h->release)) {
        hal_cpu_poll();
        if (LOAD(&h->release))
            break;
        hal_idle_until(UINT64_MAX);
    }
}
static void start_test(void)
{
    struct holder h = {0}, other = {0};
    CHECK(!hal_cpu_start(0, hold, &h));
    CHECK(!hal_cpu_start(CPUS, hold, &h));
    CHECK(!hal_cpu_start(UINT32_MAX, hold, &h));
    CHECK(!hal_cpu_start(1, NULL, &h));
    CHECK(!hal_cpu_busy(0) && !hal_cpu_busy(CPUS) && !hal_cpu_busy(UINT32_MAX));
    CHECK(start(1, hold, &h));
    CHECK(hal_cpu_busy(1));
    CHECK(!hal_cpu_start(1, hold, &other));
    WAIT_FOR(LOAD(&h.started));
    CHECK(h.nested == 1);
    CHECK(hal_cpu_busy(1));
    STORE(&h.release, 1);
    hal_cpu_request(1, HAL_CPU_WAKE);
    wait_idle(1);
    CHECK(!LOAD(&other.started));
    /* Out-of-range requests are ignored; CPU 0's mailbox stays empty. */
    quiesce();
    hal_cpu_request(CPUS, HAL_CPU_WAKE | HAL_CPU_STOP);
    hal_cpu_request(UINT32_MAX, HAL_CPU_WAKE | HAL_CPU_STOP);
    CHECK(hal_cpu_poll() == 0);
    /* The CPU starts again; the trampoline checks it is the same thread. */
    STORE(&other.release, 1);
    CHECK(start(1, hold, &other));
    WAIT_FOR(LOAD(&other.started));
    wait_idle(1);
}

/* --- completion wakes CPU 0 -------------------------------------------- */
static void finish_when_main_sleeps(void *arg)
{
    (void)arg;
    wait_sleeping(getpid());
}
static void completion_test(void)
{
    quiesce();
    for (unsigned round = 0; round < 20; ++round) {
        CHECK(start(1, finish_when_main_sleeps, NULL));
        hal_idle_until(UINT64_MAX);
        while (!(hal_cpu_poll() & HAL_CPU_WAKE))
            hal_idle_until(UINT64_MAX);
        CHECK(!hal_cpu_busy(1));
        watchdog_progress();
    }
}

/* --- per-thread timers (amd64_timer_selftest per thread) ---------------- */
static unsigned timers_done;
static void timer_selftest(void *arg)
{
    (void)arg;
    static const unsigned delays[] = {1, 10, 100};
    hal_cpu_poll(); /* consume the callback's startup wake */
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(!clock_gettime(CLOCK_MONOTONIC, &timer_origin));
        uint64_t origin =
            (uint64_t)timer_origin.tv_sec * UINT64_C(1000000000) + (uint64_t)timer_origin.tv_nsec;
        timer_expected = origin + delays[i] * MS;
        timer_waits = 0;
        timer_check = timer_frozen = true;
        hal_idle_until(timer_expected);
        timer_check = timer_frozen = false;
        /* Exact deadline and actual ETIMEDOUT above distinguish a timer
         * error from Linux leaving a ready thread descheduled. */
        CHECK(timer_waits == 1 && now() >= timer_expected);
    }
    __atomic_fetch_add(&timers_done, 1, __ATOMIC_ACQ_REL);
}
static void timer_test(void)
{
    CHECK(!pthread_barrier_init(&timer_barrier, NULL, 5));
    for (unsigned round = 0; round < 5; ++round) {
        STORE(&timers_done, 0);
        for (unsigned cpu = 3; cpu <= 6; ++cpu)
            CHECK(start(cpu, timer_selftest, NULL));
        for (unsigned step = 0; step < 3; ++step) {
            int r = pthread_barrier_wait(&timer_barrier);
            CHECK(!r || r == PTHREAD_BARRIER_SERIAL_THREAD);
            watchdog_progress();
            hal_timer_arm(now() + 3000 * MS);
            r = pthread_barrier_wait(&timer_barrier);
            CHECK(!r || r == PTHREAD_BARRIER_SERIAL_THREAD);
        }
        hal_timer_cancel();
        for (unsigned cpu = 3; cpu <= 6; ++cpu)
            wait_idle(cpu);
        CHECK(LOAD(&timers_done) == 4);
    }
    CHECK(!pthread_barrier_destroy(&timer_barrier));
}

/* --- 8 senders x 10^6 requests, no lost request ------------------------- */
#define SENDERS 8u
#define REQUESTS 1000000u
#define SENDER_BIT(k) (1u << (8 + (k)))
static struct {
    _Alignas(64) unsigned value;
} acks[SENDERS];
static unsigned count_ready;
static uint64_t count_total;
static void count_receiver(void *arg)
{
    (void)arg;
    uint64_t total = 0;
    STORE(&count_ready, 1);
    while (total < (uint64_t)SENDERS * REQUESTS) {
        hal_idle_until(UINT64_MAX);
        unsigned bits = hal_cpu_poll();
        for (unsigned k = 0; k < SENDERS; ++k)
            if (bits & SENDER_BIT(k)) {
                STORE(&acks[k].value, acks[k].value + 1);
                ++total;
                if (!(total % 4096))
                    watchdog_progress();
            }
    }
    count_total = total;
}
static void *count_sender(void *arg)
{
    unsigned k = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < REQUESTS; ++i) {
        for (unsigned spins = 0; LOAD(&acks[k].value) != i; ++spins) {
            if (spins & 255)
                relax();
            else
                sched_yield();
        }
        hal_cpu_request(1, SENDER_BIT(k));
    }
    return NULL;
}
static double count_seconds, race_seconds;
static void count_test(void)
{
    uint64_t begin = now();
    CHECK(start(1, count_receiver, NULL));
    /* cpu_thread drains startup mail before invoking its callback. Send
     * application bits only once the receiver owns the mailbox. */
    WAIT_FOR(LOAD(&count_ready));
    pthread_t threads[SENDERS];
    for (unsigned k = 0; k < SENDERS; ++k)
        CHECK(!pthread_create(&threads[k], NULL, count_sender, (void *)(uintptr_t)k));
    for (unsigned k = 0; k < SENDERS; ++k)
        CHECK(!pthread_join(threads[k], NULL));
    wait_idle(1);
    CHECK(count_total == (uint64_t)SENDERS * REQUESTS);
    for (unsigned k = 0; k < SENDERS; ++k)
        CHECK(acks[k].value == REQUESTS);
    count_seconds = (double)(now() - begin) / 1e9;
}

/* --- the lost-wakeup race, 10^5 rounds ---------------------------------- */
#define ROUNDS 100000u
#define RACE_CPU 2u
#define RACE_BIT (1u << 16)
/* Round r's request lands (0) before the receiver's mailbox check, (1)
 * between that check and its FUTEX_WAIT, (2) while it sleeps in it, or,
 * for a monitoring receiver, (3) as it arms its monitor. */
enum { BEFORE_CHECK, IN_WINDOW, ASLEEP, AT_ARM };
static struct {
    unsigned round, mode, window, injected, acked;
    pid_t tid;
} race;
static void race_window(void)
{
    unsigned round = LOAD(&race.round), mode = LOAD(&race.mode);
    if (mode == AT_ARM)
        return;
    STORE(&race.window, round);
    if (mode == IN_WINDOW)
        while (LOAD(&race.injected) != round)
            relax();
}
void hal_linux_monitor_event(void)
{
    if (hal_cpu_index() != LOAD(&hook_cpu) || LOAD(&race.mode) != AT_ARM)
        return;
    unsigned round = LOAD(&race.round);
    STORE(&race.window, round);
    while (LOAD(&race.injected) != round)
        relax();
}
static void race_receiver(void *arg)
{
    (void)arg;
    STORE(&race.tid, gettid());
    for (unsigned round = 1; round <= ROUNDS; ++round) {
        WAIT_FOR(LOAD(&race.round) == round);
        for (;;) {
            hal_idle_until(UINT64_MAX);
            if (hal_cpu_poll() & RACE_BIT)
                break;
        }
        STORE(&race.acked, round);
    }
}
static unsigned race_counts[4];
static void race_test(void)
{
    uint64_t begin = now();
    unsigned modes = hal_linux_idle_state() == HOST_IDLE_MONITOR ? 4 : 3;
    futex_wait_hook = race_window;
    STORE(&hook_cpu, RACE_CPU);
    CHECK(start(RACE_CPU, race_receiver, NULL));
    WAIT_FOR(LOAD(&race.tid));
    for (unsigned round = 1; round <= ROUNDS; ++round) {
        unsigned mode = round % modes;
        watchdog_mode = (sig_atomic_t)mode;
        watchdog_round = (sig_atomic_t)round;
        STORE(&race.mode, mode);
        if (mode == BEFORE_CHECK)
            hal_cpu_request(RACE_CPU, RACE_BIT);
        STORE(&race.round, round);
        if (mode != BEFORE_CHECK) {
            WAIT_FOR(LOAD(&race.window) == round);
            if (mode == ASLEEP)
                wait_sleeping(LOAD(&race.tid));
            hal_cpu_request(RACE_CPU, RACE_BIT);
            STORE(&race.injected, round);
        }
        WAIT_FOR(LOAD(&race.acked) == round);
        ++race_counts[mode];
    }
    wait_idle(RACE_CPU);
    STORE(&hook_cpu, UINT32_MAX);
    race_seconds = (double)(now() - begin) / 1e9;
}

/* --- interrupts: only a halted CPU takes one -------------------------------- */
#define WAKE_BIT (1u << 18)
struct waker {
    unsigned spinning, release, woken;
};
static void spin_until_released(void *arg)
{
    struct waker *w = arg;
    hal_cpu_poll();
    STORE(&w->spinning, 1);
    while (!LOAD(&w->release))
        relax();
    CHECK(hal_cpu_poll() & WAKE_BIT);
}
static void sleep_until_woken(void *arg)
{
    struct waker *w = arg;
    hal_cpu_poll();
    while (!(hal_cpu_poll() & WAKE_BIT))
        hal_idle_until(UINT64_MAX);
    STORE(&w->woken, 1);
}
static void interrupt_test(void)
{
    const unsigned cpu = VCPUS + 2;
    bool halting = hal_linux_idle_state() == HOST_IDLE_HALT;
    for (unsigned round = 0; round < 100; ++round) {
        /* A running CPU sees the bit at its next poll, uninterrupted. */
        struct waker running = {0};
        CHECK(start(cpu, spin_until_released, &running));
        WAIT_FOR(LOAD(&running.spinning));
        unsigned before = hal_linux_interrupts(cpu);
        hal_cpu_request(cpu, WAKE_BIT);
        CHECK(hal_linux_interrupts(cpu) == before);
        STORE(&running.release, 1);
        wait_idle(cpu);
        /* A sleeping CPU is interrupted only when halted; a monitoring one
         * wakes from the store. */
        struct waker idle = {0};
        CHECK(start(cpu, sleep_until_woken, &idle));
        wait_sleeping(LOAD(&cpu_tid[cpu]));
        before = hal_linux_interrupts(cpu);
        hal_cpu_request(cpu, WAKE_BIT);
        unsigned sent = hal_linux_interrupts(cpu) - before;
        CHECK(sent == (halting ? 1u : 0u));
        wait_idle(cpu);
        CHECK(LOAD(&idle.woken));
        watchdog_progress();
    }
}

/* --- STOP ---------------------------------------------------------------- */
struct stopper {
    unsigned started, stopped;
};
static void until_stop(void *arg)
{
    struct stopper *s = arg;
    STORE(&s->started, 1);
    while (!(hal_cpu_poll() & HAL_CPU_STOP)) {
        hal_idle_until(UINT64_MAX);
    }
    /* The consumed request leaves the stop flag: idle no longer sleeps. */
    stop_idle_check = true;
    hal_idle_until(UINT64_MAX);
    stop_idle_check = false;
    STORE(&s->stopped, 1);
}
static void stop_test(void)
{
    struct stopper s = {0};
    unsigned running = VCPUS + 3, idle = VCPUS + 4;
    CHECK(start(running, until_stop, &s));
    /* As on bare metal, a CPU that sees STOP before it picks up its
     * callback never runs it: request only once the callback runs. */
    WAIT_FOR(LOAD(&s.started));
    hal_cpu_request(running, HAL_CPU_STOP);
    wait_idle(running);
    CHECK(LOAD(&s.stopped));
    CHECK(!hal_cpu_start(running, noop, NULL));
    /* A CPU idle in its loop (or never started) never starts after STOP. */
    hal_cpu_request(idle, HAL_CPU_STOP);
    CHECK(!hal_cpu_start(idle, noop, NULL));
    CHECK(!hal_cpu_busy(running) && !hal_cpu_busy(idle));
    /* The other CPUs are unaffected. */
    CHECK(start(1, noop, NULL));
    wait_idle(1);
}

/* --- hal_panic from non-main threads, in a subprocess ------------------- */
static unsigned panic_go;
static void panic_now(void *arg)
{
    (void)arg;
    while (!LOAD(&panic_go))
        relax();
    hal_panic("concurrent panic");
}
static int panic_child(void)
{
    CHECK(!hal_linux_configure(true, VCPUS, 0, NULL, 0));
    for (unsigned cpu = 1; cpu <= VCPUS; ++cpu)
        CHECK(hal_cpu_start(cpu, panic_now, NULL));
    STORE(&panic_go, 1);
    for (;;)
        hal_idle_until(UINT64_MAX);
}
static void panic_test(void)
{
    const char *self = "/proc/self/exe";
    for (unsigned round = 0; round < 10; ++round) {
        int pipe_fds[2];
        CHECK(!pipe(pipe_fds));
        posix_spawn_file_actions_t actions;
        CHECK(!posix_spawn_file_actions_init(&actions));
        CHECK(!posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO));
        CHECK(!posix_spawn_file_actions_addclose(&actions, pipe_fds[0]));
        char *argv[] = {(char *)self, "--panic-child", NULL};
        extern char **environ;
        pid_t pid;
        CHECK(!posix_spawn(&pid, self, &actions, NULL, argv, environ));
        posix_spawn_file_actions_destroy(&actions);
        close(pipe_fds[1]);
        char output[65536];
        size_t used = 0;
        for (;;) {
            ssize_t n = read(pipe_fds[0], output + used, sizeof(output) - 1 - used);
            if (n < 0 && errno == EINTR)
                continue;
            CHECK(n >= 0);
            if (!n)
                break;
            used += (size_t)n;
            CHECK(used < sizeof(output) - 1);
        }
        close(pipe_fds[0]);
        output[used] = 0;
        int status;
        CHECK(waitpid(pid, &status, 0) == pid);
        unsigned messages = 0;
        for (const char *p = output; (p = strstr(p, "[panic] concurrent panic\n")); ++p)
            ++messages;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != VMM_EXIT_FAILURE || messages != 1 ||
            used != strlen("[panic] concurrent panic\n")) {
            fprintf(stderr, "panic child: status=%#x messages=%u output:\n%s", status, messages,
                    output);
            CHECK(false);
        }
        watchdog_progress();
    }
}

static const struct {
    const char *name;
    void (*run)(void);
} tests[] = {
    {"panic", panic_test}, {"start", start_test},
    {"ipi", ipi_test},     {"completion", completion_test},
    {"timer", timer_test}, {"count", count_test},
    {"race", race_test},   {"interrupt", interrupt_test},
    {"stop", stop_test},
};
int main(int argc, char **argv)
{
    const char *cap = getenv("HAL_SMP_ALARM");
    unsigned seconds = cap && (unsigned)atoi(cap) ? (unsigned)atoi(cap) : 300;
    watchdog_start("panic", seconds);
    /* Liveness only: a broken barrier or lost wake must end. */
    if (argc == 2 && !strcmp(argv[1], "--panic-child"))
        return panic_child();
    bool run[IA64VMM_ARRAY_SIZE(tests)];
    for (unsigned t = 0; t < IA64VMM_ARRAY_SIZE(tests); ++t) {
        run[t] = argc == 1;
        for (int i = 1; i < argc; ++i)
            run[t] |= !strcmp(argv[i], tests[t].name);
    }
    for (int i = 1; i < argc; ++i) {
        bool known = false;
        for (unsigned t = 0; t < IA64VMM_ARRAY_SIZE(tests); ++t)
            known |= !strcmp(argv[i], tests[t].name);
        CHECK(known);
    }
    /* Before this process has threads: the panic subprocesses. */
    if (run[0])
        panic_test();
    CHECK(!hal_linux_configure(true, VCPUS, HELPERS, NULL, 0));
    configuration_test();
    for (unsigned t = 1; t < IA64VMM_ARRAY_SIZE(tests); ++t)
        if (run[t]) {
            watchdog_start(tests[t].name, seconds);
            tests[t].run();
        }
    alarm(0);
    double load[3];
    CHECK(getloadavg(load, 3) == 3);
    if (argc == 1)
        printf(
            "test-hal-smp: %u CPUs, mailboxes, start/busy/refusal, completion wake, per-thread timers, "
            "%ux%u requests (%.2fs), %u lost-wakeup rounds %u/%u/%u/%u before/window/asleep/arm (%.2fs), "
            "interrupts, STOP, one concurrent panic passed (idle %s); load=%.2f %.2f %.2f\n",
            CPUS, SENDERS, REQUESTS, count_seconds, ROUNDS, race_counts[BEFORE_CHECK],
            race_counts[IN_WINDOW], race_counts[ASLEEP], race_counts[AT_ARM], race_seconds,
            hal_linux_idle_state() == HOST_IDLE_MONITOR ? "monitor" : "halt", load[0], load[1],
            load[2]);
    else
        for (int i = 1; i < argc; ++i)
            printf("test-hal-smp: %s passed\n", argv[i]);
    return 0;
}
