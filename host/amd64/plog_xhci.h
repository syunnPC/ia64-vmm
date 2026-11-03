/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_PLOG_XHCI_H
#define VMM_PLOG_XHCI_H
#include "persist/persist.h"
/* log: started for media_log=1 (else only for the EFI variable slots,
 * quiet when no stick is there). */
const struct plog_block *amd64_plog_xhci(bool log);
#endif
