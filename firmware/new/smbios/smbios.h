/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_SMBIOS_H
#define VMM_SMBIOS_H
#include "../acpi/tables.h"
#define SMBIOS_IMAGE_CAPACITY 32768u
struct smbios_image {
    uint8_t entry[31], bytes[SMBIOS_IMAGE_CAPACITY];
    size_t used;
    uint16_t count, largest;
};
bool smbios_build(const struct ia64vmm_handoff *h, struct smbios_image *image,
                  uint64_t table_address);
#endif
