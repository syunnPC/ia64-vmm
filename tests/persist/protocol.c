/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xHCI 1.2 §4.9.2/6.4; USB MSC BOT 1.0 §4.3/4.4/5.1/5.2/6.7; USB 2.0 §9.6
 * descriptors; SBC READ/WRITE(10): big-endian LBA and block count, no zero
 * blocks. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "persist/protocol.h"

int main(void)
{
    uint8_t cbw[31], cdb[10] = {0x2a, 0, 0, 0, 0x12, 0x34, 0, 0, 2, 0};
    assert(bot_cbw(cbw, 7, cdb, 10, 1024, false));
    assert(!memcmp(cbw, "USBC\7\0\0\0\0\4\0\0\0\0\12", 15));
    assert(!memcmp(cbw + 15, cdb, 10));
    assert(!bot_cbw(cbw, 7, cdb, 0, 0, false));
    assert(!bot_cbw(cbw, 7, cdb, 17, 0, false));
    uint8_t csw[13] = {'U', 'S', 'B', 'S', 7};
    assert(bot_csw(csw, 13, 7, 1024, 1024) == BOT_PASSED);
    csw[0] ^= 1;
    assert(bot_csw(csw, 13, 7, 1024, 1024) == BOT_RESET);
    csw[0] ^= 1;
    assert(bot_csw(csw, 12, 7, 1024, 1024) == BOT_RESET);
    assert(bot_csw(csw, 13, 8, 1024, 1024) == BOT_RESET);
    csw[12] = 1;
    assert(bot_csw(csw, 13, 7, 1024, 1024) == BOT_FAILED);
    csw[12] = 2;
    assert(bot_csw(csw, 13, 7, 1024, 1024) == BOT_RESET);
    csw[12] = 3;
    assert(bot_csw(csw, 13, 7, 1024, 1024) == BOT_RESET);
    csw[12] = 0;
    csw[9] = 8;
    assert(bot_csw(csw, 13, 7, 1024, 1024) == BOT_RESET);
    csw[9] = 0;
    assert(bot_csw(csw, 13, 7, 1024, 512) == BOT_RESET);
    csw[9] = 2;
    assert(bot_csw(csw, 13, 7, 1024, 512) == BOT_PASSED);
    assert(scsi_range(cdb, true, 0x12345678, 32));
    const uint8_t expected[10] = {0x2a, 0, 0x12, 0x34, 0x56, 0x78, 0, 0, 32, 0};
    assert(!memcmp(cdb, expected, 10));
    assert(!scsi_range(cdb, true, UINT32_MAX, 2));
    assert(!scsi_range(cdb, true, 0, 0));
    struct xhci_trb trbs[256] = {0};
    struct xhci_ring r = {.trbs = trbs, .physical = 0x10000, .cycle = 1};
    for (unsigned i = 0; i < 255; ++i) {
        uint64_t pa = xhci_push(&r, 0x1234, 512, 1u << 10 | 1u << 5);
        assert(pa == 0x10000 + i * 16 && trbs[i].control == ((1u << 10) | (1u << 5) | 1));
    }
    assert(r.next == 0 && r.cycle == 0);
    assert(trbs[255].parameter == 0x10000 && trbs[255].control == ((6u << 10) | 3));
    assert(xhci_push(&r, 0, 0, 9u << 10) == 0x10000);
    assert(trbs[0].control == 9u << 10);
    xhci_begin(&r);
    xhci_push(&r, 0, 8, 2u << 10 | 1u << 6);
    xhci_push(&r, 0, 512, 3u << 10);
    xhci_push(&r, 0, 0, 4u << 10 | 1u << 5);
    assert((trbs[1].control & 1) == 1); /* First TRB withheld, PCS=0. */
    assert((trbs[2].control & 1) == 0);
    xhci_commit(&r);
    assert((trbs[1].control & 1) == 0);
    struct xhci_trb event = {0x12340, 1u << 24, 32u << 10 | 3u << 16 | 1u << 24 | 1};
    assert(xhci_completion(&event, 32, 0x12340, 1, 3, 512) == 512);
    assert(xhci_completion(&event, 32, 0x12350, 1, 3, 512) == -1);
    event.status = 13u << 24 | 12;
    assert(xhci_completion(&event, 32, 0x12340, 1, 3, 512) == 500);
    event.status = 13u << 24 | 513;
    assert(xhci_completion(&event, 32, 0x12340, 1, 3, 512) == -1);
    event.status = 6u << 24;
    assert(xhci_completion(&event, 32, 0x12340, 1, 3, 512) == -2);

    /* USB 2.0 §9.6.3/§9.6.5/§9.6.6, USB 3.2 §9.6.7, BOT 1.0 §4.3/§4.4:
     * the first alternate-setting-0 interface 08h/06h/50h and its bulk pair. */
    struct usb_bot u;
    uint8_t set[128] = {9,    2, 32, 0, 1,    7, 0, 0x80, 50, 9, 4, 0,    0, 2, 8, 6,
                        0x50, 0, 7,  5, 0x81, 2, 0, 2,    0,  7, 5, 0x02, 2, 0, 2, 0};
    assert(usb_bot_interface(set, 32, &u) && u.configuration == 7 && u.interface == 0);
    assert(u.address[0] == 0x81 && u.address[1] == 0x02 && u.mps[0] == 512 && u.mps[1] == 512);
    assert(!u.burst[0] && !u.burst[1]);
    assert(!usb_bot_interface(set, 31, &u)); /* wTotalLength differs */
    assert(!usb_bot_interface(set, 8, &u));
    set[1] = 4;
    assert(!usb_bot_interface(set, 32, &u));
    set[1] = 2;
    set[25] = 8;
    assert(!usb_bot_interface(set, 32, &u));
    set[25] = 7; /* past the end */
    set[25] = 1;
    assert(!usb_bot_interface(set, 32, &u));
    set[25] = 7;
    set[16] = 0x62;
    assert(!usb_bot_interface(set, 32, &u));
    set[16] = 0x50; /* UAS */
    set[15] = 1;
    assert(!usb_bot_interface(set, 32, &u));
    set[15] = 6;
    set[14] = 3;
    assert(!usb_bot_interface(set, 32, &u));
    set[14] = 8;
    set[12] = 1;
    assert(!usb_bot_interface(set, 32, &u));
    set[12] = 0; /* alternate 1 */
    set[27] = 0x82;
    assert(!usb_bot_interface(set, 32, &u));
    set[27] = 0x02; /* two IN */
    set[27] = 0x00;
    assert(!usb_bot_interface(set, 32, &u));
    set[27] = 0x02; /* endpoint 0 */
    set[27] = 0x12;
    assert(!usb_bot_interface(set, 32, &u));
    set[27] = 0x02; /* reserved bits */
    set[28] = 3;
    assert(!usb_bot_interface(set, 32, &u));
    set[28] = 2; /* interrupt only */
    set[29] = set[30] = 0;
    assert(!usb_bot_interface(set, 32, &u));
    set[29] = 1;
    set[30] = 8;
    assert(!usb_bot_interface(set, 32, &u));
    set[29] = 0x40;
    set[30] = 0;
    assert(usb_bot_interface(set, 32, &u) && u.mps[1] == 64);
    /* Another interface first, a second BOT interface after: interrupt and
     * foreign endpoints are ignored. */
    const uint8_t mixed[] = {9,    2, 64,   0, 3,    1, 0, 0x80, 50, 9, 4, 0,    0, 1, 3,    1,
                             1,    0, 7,    5, 0x83, 3, 8, 0,    10, 9, 4, 1,    0, 3, 8,    6,
                             0x50, 0, 7,    5, 0x85, 3, 8, 0,    10, 7, 5, 0x02, 2, 0, 2,    0,
                             7,    5, 0x84, 2, 0,    2, 0, 9,    4,  2, 0, 2,    8, 6, 0x50, 0};
    memcpy(set, mixed, sizeof(mixed));
    assert(usb_bot_interface(set, 64, &u) && u.interface == 1 && u.configuration == 1);
    assert(u.address[0] == 0x84 && u.address[1] == 0x02);
    /* SuperSpeed companion: bMaxBurst for the preceding endpoint; no streams. */
    const uint8_t super[] = {9, 2,    44, 0,    1, 1,    0, 0x80, 50, 9,  4,  0,  0, 2, 8,
                             6, 0x50, 0,  7,    5, 0x81, 2, 0,    4,  0,  6,  48, 3, 0, 0,
                             0, 7,    5,  0x02, 2, 0,    4, 0,    6,  48, 15, 0,  0, 0};
    memcpy(set, super, sizeof(super));
    assert(usb_bot_interface(set, 44, &u) && u.burst[0] == 3 && u.burst[1] == 15 &&
           u.mps[0] == 1024);
    set[41] = 2;
    assert(!usb_bot_interface(set, 44, &u));
    set[41] = 0;
    set[40] = 16;
    assert(!usb_bot_interface(set, 44, &u));
    set[40] = 15;
    /* An interrupt endpoint's companion is not the bulk endpoint's. */
    const uint8_t companion[] = {9, 2,    45, 0,  1, 1,    0, 0x80, 50, 9, 4,    0, 0,    3, 8,
                                 6, 0x50, 0,  7,  5, 0x81, 2, 0,    4,  0, 7,    5, 0x83, 3, 8,
                                 0, 1,    6,  48, 9, 0,    8, 0,    7,  5, 0x02, 2, 0,    4, 0};
    memcpy(set, companion, sizeof(companion));
    assert(usb_bot_interface(set, 45, &u) && u.burst[0] == 0 && u.burst[1] == 0);
    puts("PLOG protocol: passed");
}
