/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64_IA32_SCALAR_H
#define IA64_IA32_SCALAR_H
#include "ia64/ia32/run.h"
bool ia32_scalar_supported(const struct ia32_decoded *);
/* Synchronous scalar device RMW. Unsupported encodings set handled=false
 * before effects. The caller owns EIP/RF advancement and completed traps. */
struct ia64_result ia32_scalar(struct vcpu *, struct ia32_native_state *,
                               const struct ia32_decoded *, const uint8_t *, bool *handled,
                               struct ia32_interruption *);
#endif
