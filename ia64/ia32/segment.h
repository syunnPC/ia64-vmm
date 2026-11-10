/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64_IA32_SEGMENT_H
#define IA64_IA32_SEGMENT_H
#include "ia64vmm/ia32_state.h"
enum ia32_segment_reference { IA32_SEGMENT_READ, IA32_SEGMENT_WRITE, IA32_SEGMENT_FETCH };
/* Vol.1 Table 6-4 integrity checks. Callers check base and offset/limit. */
bool ia32_segment_valid(const struct ia32_native_state *, unsigned segment,
                         enum ia32_segment_reference);
#endif
