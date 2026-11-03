/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_BOOTMEDIA_H
#define IA64VMM_BOOTMEDIA_H
#include <stdint.h>

#define BOOT_MEDIA_GUID                                                                            \
    {0x957437e6, 0x70c5, 0x44db, {0x9b, 0xc7, 0xe3, 0x29, 0x1b, 0x5b, 0x57, 0x81}}
#define BOOT_MEDIA_MAGIC UINT64_C(0x31544f4f424d4d56)
#define BOOT_MEDIA_VERSION 1u

enum boot_media_status {
    BOOT_MEDIA_NONE,
    BOOT_MEDIA_PCI,
    BOOT_MEDIA_INVALID,
    BOOT_MEDIA_UNSUPPORTED,
};

struct boot_media {
    uint64_t magic;
    uint32_t version, size, status;
    uint16_t segment, bdf;
    uint32_t class_code, reserved;
};
_Static_assert(sizeof(struct boot_media) == 32, "boot media wire size");
#endif
