/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_BOOT_DIAG_H
#define VMM_AMD64_BOOT_DIAG_H
#include "ia64vmm/bootinfo.h"

void amd64_boot_diag_cpu(bool after, uint64_t hhdm);
void amd64_boot_diag_reserve(const struct boot_info *, uint64_t base, uint64_t size);
void amd64_boot_diag_metadata(uint64_t address, uint64_t bytes);
void amd64_boot_diag_written(void);
bool amd64_boot_diag_available(void);
void amd64_boot_diag_dump(void);
bool amd64_boot_diag_recover(uint64_t vector, uint64_t error, uint64_t *rip, uint64_t cr2);
#endif
