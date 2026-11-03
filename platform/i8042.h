/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_PLATFORM_I8042_H
#define IA64VMM_PLATFORM_I8042_H
#include "ia64vmm/types.h"

/* Configuration and lifecycle calls run with guest executors stopped.
 * irq only updates the guest IRQ1 input; it performs no host I/O. */
void platform_i8042_configure(bool enabled);
bool platform_i8042_configured(void);
bool platform_i8042_init(void (*irq)(void *, bool), void *context);
void platform_i8042_reset(void);
void platform_i8042_cleanup(void);
uint8_t platform_i8042_read(uint16_t port);
void platform_i8042_write(uint16_t port, uint8_t value);
/* One logical key event, in set 1 make codes without a break bit/prefix.
 * Returns false for invalid, inhibited, or dropped input. Pause has no break. */
bool platform_i8042_input(uint16_t makecode, bool released, bool e0, bool e1);
#endif
