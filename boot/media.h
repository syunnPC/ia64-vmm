/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_BOOT_MEDIA_H
#define VMM_BOOT_MEDIA_H
#include "ia64vmm/bootmedia.h"
#include "ia64vmm/types.h"

typedef const void *(*boot_media_map_fn)(uint64_t physical, size_t size, void *context);
bool boot_media_read(struct boot_media *, uint64_t system_table, boot_media_map_fn, void *context);
#endif
