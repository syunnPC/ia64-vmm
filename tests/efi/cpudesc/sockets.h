/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CPUDESC_SOCKETS_H
#define CPUDESC_SOCKETS_H
/* DSP0134 §7.5: CoreCount/CoreEnabled/ThreadCount describe this socket.
 * The VMM fills model-width sockets in order, with no disabled cores or SMT. */
static inline int cpudesc_socket_valid(unsigned width, unsigned previous, unsigned cores,
                                       unsigned enabled, unsigned threads)
{
    return (!previous || previous == width) && cores && cores <= width && enabled == cores &&
           threads == cores;
}
#endif
