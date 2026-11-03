/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_PLOG_AHCI_H
#define VMM_PLOG_AHCI_H
#include "persist/persist.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/hal.h"
/* The reserved vmm_media controller when it is AHCI (class 01/06/01);
 * log: started for media_log=1, else only for the EFI variable slots. */
const struct plog_block *amd64_plog_ahci(const struct pci_device *, bool log);
/* Before the VMM powers off, resets or halts the machine, after the last
 * write: FLUSH CACHE EXT, then STANDBY IMMEDIATE when standby (power is
 * removed next). Nothing when the backend is not running or after a panic. */
void amd64_plog_ahci_shutdown(bool standby);
/* hal_shutdown removes power next unless it resets the machine; with
 * exit=halt or exit=isa-debug-exit it never does. */
static inline bool amd64_power_removed(enum hal_shutdown_kind kind, bool halt, bool debug_exit)
{
    return halt || debug_exit || kind != HAL_SHUTDOWN_RESET;
}
#endif
