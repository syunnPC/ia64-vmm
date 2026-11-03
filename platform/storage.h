/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_PLATFORM_STORAGE_H
#define IA64VMM_PLATFORM_STORAGE_H
#include "ia64vmm/block.h"
#define PLATFORM_STORAGE_BDF 0x00f8u
#define PLATFORM_STORAGE_BASE UINT64_C(0xfebf0000)
#define PLATFORM_STORAGE_GSI 20u
struct platform_state;
/* Append one medium before initialization; NULL clears all attachments. */
bool platform_storage_attach(const struct platform_block *);
bool platform_storage_configured(void);
bool platform_storage_init(struct platform_state *);
#endif
