/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_CORE_PROFILE_H
#define IA64VMM_CORE_PROFILE_H
#include <stdbool.h>
#include <stdint.h>
struct vm;
/* Opt-in statistical profile (profile_hz=N, profile_sec=S): management
 * samples every running vCPU N times per second, its guest IP and CPL and,
 * through a profile IPI, the host RIP its executor was interrupted at (and
 * the RIP of every active helper CPU), and logs the most frequent ones
 * every S seconds (default 30) and at profile_dump calls, with the
 * interval's engine, helper queue and host interrupt counts. false:
 * invalid option values. */
bool profile_configure(const char *cmdline);
/* Management loop: take the due samples; returns the next deadline. */
uint64_t profile_tick(struct vm *vm);
/* Management: log and reset the current interval. */
void profile_dump(const char *label);
/* Any CPU: management dumps at its next tick. */
void profile_request_dump(const char *label);
#endif
