/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Protocol expectations: TLFS HV_MESSAGE; OpenVMM VMBus protocol.rs;
 * docs.kernel.org/virt/hyperv/vmbus.html (ring header, trailer and wrap). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host/amd64/vmbus.h"

static struct vmbus_ring ring;
static unsigned char data[4096];
static void reset(void)
{
    memset(&ring, 0, sizeof(ring));
    memset(data, 0, sizeof(data));
}
static void packet(unsigned char *p, unsigned bytes)
{
    memset(p, 0, bytes);
    const struct vmbus_packet h = {6, 2, bytes / 8, 1, UINT64_C(0x123456789abcdef0)};
    memcpy(p, &h, sizeof(h));
    for (unsigned i = 16; i < bytes; i++)
        p[i] = i;
}
int main(void)
{
    assert(sizeof(struct vmbus_packet) == 16);
    assert(sizeof(struct vmbus_ring) == 4096);
    assert(offsetof(struct vmbus_ring, features) == 64);
    uint64_t pfns[27];
    for (unsigned i = 0; i < 27; i++)
        pfns[i] = UINT64_C(0x123450) + i;
    unsigned char gpadl[240];
    assert(vmbus_gpadl_header(gpadl, sizeof(gpadl), 42, 7, pfns, 6) == 76);
    const unsigned char prefix[] = {8, 0, 0,  0, 0, 0, 0, 0,  42, 0, 0, 0, 7, 0,
                                    0, 0, 56, 0, 1, 0, 0, 96, 0,  0, 0, 0, 0, 0};
    assert(memcmp(gpadl, prefix, sizeof(prefix)) == 0);
    assert(memcmp(gpadl + 28, pfns, 48) == 0);
    assert(vmbus_gpadl_header(gpadl, sizeof(gpadl), 42, 7, pfns, 26) == 236);
    assert(vmbus_gpadl_header(gpadl, sizeof(gpadl), 42, 7, pfns, 27) == 0);
    assert(vmbus_gpadl_header(gpadl, 75, 42, 7, pfns, 6) == 0);
    assert(vmbus_gpadl_header(gpadl, sizeof(gpadl), 42, 7, pfns, 0) == 0);
    pfns[0] = UINT64_MAX;
    assert(vmbus_gpadl_header(gpadl, sizeof(gpadl), 42, 7, pfns, 1) == 0);

    unsigned char p[32], out[64];
    unsigned bytes;
    uint64_t xid;
    uint16_t type;
    bool notify;
    packet(p, sizeof(p));
    reset();
    assert(vmbus_ring_write(&ring, data, sizeof(data), p, sizeof(p), &notify) == 1 && notify);
    assert(ring.write == 40 && memcmp(data, p, sizeof(p)) == 0);
    uint64_t trailer;
    memcpy(&trailer, data + 32, 8);
    assert(trailer == 0);
    assert(vmbus_ring_read(&ring, data, sizeof(data), out, sizeof(out), &bytes, &xid, &type,
                           &notify) == 1);
    assert(bytes == 16 && xid == UINT64_C(0x123456789abcdef0) && type == 6);
    assert(memcmp(out, p + 16, 16) == 0 && ring.read == 40);
    assert(vmbus_ring_read(&ring, data, sizeof(data), out, sizeof(out), &bytes, &xid, &type,
                           &notify) == 0);
    reset();
    ring.read = ring.write = 4080;
    assert(vmbus_ring_write(&ring, data, sizeof(data), p, sizeof(p), &notify) == 1 && notify);
    assert(ring.write == 24);
    memcpy(&trailer, data + 16, 8);
    assert(trailer == (UINT64_C(4080) << 32));
    assert(vmbus_ring_read(&ring, data, sizeof(data), out, sizeof(out), &bytes, &xid, &type,
                           &notify) == 1);
    assert(ring.read == 24 && bytes == 16 && memcmp(out, p + 16, 16) == 0);
    reset();
    ring.read = 40; /* exactly 40 free: the empty/full sentinel forbids write */
    assert(vmbus_ring_write(&ring, data, sizeof(data), p, sizeof(p), &notify) == 0 &&
           ring.write == 0);
    reset();
    ring.interrupt_mask = 1;
    assert(vmbus_ring_write(&ring, data, sizeof(data), p, sizeof(p), &notify) == 1 && !notify);
    reset();
    ring.write = 8; /* descriptor is incomplete */
    assert(vmbus_ring_read(&ring, data, sizeof(data), out, sizeof(out), &bytes, &xid, &type,
                           &notify) == -1);
    reset();
    memcpy(data, p, 32);
    ring.write = 32; /* trailer missing */
    assert(vmbus_ring_read(&ring, data, sizeof(data), out, sizeof(out), &bytes, &xid, &type,
                           &notify) == -1);
    ring.write = 40;
    ((struct vmbus_packet *)data)->offset8 = 1;
    assert(vmbus_ring_read(&ring, data, sizeof(data), out, sizeof(out), &bytes, &xid, &type,
                           &notify) == -1 &&
           ring.read == 0);
    ((struct vmbus_packet *)data)->offset8 = 5;
    assert(vmbus_ring_read(&ring, data, sizeof(data), out, sizeof(out), &bytes, &xid, &type,
                           &notify) == -1);
    reset();
    memcpy(data, p, 32);
    ring.write = 40;
    assert(vmbus_ring_read(&ring, data, sizeof(data), out, 15, &bytes, &xid, &type, &notify) ==
               -1 &&
           ring.read == 0);
    reset();
    ring.write = 4096;
    assert(vmbus_ring_read(&ring, data, sizeof(data), out, sizeof(out), &bytes, &xid, &type,
                           &notify) == -1);
    /* Reading across the host's pending-send threshold requests a wakeup. */
    reset();
    ring.read = 0;
    ring.write = 40;
    ring.pending_send = 4080;
    memcpy(data, p, 32);
    assert(vmbus_ring_read(&ring, data, sizeof(data), out, sizeof(out), &bytes, &xid, &type,
                           &notify) == 1 &&
           notify);
    puts("Hyper-V VMBus ring and GPADL tests passed");
    return 0;
}
