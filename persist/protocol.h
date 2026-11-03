/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_PERSIST_PROTOCOL_H
#define VMM_PERSIST_PROTOCOL_H
#include "ia64vmm/types.h"
struct xhci_trb {
    uint64_t parameter;
    uint32_t status, control;
};
struct xhci_ring {
    struct xhci_trb *trbs;
    uint64_t physical;
    unsigned next, cycle, first, first_cycle;
    bool building;
};
void xhci_begin(struct xhci_ring *);
void xhci_commit(struct xhci_ring *);
uint64_t xhci_push(struct xhci_ring *, uint64_t, uint32_t, uint32_t);
int xhci_completion(const struct xhci_trb *, unsigned type, uint64_t pointer, unsigned slot,
                    unsigned endpoint, unsigned length);
enum bot_result { BOT_PASSED, BOT_FAILED, BOT_RESET };
/* transfer returns byte count, -2 for stall, -1 for other transport errors.
 * reset performs BOT class reset, then IN/OUT CLEAR_FEATURE in that order. */
struct bot_io {
    int (*transfer)(bool in, void *, unsigned length);
    bool (*clear)(unsigned pipe);
    bool (*reset)(void);
};
enum bot_result bot_execute(const struct bot_io *, uint32_t, const uint8_t *, unsigned, void *,
                            unsigned, bool, unsigned *actual);
/* SPC bounded UNIT ATTENTION/not-ready retries; key is the last sense key. */
bool scsi_execute(const struct bot_io *, uint32_t *tag, const uint8_t *, unsigned, void *, unsigned,
                  bool, unsigned *actual, void (*wait)(unsigned), unsigned *key);
/* A direct-access disk with 512-byte blocks behind a BOT transport
 * (persist/sbc.c). The transport refuses I/O once its controller stops. */
struct scsi_disk {
    const struct bot_io *io;
    void (*wait)(unsigned ms);
    const char *who;
    uint32_t tag;
    uint64_t sectors;
    bool wce, fua;
};
/* INQUIRY, TEST UNIT READY, the caching mode page, READ CAPACITY(10). */
bool scsi_disk_start(struct scsi_disk *);
/* A writable medium with write-through, FUA writes or SYNCHRONIZE CACHE;
 * else WCE turned off. */
bool scsi_disk_caching(struct scsi_disk *);
/* At most 32 blocks inside the capacity. */
bool scsi_disk_read(struct scsi_disk *, uint64_t lba, unsigned count, void *);
bool scsi_disk_write(struct scsi_disk *, uint64_t lba, unsigned count, const void *);
bool scsi_disk_flush(struct scsi_disk *);
/* SBC caching mode page from a MODE SENSE(6) response. */
bool scsi_caching(const uint8_t *, unsigned, bool *wce, bool *fua);
bool bot_cbw(uint8_t[31], uint32_t, const uint8_t *, unsigned, uint32_t, bool);
enum bot_result bot_csw(const uint8_t *, unsigned, uint32_t, uint32_t, uint32_t);
bool scsi_range(uint8_t[10], bool, uint64_t, unsigned);
/* The first Mass Storage BOT interface (class 08h, subclass 06h, protocol
 * 50h, alternate setting 0) of a configuration descriptor set of length n;
 * [0] its bulk IN, [1] its bulk OUT endpoint. */
struct usb_bot {
    unsigned configuration, interface, address[2], mps[2], burst[2];
};
bool usb_bot_interface(const uint8_t *, unsigned n, struct usb_bot *);
uint32_t plog_crc(const void *, size_t);
/* CRC of the two consecutive spans of a circular record. */
uint32_t plog_crc_pair(const void *, size_t, const void *, size_t);
#endif
