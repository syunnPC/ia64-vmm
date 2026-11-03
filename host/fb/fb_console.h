/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_FB_CONSOLE_H
#define VMM_FB_CONSOLE_H
#include "ia64vmm/bootinfo.h"
/* Initialize after the HAL page allocator. A RAM cell grid survives guest
 * handover; invalid geometry or allocation failure leaves the sink disabled. */
void fb_console_early(const struct boot_framebuffer *fb);
void fb_console_clear(void);
void fb_console_init(const struct boot_framebuffer *fb);
void fb_console_release(void);
void fb_console_reclaim(void);
/* After the card's VBIOS set text
 * mode 3, the same console draws into the VGA text buffer (columns x lines
 * 16-bit cells) instead of the framebuffer, until the next fb_console_init. */
void fb_console_text(volatile uint16_t *buffer, unsigned columns, unsigned lines);
/* host/fb/vga_text.c: one cell, character and attribute; never reads. */
void vga_text_cell(volatile uint16_t *buffer, unsigned columns, unsigned x, unsigned y, unsigned ch,
                   bool cursor);
#endif
