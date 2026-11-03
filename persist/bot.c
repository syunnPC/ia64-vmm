/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/User/Usb/{Bot,Scsi}.cpp
 */
/* Blocking subset; USB MSC BOT 1.0 §6. Apache-2.0 text: LICENSES/Apache-2.0.txt. */
#include "persist/protocol.h"
static enum bot_result recover(const struct bot_io *io)
{
    /* Reset recovery is required even after an invalid CBW transfer. A
     * successful recovery does not turn the interrupted command into success. */
    io->reset();
    return BOT_RESET;
}
enum bot_result bot_execute(const struct bot_io *io, uint32_t tag, const uint8_t *cdb,
                            unsigned cdb_size, void *data, unsigned length, bool in,
                            unsigned *actual)
{
    uint8_t cbw[31], csw[13];
    *actual = 0;
    if (!bot_cbw(cbw, tag, cdb, cdb_size, length, in))
        return BOT_FAILED;
    if (io->transfer(false, cbw, 31) != 31)
        return recover(io);
    unsigned transferred = 0;
    if (length) {
        int n = io->transfer(in, data, length);
        if (n == -2) {
            if (!io->clear(in ? 0 : 1))
                return recover(io);
        } else if (n < 0 || (unsigned)n > length)
            return recover(io);
        else
            transferred = (unsigned)n;
    }
    int n = io->transfer(true, csw, 13);
    if (n == -2 && io->clear(0))
        n = io->transfer(true, csw, 13);
    if (n < 0)
        return recover(io);
    enum bot_result result = bot_csw(csw, (unsigned)n, tag, length, transferred);
    if (result == BOT_RESET)
        return recover(io);
    uint32_t residue =
        csw[8] | (uint32_t)csw[9] << 8 | (uint32_t)csw[10] << 16 | (uint32_t)csw[11] << 24;
    *actual = length - residue;
    return result;
}

/* SPC TEST UNIT READY/REQUEST SENSE: current UA and not-ready may be transient,
 * but transport failures and deferred errors cannot be retried as success. */
bool scsi_execute(const struct bot_io *io, uint32_t *tag, const uint8_t *cdb, unsigned cdb_size,
                  void *data, unsigned length, bool in, unsigned *actual, void (*wait)(unsigned),
                  unsigned *key)
{
    *actual = 0;
    *key = 0;
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        if (*tag == UINT32_MAX)
            return false;
        enum bot_result result = bot_execute(io, ++*tag, cdb, cdb_size, data, length, in, actual);
        if (result == BOT_PASSED)
            return true;
        if (result != BOT_FAILED || cdb[0] == 3 || *tag == UINT32_MAX)
            return false;
        uint8_t sense[18], request[6] = {3, 0, 0, 0, 18};
        unsigned n;
        if (bot_execute(io, ++*tag, request, 6, sense, 18, true, &n) != BOT_PASSED)
            return false;
        if (n >= 14 && (sense[0] & 0x7f) == 0x70 && sense[7] >= 6)
            *key = sense[2] & 15;
        else if (n >= 8 && (sense[0] & 0x7f) == 0x72)
            *key = sense[1] & 15;
        else
            return false;
        if ((*key != 6 && *key != 2) || attempt == 9)
            return false;
        wait(100);
    }
    return false;
}
