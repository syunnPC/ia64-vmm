/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_VMBUS_H
#define VMM_AMD64_VMBUS_H
#include "ia64vmm/types.h"

#define VMBUS_MAX_PFNS 32u
#define VMBUS_PACKET_INBAND 6u
#define VMBUS_PACKET_COMPLETION 11u
struct vmbus_offer {
    uint8_t type[16], instance[16];
    uint32_t relid, connection_id;
    uint16_t subchannel, flags;
    bool dedicated, rescinded;
};
struct vmbus_channel;
/* CPU0 only, no simultaneous callers. Up to eight primary channels.
 * close invalidates the handle. DMA buffers must be VMM-owned pages; retain
 * them after uncertain completion. PFNs describe exactly
 * ceil((data_offset + data_bytes)/4096) pages, <=32. */
bool amd64_vmbus_start(void);
unsigned amd64_vmbus_offer_count(void);
const struct vmbus_offer *amd64_vmbus_offer(unsigned index);
struct vmbus_channel *amd64_vmbus_open(const struct vmbus_offer *);
bool amd64_vmbus_close(struct vmbus_channel *);
bool amd64_vmbus_send(struct vmbus_channel *, const void *payload, unsigned bytes,
                      uint64_t transaction, const uint64_t *pfns, unsigned pfn_count,
                      unsigned data_offset, unsigned data_bytes, bool completion);
/* 1: packet, 0: empty, -1: channel or bus failed. bytes includes padding.
 * The caller bounds its wait and validates transaction, packet type and data.
 * Polling any channel also dispatches control messages for all channels. */
int amd64_vmbus_receive(struct vmbus_channel *, void *payload, unsigned capacity, unsigned *bytes,
                        uint64_t *transaction, uint16_t *type);
void amd64_vmbus_stop(void);

/* Wire helpers also used by hosted protocol tests. Ring data is separate from
 * its header; each index is measured from the first data byte. */
struct vmbus_packet {
    uint16_t type, offset8, length8, flags;
    uint64_t transaction;
} IA64VMM_PACKED;
struct vmbus_ring {
    uint32_t write, read, interrupt_mask, pending_send, reserved[12], features;
    uint8_t padding[4096 - 68];
};
unsigned vmbus_gpadl_header(void *out, unsigned capacity, uint32_t relid, uint32_t handle,
                            const uint64_t *pfns, unsigned count);
int vmbus_ring_write(struct vmbus_ring *, void *data, unsigned size, const void *packet,
                     unsigned bytes, bool *notify);
int vmbus_ring_read(struct vmbus_ring *, const void *data, unsigned size, void *out,
                    unsigned capacity, unsigned *bytes, uint64_t *transaction, uint16_t *type,
                    bool *notify);
#endif
