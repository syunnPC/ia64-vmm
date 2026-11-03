/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hosted host-window backend: the Linux window backend
 * (hostwin_host_backend, engine/hostwin/hostwin.h) of the Tier1 test binaries. memfd-backed host memory, window reservations
 * placed with MAP_FIXED_NOREPLACE, single-page MAP_SHARED|MAP_FIXED
 * shadows, and the SIGSEGV trap with chaining to the previous handler.
 */
#ifndef IA64VMM_HOST_LINUX_HOSTWIN_LINUX_H
#define IA64VMM_HOST_LINUX_HOSTWIN_LINUX_H
#include "engine/hostwin/hostwin.h"

/* Installs the SIGSEGV handler (SA_SIGINFO, no SA_ONSTACK) and remembers
 * the previous one for chaining. Idempotent: while our handler is the
 * installed one, a further call changes nothing (re-saving it as
 * "previous" would make it chain to itself forever). hostwin_host_backend
 * calls it; nothing else in a test does, so the default engines run
 * with the process's original SIGSEGV disposition. */
bool hostwin_linux_init(void);
const struct hostwin_backend *hostwin_linux_backend(void);
void *hostwin_linux_ctx(void);
/* Host memory a shadow can alias: bytes rounded up to
 * 4 KiB, zeroed, a memfd mapped MAP_SHARED at an address of the kernel's
 * choice; NULL on failure. a test that needs aliasable guest RAM allocates it
 * with this. map()
 * aliases the page gm_direct_ptr(pa_page, 4096) returns -- the very page
 * gm_read/gm_write, the interpreter and Tier0 access -- whenever that page
 * lies in one of these allocations; any other backing is
 * HOSTWIN_MAP_UNBACKED. Setup only (before any executor runs); never
 * freed. */
uint8_t *hostwin_linux_alloc(uint64_t bytes);
/* Guest RAM [base, base + size) for the test harnesses, both 4 KiB
 * multiples: a hostwin_linux_alloc allocation; returns its address, the hva
 * for gm_add_region (interpreter, helpers and windows then see the same
 * pages), or NULL. Ranges must not overlap. */
uint8_t *hostwin_linux_ram(gpa_t base, uint64_t size);

/* (tests and diagnostics) */
struct hostwin_linux_stats {
    long max_map_count;        /* vm.max_map_count, -1 if unreadable */
    uint64_t process_vmas;     /* all VMAs of the process */
    uint64_t reservation_vmas; /* VMAs inside window reservations (incl. poison page) */
    unsigned reservations;
    uint64_t maps, unmaps, protects, failures; /* backend calls; failed ones */
    int last_errno;
};
/* Counts VMAs from /proc/self/maps without allocating (usable at the map
 * limit). */
void hostwin_linux_get_stats(struct hostwin_linux_stats *out);
/* VMAs a reservation must be able to grow to (2 * HOSTWIN_MAX_SHADOWS + 1)
 * plus the headroom kept free for the rest of the process. */
uint64_t hostwin_linux_vmas_per_reservation(void);
uint64_t hostwin_linux_headroom(void);
/* (tests only) Admission as if vm.max_map_count were
 * min(the real value, limit); 0 restores the real value. Lets a host with a
 * raised limit check the behaviour of the default 65530: how many
 * instances coexist and that teardown returns every VMA. The kernel still
 * enforces only the real limit. */
void hostwin_linux_set_map_limit(long limit);
long hostwin_linux_map_limit(void);
/* (tests only) Refuses reservations of width w; 0 refuses none. */
void hostwin_linux_refuse_width(unsigned w);
#endif
