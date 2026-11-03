/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_UNALIGNED_H
#define VMM_UNALIGNED_H
#include <stdbool.h>
#include <stdint.h>
struct unaligned_frame {
    uint64_t gr[128], nat, nat_hi, ipsr, isr, iip, ifa, cfm, unat;
    /* stf.spill representation: significand, then sign:17/exponent:16..0. */
    uint64_t fp[128][2];
};
bool unaligned_m_slot(unsigned template_code, unsigned slot);
bool unaligned_emulate(struct unaligned_frame *frame, uint64_t instruction,
                       volatile uint8_t *memory);
void unaligned_dispatch(uint64_t vector, uint64_t *context);
#endif
