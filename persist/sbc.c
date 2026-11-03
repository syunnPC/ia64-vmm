/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/User/Usb/{Scsi,Xhci}.cpp
 */
/* SCSI disk on a BOT transport: SPC INQUIRY, TEST UNIT READY, MODE
 * SENSE(6)/MODE SELECT(6); SBC READ CAPACITY(10), READ(10), WRITE(10) and
 * SYNCHRONIZE CACHE(10). Apache-2.0 text: LICENSES/Apache-2.0.txt. */
#include "persist/protocol.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

static bool data(struct scsi_disk *d, const uint8_t *cdb, unsigned cdb_size, void *buffer,
                 unsigned length, bool in, unsigned *actual)
{
    if (length > 16384)
        return false;
    unsigned key;
    bool ok =
        scsi_execute(d->io, &d->tag, cdb, cdb_size, buffer, length, in, actual, d->wait, &key);
    if (!ok)
        LOGE("%s SCSI command=%x failed sense=%u", d->who, cdb[0], key);
    return ok;
}
static bool command(struct scsi_disk *d, const uint8_t *cdb, unsigned cdb_size, void *buffer,
                    unsigned length, bool in)
{
    unsigned actual;
    return data(d, cdb, cdb_size, buffer, length, in, &actual) && actual == length;
}
bool scsi_disk_read(struct scsi_disk *d, uint64_t lba, unsigned count, void *buffer)
{
    uint8_t cdb[10];
    return count <= 32 && lba < d->sectors && count <= d->sectors - lba &&
           scsi_range(cdb, false, lba, count) && command(d, cdb, 10, buffer, count * 512, true);
}
bool scsi_disk_write(struct scsi_disk *d, uint64_t lba, unsigned count, const void *buffer)
{
    uint8_t cdb[10];
    if (count > 32 || lba >= d->sectors || count > d->sectors - lba ||
        !scsi_range(cdb, true, lba, count))
        return false;
    if (d->fua)
        cdb[1] |= 8; /* SBC WRITE(10): force stable media. */
    return command(d, cdb, 10, (void *)buffer, count * 512, false);
}
bool scsi_disk_flush(struct scsi_disk *d)
{
    if (!d->wce || d->fua)
        return true;
    uint8_t cdb[10] = {0x35};
    return command(d, cdb, 10, NULL, 0, false);
}
bool scsi_disk_caching(struct scsi_disk *d)
{
    uint8_t mode[64], cdb[6] = {0x1a, 8, 8, 0, 64};
    unsigned actual;
    if (!data(d, cdb, 6, mode, 64, true, &actual) ||
        !scsi_caching(mode, actual, &d->wce, &d->fua)) {
        LOGE("%s caching mode unavailable; durability cannot be established", d->who);
        return false;
    }
    /* SBC mode parameter header: WP in the device-specific parameter. */
    if (mode[2] & 0x80) {
        LOGE("%s medium is write-protected", d->who);
        return false;
    }
    d->fua = d->wce && d->fua;
    if (!d->wce || d->fua || scsi_disk_flush(d))
        return true;
    /* A cache-enabled device rejecting SYNCHRONIZE CACHE can still provide
     * durable writes if MODE SELECT disables WCE. Verify the new setting. */
    if (mode[3] || (mode[4] & 0x3f) != 8 || mode[5] != 18)
        return false;
    mode[0] = mode[1] = mode[2] = mode[3] = 0;
    mode[4] &= 0x3f;
    mode[6] &= ~4u;
    uint8_t select[6] = {0x15, 0x10, 0, 0, 24};
    if (!command(d, select, 6, mode, 24, false) || !data(d, cdb, 6, mode, 64, true, &actual) ||
        !scsi_caching(mode, actual, &d->wce, &d->fua) || d->wce)
        return false;
    d->fua = false;
    return true;
}
bool scsi_disk_start(struct scsi_disk *d)
{
    uint8_t inquiry[36], capacity[8], cdb[10] = {0x12, 0, 0, 0, 36};
    if (!command(d, cdb, 6, inquiry, 36, true) || (inquiry[0] & 31) != 0)
        return false;
    memset(cdb, 0, 10);
    if (!command(d, cdb, 6, NULL, 0, false) || !scsi_disk_caching(d))
        return false;
    cdb[0] = 0x25;
    if (!command(d, cdb, 10, capacity, 8, true))
        return false;
    uint32_t last = 0, size = 0;
    for (unsigned i = 0; i < 4; ++i) {
        last = last << 8 | capacity[i];
        size = size << 8 | capacity[i + 4];
    }
    if (last == UINT32_MAX || size != 512) {
        LOGE("%s requires 512-byte sectors and READ CAPACITY(10) range", d->who);
        return false;
    }
    d->sectors = (uint64_t)last + 1;
    return true;
}
