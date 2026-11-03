/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_BOOT_ACPI_H
#define VMM_BOOT_ACPI_H
#include "ia64vmm/bootinfo.h"

/* Copy host ACPI after installing the VMM mapping; private roots and
 * DSDT pointers support the full physical address width. */
void amd64_copy_acpi(struct boot_info *);
#endif
