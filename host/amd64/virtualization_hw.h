/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_AMD64_VIRTUALIZATION_HW_H
#define IA64VMM_AMD64_VIRTUALIZATION_HW_H
#include "host/amd64/ia32.h"

struct amd64_virtualization_hw {
    void *page;
    uint64_t saved_efer, saved_hsave, saved_cr0, saved_cr4;
    uint32_t tags;
    bool invept_single;
};
bool amd64_virtualization_hw_enable(enum ia32_backend_kind, struct amd64_virtualization_hw *,
                                    const char **why);
void amd64_virtualization_hw_disable(enum ia32_backend_kind, struct amd64_virtualization_hw *);
bool amd64_virtualization_hw_invalidate(const struct amd64_virtualization_hw *, uint32_t tag,
                                        uint64_t eptp);
#endif
