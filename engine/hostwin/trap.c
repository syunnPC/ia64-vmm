/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host trap plumbing. The backend's fault
 * handler (SIGSEGV hosted, #PF bare metal) fills a struct hostwin_trap for
 * a fault inside a window and calls hostwin_trap(), which hands it to the
 * registered client (Tier1's t1_trap_client). The client runs in the fault
 * context and must be async-signal-safe; it either redirects t->rip (true)
 * or declines (false: the backend treats the fault as a real crash).
 */
#include "engine/hostwin/internal.h"

static hostwin_trap_client_fn client;

void hostwin_set_trap_client(hostwin_trap_client_fn fn)
{
    __atomic_store_n(&client, fn, __ATOMIC_RELEASE);
}

bool hostwin_trap(struct hostwin_trap *t)
{
    hostwin_trap_client_fn fn = __atomic_load_n(&client, __ATOMIC_ACQUIRE);
    return fn && fn(t);
}
