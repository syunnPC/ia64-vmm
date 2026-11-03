/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "boot/media.h"
#include "ia64vmm/string.h"

static uint64_t little(const uint8_t *p, unsigned size)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < size; ++i)
        n |= (uint64_t)p[i] << (8 * i);
    return n;
}
bool boot_media_read(struct boot_media *out, uint64_t system_table, boot_media_map_fn map,
                     void *context)
{
    static const struct {
        uint32_t a;
        uint16_t b, c;
        uint8_t d[8];
    } guid = BOOT_MEDIA_GUID;
    *out = (struct boot_media){.status = BOOT_MEDIA_INVALID};
    if (!system_table)
        return false;
    /* UEFI 2.10 §4.3: x64 EFI_SYSTEM_TABLE; configuration pointers stay physical. */
    const uint8_t *st = map(system_table, 120, context);
    if (!st || memcmp(st, "IBI SYST", 8) || little(st + 12, 4) < 120)
        return false;
    uint64_t count = little(st + 104, 8), address = little(st + 112, 8);
    if (!count || count > 1024 || !address || address > UINT64_MAX - count * 24)
        return false;
    const uint8_t *tables = map(address, count * 24, context);
    if (!tables)
        return false;
    struct boot_media found = {0};
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *entry = tables + i * 24;
        if (memcmp(entry, &guid, sizeof(guid)))
            continue;
        if (found.magic)
            return false;
        address = little(entry + 16, 8);
        if (!address || address > UINT64_MAX - sizeof(found))
            return false;
        const void *record = map(address, sizeof(found), context);
        if (!record)
            return false;
        memcpy(&found, record, sizeof(found));
        if (found.magic != BOOT_MEDIA_MAGIC || found.version != BOOT_MEDIA_VERSION ||
            found.size != sizeof(found) || found.status > BOOT_MEDIA_UNSUPPORTED || found.reserved)
            return false;
    }
    if (!found.magic)
        return false;
    *out = found;
    return true;
}
