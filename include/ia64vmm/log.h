/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VMM log. One RAM ring, replicated to sinks: the HAL debug
 * sink (serial / stderr) always, the framebuffer console while the VMM owns
 * the framebuffer. Formatting supports the printf subset
 * %s %c %d %i %u %x %X %p %% with l/ll/z length modifiers, field width and
 * '0'/'-' flags. Safe to call from any VMM context; never allocates.
 */
#ifndef IA64VMM_LOG_H
#define IA64VMM_LOG_H

#include <stdarg.h>
#include "ia64vmm/types.h"

enum log_level { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };

void vmm_log(enum log_level level, const char *fmt, ...) IA64VMM_PRINTF(2, 3);
void vmm_vlog(enum log_level level, const char *fmt, va_list ap);

/* Unformatted text, e.g. guest console output forwarded by the firmware
 * service window. Lines from the guest are prefixed "[guest] " by core. */
void vmm_log_guest_text(const char *s, size_t n);
/* Diagnostics (hosted --run-stats): called on the writer's thread after each
 * complete guest text line has been logged. */
void vmm_log_set_guest_line_hook(void (*hook)(const char *line, size_t n));
/* Only UART TX enters this byte-exact ring; firmware diagnostics remain
 * in the ordinary log. Sinks drain on the management CPU, never the vCPU. */
void vmm_guest_serial_write(const char *s, size_t n);
void vmm_guest_serial_configure(bool screen, bool legacy_vga);
bool vmm_guest_serial_screen_enabled(void);
/* Management-only post-exit display of the retained serial tail. */
void vmm_guest_serial_show_tail(void);
/* Copy from a monotonic cursor; lost reports bytes overwritten before it. */
size_t vmm_guest_serial_read(uint64_t *cursor, char *data, size_t size, uint64_t *lost);

int vmm_snprintf(char *buf, size_t size, const char *fmt, ...) IA64VMM_PRINTF(3, 4);
int vmm_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* Framebuffer sink control: the VMM stops
 * drawing when the framebuffer is handed to the guest and resumes on
 * failure or after the guest stops. */
void vmm_log_fb_enable(bool enable);
/* Pre-guest management context: preserve sink state across PCI ROM reads. */
bool vmm_log_fb_enabled(void);

#define LOGE(...) vmm_log(LOG_ERROR, __VA_ARGS__)
#define LOGW(...) vmm_log(LOG_WARN, __VA_ARGS__)
#define LOGI(...) vmm_log(LOG_INFO, __VA_ARGS__)
#define LOGD(...) vmm_log(LOG_DEBUG, __VA_ARGS__)

#endif
