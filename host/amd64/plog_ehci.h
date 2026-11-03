/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_PLOG_EHCI_H
#define VMM_PLOG_EHCI_H
#include "persist/persist.h"
#include "ia64vmm/pci.h"
/* The reserved vmm_media controller when it is EHCI (class 0C/03/20);
 * log: started for media_log=1, else only for the EFI variable slots,
 * quiet when no stick is there. */
const struct plog_block *amd64_plog_ehci(const struct pci_device *, bool log);
/* Management CPU during a guest run: the root ports other than the
 * medium's, on a controller with companions; returns the time of the next
 * visit (UINT64_MAX: none). */
uint64_t amd64_plog_ehci_watch(void);
/* After the guest run, before the VMM powers off, resets or halts the
 * machine: the ports go to the companion controllers. Nothing when the
 * backend is not running or after a panic. */
void amd64_plog_ehci_shutdown(void);
#endif
