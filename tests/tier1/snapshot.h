/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Snapshots of the Tier1 test machine: the
 * vCPU by value plus the whole test RAM. The device has no state besides
 * its log, and logs live per run (struct t1h_logs), so a restore empties
 * the logs of the target vCPU instead of saving them.
 *
 * Copies are page-differential: only pages whose bytes differ are copied,
 * and restored pages are announced with code_notify_vmm_write so the
 * decoded-bundle, validity and Tier0 caches drop what the run changed while
 * translations of unchanged code pages stay warm.
 */
#ifndef IA64VMM_TESTS_TIER1_SNAPSHOT_H
#define IA64VMM_TESTS_TIER1_SNAPSHOT_H
#include "tests/tier1/harness.h"

struct snapshot {
    struct vcpu v;
    uint8_t *ram; /* T1H_RAM_SIZE bytes */
};

void snapshot_init(struct snapshot *s);
void snapshot_free(struct snapshot *s);
/* Saves *v and the live RAM. */
void snapshot_take(struct snapshot *s, const struct vcpu *v);
/* Replaces *v and the live RAM by the snapshot, empties v's bound logs (if
 * any) and re-attaches the store log when it was on. Engine-side state
 * (e.g. Tier1's t1_vcpu_state_restored) is the caller's (engines.h). */
void snapshot_restore(const struct snapshot *s, struct vcpu *v);

/* RAM alone: save the live RAM into buf / load buf into the live RAM. */
void ram_save(uint8_t *buf);
void ram_load(const uint8_t *buf);
#endif
