/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_PLOG_HYPERV_H
#define VMM_PLOG_HYPERV_H
#include "persist/persist.h"
const struct plog_block *amd64_plog_hyperv(const struct boot_info *);
bool amd64_hyperv_storage_start(void);
bool amd64_hyperv_storage_discover(void);
void amd64_plog_hyperv_shutdown(void);
#endif
