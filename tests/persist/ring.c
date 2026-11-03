/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "core/log_internal.h"
static struct log_ring fixed;
int main(void)
{
    fixed.magic = LOG_RING_MAGIC;
    fixed.generation = 7;
    fixed.write_pos = 8;
    memcpy(fixed.data, "crashed\n", 8);
    log_relocate_fixed(&fixed);
    char out[32];
    uint64_t cursor = 0, lost;
    assert(log_crash_read(&cursor, out, sizeof(out)) == 8 && !memcmp(out, "crashed\n", 8));
    assert(log_crash_read(&cursor, out, sizeof(out)) == 0);
    assert(log_get_ring() == &fixed && fixed.generation == 8);
    cursor = 0;
    size_t n = log_media_read(&cursor, out, sizeof(out), &lost);
    assert(n && !lost && fixed.flushed_pos == 0);
    log_media_flushed(cursor);
    assert(fixed.flushed_pos == cursor);
    log_media_flushed(fixed.write_pos + 1);
    assert(fixed.flushed_pos == cursor);
    log_media_clean();
    assert(fixed.magic == 0);
    log_relocate_fixed(&fixed);
    cursor = 0;
    assert(log_crash_read(&cursor, out, sizeof(out)) == 0);
    log_panic_begin();
    cursor = 0;
    assert(log_media_read(&cursor, out, sizeof(out), &lost) == 0 && log_in_panic());
    puts("PLOG crash ring: passed");
}
