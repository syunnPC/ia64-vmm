/* SPDX-License-Identifier: GPL-2.0-or-later */
/* USB MSC BOT §6: stalled data clears that pipe before CSW; stalled CSW
 * clears IN once; invalid/phase-error CSW requires full reset recovery. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "persist/protocol.h"
static unsigned mode, step, resets, clears, last_pipe;
static int transfer(bool in, void *data, unsigned length)
{
    ++step;
    if (step == 1) {
        assert(!in && length == 31 && !memcmp(data, "USBC", 4));
        return mode == 1 ? 30 : 31;
    }
    if (step == 2) {
        assert(in && length == 512);
        if (mode == 2 || mode == 3)
            return -2;
        return mode == 4 ? -1 : mode == 10 ? 513 : 512;
    }
    assert(in && length == 13);
    if (mode == 5 && step == 3)
        return -2;
    if (mode == 6)
        return -2;
    uint8_t csw[13] = {'U', 'S', 'B', 'S', 7};
    if (mode == 2 || mode == 3)
        csw[9] = 2;
    if (mode == 7)
        csw[4] = 8;
    if (mode == 8)
        csw[12] = 2;
    if (mode == 9)
        csw[12] = 1;
    memcpy(data, csw, 13);
    return mode == 11 ? 12 : 13;
}
static bool clear(unsigned which)
{
    ++clears;
    last_pipe = which;
    return mode != 3;
}
static bool reset(void)
{
    ++resets;
    return mode != 10;
}
int main(void)
{
    struct bot_io io = {transfer, clear, reset};
    uint8_t data[512], cdb[10] = {0x28};
    unsigned actual;
    for (mode = 0; mode <= 11; ++mode) {
        step = resets = clears = last_pipe = 0;
        actual = 123;
        enum bot_result result = bot_execute(&io, 7, cdb, 10, data, 512, true, &actual);
        if (mode == 0 || mode == 2 || mode == 5) {
            assert(result == BOT_PASSED && !resets && actual == (mode == 2 ? 0 : 512));
            assert(step == (mode == 5 ? 4 : 3));
        } else if (mode == 9)
            assert(result == BOT_FAILED && !resets);
        else
            assert(result == BOT_RESET && resets == 1);
        assert(clears == (mode == 2 || mode == 3 || mode == 5 || mode == 6 ? 1u : 0u));
        assert(last_pipe == 0);
    }
    puts("PLOG BOT state machine: passed");
}
