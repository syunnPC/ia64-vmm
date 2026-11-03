/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64vmm/ia32_state.h"

enum ia32_state_status ia32_make_record(const struct ia32_record_input *in,
                                        struct ia32_interruption *out)
{
    *out = (struct ia32_interruption){0};
    bool complete = in->phase == IA32_PHASE_COMPLETED;
    if ((in->phase != IA32_PHASE_BEFORE && !complete && in->phase != IA32_PHASE_PARTIAL) ||
        (complete && !in->next_valid))
        return IA32_STATE_UNREPRESENTABLE;
    struct ia32_interruption r = {.valid = true,
                                  .phase = in->phase,
                                  .iip = complete ? in->next.value : in->source.value,
                                  .iipa = in->source.value,
                                  .iipa_valid = true,
                                  .ivt = 0x6a00};
    unsigned vector = in->vector;
    uint16_t code = in->code;
    switch (in->kind) {
    case IA32_RECORD_EXCEPTION:
        r.ivt = 0x6900;
        if (vector == 1) {
            if (complete) {
                if (code & ~0xfc || in->execute)
                    return IA32_STATE_UNREPRESENTABLE;
                r.iipa_valid = (code & 4) != 0;
            } else if (code || !in->execute)
                return IA32_STATE_UNREPRESENTABLE;
        } else if (vector == 3 || vector == 4) {
            if (!complete || (code & ~0xfc))
                return IA32_STATE_UNREPRESENTABLE;
        } else {
            bool fault = vector == 0 || vector == 5 || vector == 7 || vector == 11 ||
                         vector == 12 || vector == 13 || vector == 16 || vector == 17 ||
                         vector == 19;
            if (!fault || complete || in->execute || (code && (vector < 11 || vector > 13)))
                return IA32_STATE_UNREPRESENTABLE;
        }
        if (vector == 17) {
            if (!in->ifa_valid)
                return IA32_STATE_UNREPRESENTABLE;
            r.ifa_valid = true;
            r.ifa = in->ifa;
        }
        if (in->execute)
            r.isr |= UINT64_C(1) << 32;
        break;
    case IA32_RECORD_SOFTWARE_INT:
        if (!complete || (code & ~0xfc))
            return IA32_STATE_UNREPRESENTABLE;
        r.ivt = 0x6b00;
        break;
    case IA32_RECORD_INSTRUCTION:
        if (in->phase != IA32_PHASE_BEFORE || (code & ~0xf3fe) ||
            ((code & 0x40) && ((code >> 7) & 7) > 5))
            return IA32_STATE_UNREPRESENTABLE;
        vector = 0;
        r.iim_valid = true;
        r.iim = in->iim;
        break;
    case IA32_RECORD_GATE:
        if (!complete || (code & ~0x40fc) || !in->ifa_valid || in->ifa > 0xffff)
            return IA32_STATE_UNREPRESENTABLE;
        vector = 1;
        r.iim_valid = r.ifa_valid = true;
        r.iim = in->iim;
        r.ifa = in->ifa;
        break;
    case IA32_RECORD_SYSTEM_FLAG:
        /* The qualified AMD pre-notification has no committed traps. */
        if ((in->phase != IA32_PHASE_BEFORE && !complete) || (code & ~0xc0fc) ||
            (!complete && (code & 0xfc)) || in->iim > UINT32_MAX)
            return IA32_STATE_UNREPRESENTABLE;
        vector = 2;
        r.iim_valid = true;
        r.iim = in->iim;
        break;
    case IA32_RECORD_LOCK:
        if (in->phase != IA32_PHASE_BEFORE || code || !in->ifa_valid || !in->read || !in->write)
            return IA32_STATE_UNREPRESENTABLE;
        vector = 4;
        r.ifa_valid = true;
        r.ifa = in->ifa;
        r.isr = (UINT64_C(1) << 33) | (UINT64_C(1) << 34);
        break;
    default:
        return IA32_STATE_UNREPRESENTABLE;
    }
    r.isr |= (uint64_t)vector << 16 | code;
    *out = r;
    return IA32_STATE_OK;
}
