/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/platform.h"
#include "ia64vmm/string.h"
#include "ia64vmm/log.h"

static bool uart_present;
static uint32_t debug_flags;
bool platform_debug_uart_present(void)
{
    return uart_present;
}
uint32_t platform_debug_flags(void)
{
    return debug_flags;
}
void platform_debug_display_policy(const struct ia64vmm_handoff *h)
{
    debug_flags = h->flags;
    /* Intel gen6 leaves GOP only when VGA is the console (firmware: VGA
     * primary needs a non-console UART). A console UART keeps the GOP and
     * the serial-to-screen mirror remains its kernel display. */
    vmm_guest_serial_configure(
        !!(h->flags & IA64VMM_HF_DEBUG_SCREEN),
        (h->flags & IA64VMM_HF_LEGACY_VGA) &&
            (h->vga_kind == IA64VMM_VGA_BOCHS || h->vga_kind == IA64VMM_VGA_VMWARE ||
             (h->vga_kind == IA64VMM_VGA_INTEL_GEN6 && !(h->flags & IA64VMM_HF_UART_CONSOLE))));
}
bool platform_debug_policy(const char *line, struct ia64vmm_handoff *h)
{
    const char *value;
    size_t n;
    h->flags &= ~(IA64VMM_HF_UART_PRESENT | IA64VMM_HF_UART_CONSOLE | IA64VMM_HF_DEBUG_SCREEN);
    uart_present = false;
    if (platform_cmdline_value(line, "debug_serial", &value, &n)) {
        if (n == 7 && !strncmp(value, "console", n))
            h->flags |= IA64VMM_HF_UART_PRESENT | IA64VMM_HF_UART_CONSOLE;
        else if (n == 6 && !strncmp(value, "device", n))
            h->flags |= IA64VMM_HF_UART_PRESENT;
        else if (n != 3 || strncmp(value, "off", n)) {
            LOGE("debug_serial must be off, device or console");
            return false;
        }
    }
    if (platform_cmdline_value(line, "debug_screen", &value, &n)) {
        if (n != 1 || (*value != '0' && *value != '1')) {
            LOGE("debug_screen must be 0 or 1");
            return false;
        }
        if (*value == '1')
            h->flags |= IA64VMM_HF_DEBUG_SCREEN;
    }
    uart_present = (h->flags & IA64VMM_HF_UART_PRESENT) != 0;
    platform_debug_display_policy(h);
    LOGI("debug_serial=%s debug_screen=%u",
         h->flags & IA64VMM_HF_UART_CONSOLE ? "console"
         : uart_present                     ? "device"
                                            : "off",
         !!(h->flags & IA64VMM_HF_DEBUG_SCREEN));
    return true;
}
