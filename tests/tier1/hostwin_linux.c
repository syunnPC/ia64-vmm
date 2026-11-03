/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Linux host-window test backend: PROT_NONE/MAP_NORESERVE reservations placed
 * with MAP_FIXED_NOREPLACE, memfd page aliases and SIGSEGV chaining (including
 * ASan under SAN=1 with allow_user_segv_handler=1).
 * Aliases use gm_direct_ptr(pa_page)'s backing allocation, so shadows share
 * the interpreter's RAM even when one allocation spans multiple GM_RAM regions.
 * Allocation and reservation changes require no active executor; shared
 * map/unmap/protect_ro counters are atomic for concurrent vCPUs.
 *
 * L isolated shadows need at most 2L + 1 VMAs; the poison page shares the
 * trailing PROT_NONE VMA. Splitting a merged mapping during unmap or mprotect
 * can fail with ENOMEM at vm.max_map_count. reserve() budgets the worst case
 * of 2 * HOSTWIN_MAX_SHADOWS + 1 VMAs per instance, plus process headroom.
 * unmap and protect_ro remain fallible.
 */
#define _GNU_SOURCE
#include "tests/tier1/hostwin_linux.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "ia64vmm/guest_mem.h"

#define PAGE 4096u
#define MAX_RAM 64u
#define MAX_MEMFDS 64u
#define MAX_RESERVATIONS 64u

/* hostwin_linux_alloc: [hva, hva + size) is offset 0.. of fd. */
struct memfd_range {
    uintptr_t hva;
    uint64_t size;
    int fd;
};
/* hostwin_linux_ram's guest ranges, only for its overlap refusal. */
struct ram_range {
    gpa_t base;
    uint64_t size;
};
struct reservation {
    uintptr_t base, end;
}; /* [base, end): slots + poison page */

static struct memfd_range memfds[MAX_MEMFDS];
static unsigned memfd_count;
static struct ram_range ram[MAX_RAM];
static unsigned ram_count;
static struct reservation reservations[MAX_RESERVATIONS];
static unsigned reservation_count;
static struct hostwin_linux_stats counters; /* maps .. last_errno: __atomic only */
static struct sigaction previous;
static int ctx_token; /* the hosted backend has one process-wide context */

/* ---- VMA accounting ---------------------------------------------------- */

static long read_max_map_count(void)
{
    char text[32];
    int fd = open("/proc/sys/vm/max_map_count", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    text[n] = 0;
    return strtol(text, NULL, 10);
}

static bool in_reservation(uintptr_t lo)
{
    for (unsigned i = 0; i < reservation_count; ++i)
        if (lo >= reservations[i].base && lo < reservations[i].end)
            return true;
    return false;
}

/* /proc/self/maps line by line through a stack buffer: no allocation, so
 * it also works with the process at its map limit, and executors failing
 * at the same time do not share it. A line is at most the 73-byte prefix
 * and a PATH_MAX path. */
static void count_vmas(uint64_t *all, uint64_t *ours)
{
    char buffer[8192];
    *all = *ours = 0;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, buffer + used, sizeof(buffer) - used);
        if (n <= 0)
            break;
        size_t end = used + (size_t)n, at = 0;
        while (at < end) {
            char *nl = memchr(buffer + at, '\n', end - at);
            if (!nl)
                break;
            ++*all;
            if (in_reservation((uintptr_t)strtoull(buffer + at, NULL, 16)))
                ++*ours;
            at = (size_t)(nl - buffer) + 1;
        }
        memmove(buffer, buffer + at, end - at);
        used = end - at;
        if (used == sizeof(buffer))
            break; /* no line is that long */
    }
    close(fd);
}

void hostwin_linux_get_stats(struct hostwin_linux_stats *out)
{
    *out = (struct hostwin_linux_stats){
        .maps = __atomic_load_n(&counters.maps, __ATOMIC_RELAXED),
        .unmaps = __atomic_load_n(&counters.unmaps, __ATOMIC_RELAXED),
        .protects = __atomic_load_n(&counters.protects, __ATOMIC_RELAXED),
        .failures = __atomic_load_n(&counters.failures, __ATOMIC_RELAXED),
        .last_errno = __atomic_load_n(&counters.last_errno, __ATOMIC_RELAXED),
    };
    out->max_map_count = read_max_map_count();
    out->reservations = reservation_count;
    count_vmas(&out->process_vmas, &out->reservation_vmas);
}

static long limit_override;
void hostwin_linux_set_map_limit(long limit)
{
    limit_override = limit;
}

static unsigned refused_width;
void hostwin_linux_refuse_width(unsigned w)
{
    __atomic_store_n(&refused_width, w, __ATOMIC_RELAXED);
}
long hostwin_linux_map_limit(void)
{
    long real = read_max_map_count();
    return limit_override > 0 && (real < 0 || limit_override < real) ? limit_override : real;
}

uint64_t hostwin_linux_vmas_per_reservation(void)
{
    return 2u * HOSTWIN_MAX_SHADOWS + 1;
}
uint64_t hostwin_linux_headroom(void)
{
    return 4096;
}

static void failure(const char *what, uintptr_t host_page)
{
    int error = errno;
    __atomic_store_n(&counters.last_errno, error, __ATOMIC_RELAXED);
    __atomic_fetch_add(&counters.failures, 1, __ATOMIC_RELAXED);
    uint64_t all, ours;
    count_vmas(&all, &ours);
    char line[256];
    /* strerrorname_np, not strerror: no allocation (at the map limit even
     * ASan's allocator cannot get memory). */
    const char *name = strerrorname_np(error);
    int n = snprintf(line, sizeof(line),
                     "hostwin_linux: %s at %#lx failed: errno %d (%s); process VMAs %llu, in "
                     "reservations %llu, vm.max_map_count %ld\n",
                     what, (unsigned long)host_page, error, name ? name : "?",
                     (unsigned long long)all, (unsigned long long)ours, read_max_map_count());
    if (n > 0)
        (void)!write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

/* Every MAP_FIXED below must stay inside a reservation: anything else
 * would silently replace unrelated memory. */
static void require_inside(uintptr_t host_page, uint64_t bytes)
{
    for (unsigned i = 0; i < reservation_count; ++i)
        if (host_page >= reservations[i].base && bytes <= reservations[i].end - host_page &&
            !(host_page & (PAGE - 1)) && bytes && !(bytes & (PAGE - 1)))
            return;
    fprintf(stderr, "hostwin_linux: operation outside the reservations at %#lx (+%#llx)\n",
            (unsigned long)host_page, (unsigned long long)bytes);
    abort();
}

/* ---- backend operations ------------------------------------------------ */

static bool reserve(void *ctx, unsigned w, unsigned slots, uintptr_t *base, uintptr_t *poison)
{
    (void)ctx;
    if (reservation_count == MAX_RESERVATIONS || w >= 47 || !slots)
        return false;
    if (w == __atomic_load_n(&refused_width, __ATOMIC_RELAXED))
        return false;
    long limit = hostwin_linux_map_limit();
    uint64_t all, ours;
    count_vmas(&all, &ours);
    uint64_t need = hostwin_linux_vmas_per_reservation();
    uint64_t committed = (uint64_t)reservation_count * need;
    /* (fix) The reservations' own VMAs are inside `committed`
     * (their worst case); counting them in `all` too refused a new
     * reservation next to filled ones although the worst case fits. */
    if (limit < 0 || (all - ours) + committed + need + hostwin_linux_headroom() > (uint64_t)limit) {
        fprintf(
            stderr,
            "hostwin_linux: reservation refused: %llu VMAs outside reservations, %llu committed "
            "to %u reservations, %llu needed + %llu headroom > vm.max_map_count %ld\n",
            (unsigned long long)(all - ours), (unsigned long long)committed, reservation_count,
            (unsigned long long)need, (unsigned long long)hostwin_linux_headroom(), limit);
        return false;
    }
    /* Find a free range large enough to align, then place the slots and
     * the poison page exactly; NOREPLACE never overwrites a mapping. */
    size_t span = ((size_t)slots << w) + PAGE, align = (size_t)1 << w;
    void *p =
        mmap(NULL, span + align, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return false;
    uintptr_t at = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
    if (munmap(p, span + align))
        return false;
    void *q = mmap((void *)at, span, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (q == MAP_FAILED)
        return false;
    if ((uintptr_t)q != at) {
        munmap(q, span);
        return false;
    }
    reservations[reservation_count++] = (struct reservation){at, at + span};
    *base = at;
    *poison = at + ((uintptr_t)slots << w);
    return true;
}

static void release(void *ctx, uintptr_t base, unsigned w, unsigned slots)
{
    (void)ctx;
    size_t span = ((size_t)slots << w) + PAGE;
    for (unsigned i = 0; i < reservation_count; ++i)
        if (reservations[i].base == base && reservations[i].end == base + span) {
            if (munmap((void *)base, span)) {
                failure("release", base);
                abort();
            }
            reservations[i] = reservations[--reservation_count];
            return;
        }
    fprintf(stderr, "hostwin_linux: release of an unknown reservation %#lx\n", (unsigned long)base);
    abort();
}

static enum hostwin_map_status map(void *ctx, uintptr_t host_page, gpa_t pa_page, bool writable)
{
    (void)ctx;
    require_inside(host_page, PAGE);
    if (pa_page & (PAGE - 1)) {
        fprintf(stderr, "hostwin_linux: unaligned pa_page %#llx\n", (unsigned long long)pa_page);
        abort();
    }
    __atomic_fetch_add(&counters.maps, 1, __ATOMIC_RELAXED);
    /* The page every other accessor uses (gm_read/gm_write, interpreter,
     * Tier0): hostwin_resolve has found it wholly inside one GM_RAM region
     * with a page-aligned hva. The alias is that host page's
     * own memfd page, or none. */
    uintptr_t hva = (uintptr_t)gm_direct_ptr(pa_page, PAGE);
    for (unsigned i = 0; hva && i < memfd_count; ++i) {
        const struct memfd_range *m = &memfds[i];
        if (hva < m->hva || hva - m->hva >= m->size)
            continue;
        if ((hva - m->hva) & (PAGE - 1))
            break; /* not a whole page of the file */
        void *p = mmap((void *)host_page, PAGE, PROT_READ | (writable ? PROT_WRITE : 0),
                       MAP_SHARED | MAP_FIXED, m->fd, (off_t)(hva - m->hva));
        if (p == (void *)host_page)
            return HOSTWIN_MAP_OK;
        failure("map", host_page);
        return HOSTWIN_MAP_ERROR;
    }
    return HOSTWIN_MAP_UNBACKED;
}

static bool unmap(void *ctx, uintptr_t host_page, uint64_t bytes)
{
    (void)ctx;
    require_inside(host_page, bytes);
    __atomic_fetch_add(&counters.unmaps, 1, __ATOMIC_RELAXED);
    void *p = mmap((void *)host_page, bytes, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    if (p == (void *)host_page)
        return true;
    failure("unmap", host_page);
    return false;
}

static bool protect_ro(void *ctx, uintptr_t host_page)
{
    (void)ctx;
    require_inside(host_page, PAGE);
    __atomic_fetch_add(&counters.protects, 1, __ATOMIC_RELAXED);
    if (!mprotect((void *)host_page, PAGE, PROT_READ))
        return true;
    failure("protect_ro", host_page);
    return false;
}

/* Linux mmap/mprotect are synchronous for the process. */
static void flush(void *ctx, uintptr_t host_page_or_all, bool sync)
{
    (void)ctx;
    (void)host_page_or_all;
    (void)sync;
}

static const struct hostwin_backend backend = {
    .reserve = reserve,
    .release = release,
    .map = map,
    .unmap = unmap,
    .protect_ro = protect_ro,
    .flush = flush,
};
const struct hostwin_backend *hostwin_linux_backend(void)
{
    return &backend;
}
void *hostwin_linux_ctx(void)
{
    return &ctx_token;
}

/* The product's backend (hostwin.h): the SIGSEGV trap path is installed
 * once, on the first call; hostwin_linux_init keeps later calls from
 * chaining the handler to itself. */
const struct hostwin_backend *hostwin_host_backend(void **ctx)
{
    *ctx = NULL;
    if (!hostwin_linux_init()) {
        fprintf(stderr, "hostwin_linux: cannot install the SIGSEGV handler: errno %d\n", errno);
        return NULL;
    }
    *ctx = hostwin_linux_ctx();
    return hostwin_linux_backend();
}

uint8_t *hostwin_linux_alloc(uint64_t bytes)
{
    if (!bytes || bytes > (uint64_t)INT64_MAX - (PAGE - 1) || bytes > SIZE_MAX - (PAGE - 1) ||
        memfd_count == MAX_MEMFDS)
        return NULL;
    uint64_t size = (bytes + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    int fd = memfd_create("ia64vmm-memory", MFD_CLOEXEC);
    if (fd < 0)
        return NULL;
    /* A new memfd reads as zeros (shmem allocates on first touch). */
    if (ftruncate(fd, (off_t)size)) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    memfds[memfd_count++] = (struct memfd_range){(uintptr_t)p, size, fd};
    return p;
}

uint8_t *hostwin_linux_ram(gpa_t base, uint64_t size)
{
    if ((base | size) & (PAGE - 1) || !size || size > UINT64_MAX - base || ram_count == MAX_RAM)
        return NULL;
    for (unsigned i = 0; i < ram_count; ++i)
        if (base < ram[i].base + ram[i].size && ram[i].base < base + size)
            return NULL;
    uint8_t *p = hostwin_linux_alloc(size);
    if (p)
        ram[ram_count++] = (struct ram_range){base, size};
    return p;
}

/* ---- SIGSEGV ------------------------------------------------------------ */

static const int greg_of[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
                                REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
                                REG_R12, REG_R13, REG_R14, REG_R15};

static void handler(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    uintptr_t va = (uintptr_t)info->si_addr;
    if (hostwin_owns(va)) {
        struct hostwin_trap t;
        for (unsigned i = 0; i < 16; ++i)
            t.gpr[i] = (uint64_t)uc->uc_mcontext.gregs[greg_of[i]];
        t.rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
        t.rflags = (uint64_t)uc->uc_mcontext.gregs[REG_EFL];
        t.fault_va = va;
        t.write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
        if (hostwin_trap(&t)) {
            for (unsigned i = 0; i < 16; ++i)
                if (i != 4)
                    uc->uc_mcontext.gregs[greg_of[i]] = (greg_t)t.gpr[i];
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)t.rip;
            uc->uc_mcontext.gregs[REG_EFL] = (greg_t)t.rflags;
            return;
        }
    }
    /* Not ours, or declined: a real crash for whoever handled SIGSEGV
     * before us. */
    if (previous.sa_flags & SA_SIGINFO) {
        previous.sa_sigaction(sig, info, context);
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(sig);
    } else {
        /* The fault repeats on return and terminates the process. */
        signal(SIGSEGV, SIG_DFL);
    }
}

bool hostwin_linux_init(void)
{
    struct sigaction current;
    if (sigaction(SIGSEGV, NULL, &current))
        return false;
    if ((current.sa_flags & SA_SIGINFO) && current.sa_sigaction == handler)
        return true;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO; /* no SA_ONSTACK: run on the faulting (region) stack */
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGSEGV, &sa, &previous) == 0;
}
