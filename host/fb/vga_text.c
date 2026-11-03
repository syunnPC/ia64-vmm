/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VGA text backend of the VMM console. IBM VGA Technical Reference, alphanumeric modes: each cell of
 * the B8000h buffer is a 16-bit word, the character code in the low byte
 * and the attribute (background 7:4, foreground 3:0) in the high byte.
 * fb_console.c keeps the RAM cell grid; this backend only writes words,
 * never reads the buffer (device memory). */
#include "host/fb/fb_console.h"

#define ATTR_TEXT 0x07u   /* light grey on black */
#define ATTR_CURSOR 0x70u /* inverse: the software cursor cell */

void vga_text_cell(volatile uint16_t *buffer, unsigned columns, unsigned x, unsigned y, unsigned ch,
                   bool cursor)
{
    /* Same printable range as the framebuffer font; code page 437 glyphs
     * outside it would differ from what the serial mirror shows. */
    unsigned glyph = ch >= 0x20 && ch <= 0x7e ? ch : '?';
    buffer[(size_t)y * columns + x] = (uint16_t)(glyph | (cursor ? ATTR_CURSOR : ATTR_TEXT) << 8);
}
