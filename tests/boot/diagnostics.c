/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "host/amd64/boot_walk.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

/* Intel SDM Vol.3A, 4-level paging: 9-bit indices, 4 KiB tables,
 * 1 GiB/2 MiB PS leaves, cumulative R/W permissions, and #PF error bits.
 * Limine base revision 6 provides a direct-map offset for table memory. */
static const uint64_t hhdm = UINT64_C(0xffff800000000000);
static const uint64_t va = UINT64_C(0xffff800123456788);
static uint64_t tables[4][512];
static unsigned reads, deny;
static bool read_entry(void *ctx, uint64_t address, uint64_t *value)
{
    assert(ctx == tables);
    ++reads;
    if (reads == deny)
        return false;
    assert(address >= hhdm + 0x1000 && address < hhdm + 0x5000);
    uint64_t offset = address - hhdm - 0x1000;
    assert(!(offset & 7));
    *value = tables[offset / 4096][offset % 4096 / 8];
    return true;
}
static void fixture(void)
{
    memset(tables, 0, sizeof(tables));
    reads = deny = 0;
    tables[0][(va >> 39) & 511] = 0x2003;
    tables[1][(va >> 30) & 511] = 0x3003;
    tables[2][(va >> 21) & 511] = 0x4003;
    tables[3][(va >> 12) & 511] = 0x98765003;
}
static struct amd64_boot_walk walk(void)
{
    struct amd64_boot_walk w;
    amd64_boot_walk(0x1018, hhdm, va, read_entry, tables, &w);
    return w;
}
extern bool amd64_boot_read64(uint64_t, uint64_t *);
extern char amd64_boot_read64_fault[];
static struct amd64_boot_probe signal_probe;
static volatile sig_atomic_t recovered;
static void fault(int sig, siginfo_t *info, void *arg)
{
    (void)sig;
    ucontext_t *ctx = arg;
    /* Linux executes this same load at CPL3: remove U/S to exercise the
     * CPL0 recovery contract with the actual fault RIP/address/error. */
    if (!amd64_boot_probe_matches(&signal_probe, true, 14,
                                  (uint64_t)ctx->uc_mcontext.gregs[REG_ERR] & ~UINT64_C(4),
                                  ctx->uc_mcontext.gregs[REG_RIP], (uintptr_t)info->si_addr))
        _exit(88);
    ++recovered;
    ctx->uc_mcontext.gregs[REG_RIP] = (uintptr_t)amd64_boot_read64_fault;
}
static void load_recovery(void)
{
    uint64_t value = 0x12345678, result = 0;
    assert(amd64_boot_read64((uintptr_t)&value, &result) && result == value);
    void *missing = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(missing != MAP_FAILED);
    struct sigaction action = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO}, prior;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGSEGV, &action, &prior));
    signal_probe =
        (struct amd64_boot_probe){true, (uintptr_t)amd64_boot_read64, (uintptr_t)missing};
    assert(!amd64_boot_read64((uintptr_t)missing, &result));
    assert(recovered == 1 && result == value);
    /* A fault at the following output store must not use the load fixup. */
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        amd64_boot_read64((uintptr_t)&value, missing);
        _exit(99);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 88);
    signal_probe.armed = false;
    assert(!sigaction(SIGSEGV, &prior, NULL));
    assert(!munmap(missing, 4096));
}
int main(void)
{
    fixture();
    struct amd64_boot_walk w = walk();
    assert(w.status == AMD64_BOOT_PRESENT && w.count == 4 && reads == 4);
    assert(w.physical == 0x98765788 && w.page_bytes == 4096 && w.writable);
    tables[0][(va >> 39) & 511] &= ~UINT64_C(2);
    w = walk();
    assert(w.status == AMD64_BOOT_PRESENT && !w.writable);
    for (unsigned level = 0; level < 4; ++level) {
        fixture();
        tables[level][(va >> (39 - 9 * level)) & 511] = 0;
        w = walk();
        assert(w.status == AMD64_BOOT_ABSENT && reads == level + 1);
        assert(w.count == level + 1 && w.entry[level] == 0);
        fixture();
        deny = level + 1;
        w = walk();
        assert(w.status == AMD64_BOOT_UNREADABLE && reads == level + 1);
        assert(w.count == level + 1 && w.slot[level] != 0);
    }
    fixture();
    tables[1][(va >> 30) & 511] = UINT64_C(0x180001083); /* 1 GiB, PAT. */
    w = walk();
    assert(w.status == AMD64_BOOT_PRESENT && reads == 2 && w.writable);
    assert(w.page_bytes == 0x40000000 && w.physical == UINT64_C(0x1a3456788));
    fixture();
    tables[2][(va >> 21) & 511] = UINT64_C(0x8000000123401083); /* 2 MiB, PAT/NX. */
    w = walk();
    assert(w.status == AMD64_BOOT_PRESENT && reads == 3 && w.writable);
    assert(w.page_bytes == 0x200000 && w.physical == UINT64_C(0x123456788));
    fixture();
    amd64_boot_walk(0x1000, hhdm, UINT64_C(0x0000800000000000), read_entry, tables, &w);
    assert(w.status == AMD64_BOOT_INVALID && reads == 0);
    fixture();
    tables[0][(va >> 39) & 511] |= 128;
    w = walk();
    assert(w.status == AMD64_BOOT_INVALID && reads == 1);

    struct amd64_boot_probe probe = {.armed = true, .rip = 0x1234, .address = 0x5678};
    assert(amd64_boot_probe_matches(&probe, true, 14, 0, 0x1234, 0x5678));
    assert(amd64_boot_probe_matches(&probe, true, 14, 1, 0x1234, 0x5678));
    assert(!amd64_boot_probe_matches(&probe, false, 14, 0, 0x1234, 0x5678));
    assert(!amd64_boot_probe_matches(&probe, true, 13, 0, 0x1234, 0x5678));
    assert(!amd64_boot_probe_matches(&probe, true, 14, 0, 0x1235, 0x5678));
    assert(!amd64_boot_probe_matches(&probe, true, 14, 0, 0x1234, 0x5679));
    for (unsigned bit = 1; bit < 64; ++bit)
        assert(!amd64_boot_probe_matches(&probe, true, 14, UINT64_C(1) << bit, 0x1234, 0x5678));
    probe.armed = false;
    assert(!amd64_boot_probe_matches(&probe, true, 14, 0, 0x1234, 0x5678));
    load_recovery();
    puts("boot diagnostics: paging and probe scope passed");
}
