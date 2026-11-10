/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64_IA32_STRING_H
#define IA64_IA32_STRING_H
#include "ia64/ia32/run.h"

/* Execute one MOVS/STOS/LODS/CMPS/SCAS iteration through D-side operand
 * accesses. F2/F3 are the decoder's effective repeat bits in intercept_code.
 * Every operand is prepared before any access. A remaining repeat count
 * retains EIP and sets RF; completion advances EIP and clears RF. The caller
 * handles full-instruction TF/PSR.ss traps; this helper handles partial-REP
 * single-step and operand breakpoint traps, matching ia32_io. */
struct ia64_result ia32_string(struct vcpu *, struct ia32_native_state *,
                               const struct ia32_decoded *, struct ia32_interruption *);
#endif
