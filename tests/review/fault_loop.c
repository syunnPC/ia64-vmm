/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A guest whose entry and IVA are both unmapped virtual addresses
 * (PSR.it = 1, no TLB entry, VHPT walker off) takes an Alternate ITLB
 * fault, whose delivery keeps PSR.it (SDM Vol.2 §5.5) and faults again at
 * IVA + 0x0c00, forever, without retiring an instruction. engine_run must
 * still return -- a diagnostic VMM exit with a state dump, never an
 * injected guest exception -- so the hosted single-thread loop sees its
 * timeout and management requests. Each engine runs under an alarm: the
 * old code never returns and the alarm fails the test.
 *
 * A fault loop that retires instructions between deliveries (a handler
 * whose `nop; nop; break` faults again after two retirements) is ordinary
 * guest progress and runs to its budget.
 */
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "tests/review/review.h"
#include "ia64/state.h"
#include "ia64vmm/guest_mem.h"
#include "core/log_internal.h"

#define IVT UINT64_C(0x180000) /* 32 KiB aligned, inside the RAM below */
static uint8_t ram[0x10000] __attribute__((aligned(4096)));
static volatile const char *running;

static void hung(int sig)
{
    (void)sig;
    static const char text[] = "review: fault-loop: engine_run did not return within the alarm: ";
    if (write(2, text, sizeof(text) - 1) < 0)
        _exit(1);
    if (running && write(2, (const char *)running, strlen((const char *)running)) < 0)
        _exit(1);
    if (write(2, "\n", 1) < 0)
        _exit(1);
    _exit(1);
}

/* True if the log ring holds `needle` after byte position `from`. */
static bool logged_since(uint64_t from, const char *needle)
{
    const struct log_ring *ring = log_get_ring();
    uint64_t end = ring->write_pos, start = end > LOG_RING_SIZE ? end - LOG_RING_SIZE : 0;
    if (from < start)
        from = start;
    size_t n = strlen(needle);
    for (uint64_t at = from; at + n <= end; ++at) {
        size_t k = 0;
        while (k < n && ring->data[(at + k) % LOG_RING_SIZE] == needle[k])
            ++k;
        if (k == n)
            return true;
    }
    return false;
}

static const char *const names[] = {"interp", "tier0", "lockstep"};

static void unmapped(enum engine_kind engine)
{
    struct vcpu_config cfg = {.engine = engine, .itc_hz = 400000000};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    const uint64_t entry = UINT64_C(0x4000000012340000), iva = UINT64_C(0x4000000056780000);
    struct ia64_entry_regs regs = {.ip = entry, .psr = IA64_PSR_IT | IA64_PSR_IC | IA64_PSR_BN};
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = iva;
    uint64_t mark = log_get_ring()->write_pos;
    running = names[engine];
    alarm(60);
    struct vcpu_exit e = engine_run(v, 1000);
    alarm(0);
    if (e.reason != VCPU_EXIT_BAD_ACCESS)
        fprintf(stderr, "review: fault-loop %s: exit %u (%s)\n", names[engine], e.reason,
                e.detail ? e.detail : "-");
    CHECK(e.reason == VCPU_EXIT_BAD_ACCESS);
    CHECK(v->retired == 0);
    /* Stopped in the IVT after an architectural delivery (Alternate ITLB,
     * IVA + 0x0c00), with PSR.ic cleared by the first one. */
    CHECK(v->s.ip == (iva | 0x0c00) && v->s.slot == 0);
    CHECK((v->s.psr & IA64_PSR_IT) && !(v->s.psr & IA64_PSR_IC));
    CHECK(v->s.cr[CR_IIP] == entry);
    CHECK(logged_since(mark, "interruption deliveries without a retired instruction"));
    vcpu_destroy(v);
}

static void retiring(enum engine_kind engine, uint64_t budget)
{
    /* { .mii nop.m 0 ; nop.i 0 ; break.i 0x1234 ;; } at the Break vector. */
    static const uint64_t handler[2] = {UINT64_C(0x0000000100000001), UINT64_C(0x0000024680000200)};
    CHECK(gm_write(IVT + 0x2c00, handler, 16));
    struct vcpu_config cfg = {.engine = engine, .itc_hz = 400000000};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    struct ia64_entry_regs regs = {.ip = IVT + 0x2c00, .psr = IA64_PSR_IC | IA64_PSR_BN};
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = IVT;
    running = names[engine];
    alarm(120);
    uint64_t left = budget;
    while (left) {
        struct vcpu_exit e = engine_run(v, left);
        CHECK(e.reason == VCPU_EXIT_BUDGET);
        left = budget > v->retired ? budget - v->retired : 0;
    }
    alarm(0);
    CHECK(v->retired >= budget);
    CHECK(v->s.ip == IVT + 0x2c00 && v->s.cr[CR_IIM] == 0x1234);
    vcpu_destroy(v);
}

static void review_fault_loop(void)
{
    signal(SIGALRM, hung);
    CHECK(gm_add_region(&(struct gm_region){
        .base = IVT, .size = sizeof(ram), .kind = GM_RAM, .name = "review-ivt", .hva = ram}));
    for (unsigned engine = ENGINE_INTERP; engine <= ENGINE_LOCKSTEP; ++engine) {
        unmapped(engine);
        unmapped(engine); /* a second guest right after a stopped one */
    }
    /* More than the bound's 2^20 deliveries, each after two retirements. */
    retiring(ENGINE_INTERP, UINT64_C(3) << 20);
    retiring(ENGINE_TIER0, UINT64_C(3) << 20);
    retiring(ENGINE_LOCKSTEP, UINT64_C(1) << 16);
}
REVIEW_SUITE("fault-loop", review_fault_loop)
