/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_STORVSC_H
#define VMM_AMD64_STORVSC_H
#include "ia64vmm/types.h"

struct amd64_storvsc_device {
    uint64_t blocks;
    uint32_t sector_bytes;
    uint8_t target, lun, peripheral_type;
    bool readonly;
};
/* Synchronous management-CPU-only transport. NULL requires exactly one
 * primary synthetic SCSI controller; instance GUIDs use VMBus byte order.
 * Allocations are retained, including after timeout; start is one-shot. */
bool amd64_storvsc_start(const uint8_t instance[16]);
/* Probe and cache an explicit path-0 target/LUN. No disk writes occur while
 * probing. Callers verify the selected medium before issuing writes. */
const struct amd64_storvsc_device *amd64_storvsc_find(unsigned target, unsigned lun);
/* Optional bounded REPORT LUNS discovery of target 0. */
bool amd64_storvsc_enumerate(void);
unsigned amd64_storvsc_count(void);
const struct amd64_storvsc_device *amd64_storvsc_device(unsigned index);
bool amd64_storvsc_read(const struct amd64_storvsc_device *, uint64_t lba, unsigned blocks,
                        void *data);
bool amd64_storvsc_write(const struct amd64_storvsc_device *, uint64_t lba, unsigned blocks,
                         const void *data);
bool amd64_storvsc_flush(const struct amd64_storvsc_device *);
void amd64_storvsc_stop(void);
#endif
