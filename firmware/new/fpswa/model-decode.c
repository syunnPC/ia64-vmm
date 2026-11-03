/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <stdbool.h>
/* The decoder uses no host types or services. */
#define IA64VMM_TYPES_H
#define IA64VMM_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#include "../../../ia64/decode/decode.c"
#include "../../../ia64/decoder.c"
