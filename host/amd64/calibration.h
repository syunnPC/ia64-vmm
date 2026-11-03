/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_CALIBRATION_H
#define VMM_AMD64_CALIBRATION_H
#include "host/amd64/time_math.h"
struct amd64_clock_reference {
    uint32_t (*counter)(void *);
    uint64_t (*tsc)(void *);
    void *context;
};
static inline uint64_t amd64_calibration_hz(uint64_t ticks, uint64_t frequency, uint64_t elapsed)
{
    if (!ticks || !frequency || !elapsed)
        return 0;
    uint64_t hz = amd64_scale_ticks(ticks, frequency, elapsed, false);
    return hz >= 1000000 && hz <= UINT64_C(65536000000) ? hz : 0;
}
uint64_t amd64_measure_reference(const struct amd64_clock_reference *, uint32_t mask, uint64_t hz);
#endif
