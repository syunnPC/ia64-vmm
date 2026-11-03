/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Common freestanding types and compiler helpers for every host-side module
 * (boot, host, core, platform, ia64, engine). Never included by firmware/.
 */
#ifndef IA64VMM_TYPES_H
#define IA64VMM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t gpa_t; /* guest physical address (IA-64 view, bit 63 = UC already stripped) */
typedef uint64_t hpa_t; /* host physical address */

#define IA64VMM_PACKED __attribute__((packed))
#define IA64VMM_NORETURN _Noreturn
#define IA64VMM_UNUSED __attribute__((unused))
#define IA64VMM_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define IA64VMM_ALIGNED(n) __attribute__((aligned(n)))

#define IA64VMM_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IA64VMM_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define IA64VMM_ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))

#define IA64VMM_PAGE_SIZE 4096ULL

#endif
