/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DSP0134 §7.5: one type 4 per populated socket; this model fills each
 * socket before starting the next and exposes one thread per core. */
#include <assert.h>
#include <stdio.h>
#include "tests/efi/cpudesc/sockets.h"
int main(void)
{
    for (unsigned width = 1; width <= 2; ++width)
        for (unsigned count = 1; count <= 32; ++count) {
            unsigned previous = 0;
            for (unsigned left = count; left;) {
                unsigned cores = left < width ? left : width;
                assert(cpudesc_socket_valid(width, previous, cores, cores, cores));
                previous = cores;
                left -= cores;
            }
        }
    /* Sums cannot distinguish 2+2 from 3+1, 1+3 or 1+1+2. */
    assert(!cpudesc_socket_valid(2, 0, 3, 3, 3));
    assert(!cpudesc_socket_valid(2, 1, 3, 3, 3));
    assert(!cpudesc_socket_valid(2, 1, 1, 1, 1));
    assert(!cpudesc_socket_valid(1, 0, 2, 2, 2));
    assert(!cpudesc_socket_valid(2, 0, 0, 0, 0));
    assert(!cpudesc_socket_valid(2, 0, 2, 1, 2));
    assert(!cpudesc_socket_valid(2, 0, 2, 2, 1));
    assert(cpudesc_socket_valid(2, 2, 1, 1, 1));
    puts("CPUDESC per-socket checks: models/counts and invalid populations passed");
}
