/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SPC REQUEST SENSE fixed/descriptor response, bounded UA/not-ready
 * retries; SBC caching mode page WCE and MODE SENSE DPOFUA. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "persist/protocol.h"
static unsigned phase, commands, senses, waits, mode, opcode, length;
static uint32_t tag;
static int transfer(bool in, void *data, unsigned n)
{
    unsigned char *b = data;
    if (phase == 0) {
        assert(!in && n == 31);
        memcpy(&tag, b + 4, 4);
        opcode = b[15];
        memcpy(&length, b + 8, 4);
        ++commands;
        phase = length ? 1 : 2;
        if (opcode == 3)
            ++senses;
        return n;
    }
    if (phase == 1) {
        assert(in && n == length);
        memset(b, 0, n);
        phase = 2;
        if (opcode == 3) {
            if (mode == 5) {
                b[0] = 0x72;
                b[1] = 6;
                b[7] = 0;
            } else {
                b[0] = 0x70;
                b[2] = mode == 1 ? 2 : mode == 2 ? 5 : 6;
                b[7] = 10;
            }
        }
        return n;
    }
    assert(in && n == 13);
    memset(b, 0, n);
    memcpy(b, "USBS", 4);
    memcpy(b + 4, &tag, 4);
    b[12] = opcode != 3 && (mode == 3 || senses == 0);
    phase = 0;
    return n;
}
static bool clear(unsigned pipe)
{
    (void)pipe;
    assert(false);
    return false;
}
static bool reset(void)
{
    assert(false);
    return false;
}
static void wait(unsigned ms)
{
    assert(ms == 100);
    ++waits;
}
int main(void)
{
    const struct bot_io io = {transfer, clear, reset};
    for (mode = 0; mode <= 5; ++mode) {
        uint32_t next_tag = 0;
        unsigned actual, key;
        uint8_t cdb[10] = {mode == 4 ? 0x25 : 0}, data[8];
        phase = commands = senses = waits = 0;
        bool ok = scsi_execute(&io, &next_tag, cdb, mode == 4 ? 10 : 6, data, mode == 4 ? 8 : 0,
                               true, &actual, wait, &key);
        if (mode == 2 || mode == 3)
            assert(!ok);
        else
            assert(ok && senses == 1 && commands == 3 && waits == 1);
        if (mode == 2)
            assert(key == 5 && waits == 0 && commands == 2);
        if (mode == 3)
            assert(waits == 9 && commands == 20);
        if (mode == 4)
            assert(actual == 8);
    }
    uint8_t page[64] = {23, 0, 0, 0, 8, 18};
    bool wce, fua;
    assert(scsi_caching(page, 24, &wce, &fua) && !wce && !fua);
    page[2] = 0x10;
    page[6] = 4;
    assert(scsi_caching(page, 24, &wce, &fua) && wce && fua);
    assert(!scsi_caching(page, 6, &wce, &fua));
    page[0] = 3;
    assert(!scsi_caching(page, 24, &wce, &fua));
    page[0] = 23;
    page[3] = 64;
    assert(!scsi_caching(page, 24, &wce, &fua));
    page[3] = 0;
    page[4] = 9;
    assert(!scsi_caching(page, 24, &wce, &fua));
    page[4] = 8;
    page[5] = 1;
    assert(!scsi_caching(page, 24, &wce, &fua));
    puts("PLOG SCSI: passed");
}
