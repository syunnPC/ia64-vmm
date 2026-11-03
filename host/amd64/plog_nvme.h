/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_PLOG_NVME_H
#define VMM_PLOG_NVME_H
#include "persist/persist.h"
#include "ia64vmm/pci.h"
/* The reserved vmm_media controller when it is NVMe (class 01/08/02);
 * log: started for media_log=1, else only for the EFI variable slots. */
const struct plog_block *amd64_plog_nvme(const struct pci_device *, bool log);
/* Before the VMM powers off, resets or halts the machine, after the last
 * write; nothing when the backend is not running or after a panic. */
void amd64_plog_nvme_shutdown(void);
#endif
