/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Legacy VGA: the VGA text console the
 * new firmware draws when the VMM POSTed the card's legacy VBIOS. Device
 * accesses go through vga-text.h's accessors, so tests/firmware/legacy_bios.c
 * runs this code against a register model. */
#ifndef VMM_LEGACY_BIOS_H
#define VMM_LEGACY_BIOS_H
#include "vga-text.h"
#include "legacy-handoff.h"

/* The 80x25 text mode POST left (mode 3, color: CRTC at 3D4h/3D5h). A RAM
 * shadow of the cells keeps scrolling free of device-memory reads; the
 * hardware cursor follows the output position (IBM VGA Technical
 * Reference: CRTC 0Ah cursor start, bit 5 off; 0Eh/0Fh cursor location). */
#define LEGACY_TEXT_COLUMNS 80u
#define LEGACY_TEXT_ROWS 25u
struct legacy_text {
    uint64_t cells;   /* address of the B8000h buffer */
    uint64_t io_base; /* sparse legacy I/O base */
    uint16_t shadow[LEGACY_TEXT_ROWS][LEGACY_TEXT_COLUMNS];
    unsigned column, row;
    uint8_t attribute;
    bool cursor_visible;
};

/* EFI text is UCS-2; the card's font is code page 437. ASCII maps to
 * itself; the EFI box-drawing, block and arrow characters (UEFI 2.x
 * SIMPLE_TEXT_OUTPUT "Unicode drawing characters") to their CP437 cells;
 * anything else to '?'. */
static inline uint8_t legacy_text_cp437(uint16_t ch)
{
    static const uint16_t from[] = {
        0x2500, 0x2502, 0x250c, 0x2510, 0x2514, 0x2518, 0x251c, 0x2524, 0x252c, 0x2534, 0x253c,
        0x2550, 0x2551, 0x2554, 0x2557, 0x255a, 0x255d, 0x2560, 0x2563, 0x2566, 0x2569, 0x256c,
        0x2588, 0x2591, 0x25b2, 0x25ba, 0x25bc, 0x25c4, 0x2190, 0x2191, 0x2192, 0x2193, 0x00b7,
    };
    static const uint8_t to[] = {
        0xc4, 0xb3, 0xda, 0xbf, 0xc0, 0xd9, 0xc3, 0xb4, 0xc2, 0xc1, 0xc5,
        0xcd, 0xba, 0xc9, 0xbb, 0xc8, 0xbc, 0xcc, 0xb9, 0xcb, 0xca, 0xce,
        0xdb, 0xb0, 0x1e, 0x10, 0x1f, 0x11, 0x1b, 0x18, 0x1a, 0x19, 0xfa,
    };
    if (ch >= 0x20 && ch < 0x7f)
        return (uint8_t)ch;
    for (unsigned i = 0; i < sizeof(from) / sizeof(from[0]); ++i)
        if (from[i] == ch)
            return to[i];
    return '?';
}
static inline void legacy_text_cell(struct legacy_text *t, unsigned column, unsigned row,
                                    uint16_t cell)
{
    t->shadow[row][column] = cell;
    legacy_vga_wr16(t->cells + 2 * ((uint64_t)row * LEGACY_TEXT_COLUMNS + column), cell);
}
static inline void legacy_text_cursor(struct legacy_text *t)
{
    unsigned at = t->row * LEGACY_TEXT_COLUMNS + t->column;
    uint8_t start = legacy_vga_indexed_read(t->io_base, 0x3d4, 0x3d5, 0x0a);
    legacy_vga_indexed_write(t->io_base, 0x3d4, 0x3d5, 0x0a,
                             (uint8_t)(t->cursor_visible ? start & ~0x20u : start | 0x20u));
    legacy_vga_indexed_write(t->io_base, 0x3d4, 0x3d5, 0x0e, (uint8_t)(at >> 8));
    legacy_vga_indexed_write(t->io_base, 0x3d4, 0x3d5, 0x0f, (uint8_t)at);
}
static inline void legacy_text_clear(struct legacy_text *t)
{
    uint16_t blank = (uint16_t)(t->attribute << 8 | ' ');
    for (unsigned row = 0; row < LEGACY_TEXT_ROWS; ++row)
        for (unsigned column = 0; column < LEGACY_TEXT_COLUMNS; ++column)
            legacy_text_cell(t, column, row, blank);
    t->column = t->row = 0;
    legacy_text_cursor(t);
}
static inline void legacy_text_scroll(struct legacy_text *t)
{
    for (unsigned row = 1; row < LEGACY_TEXT_ROWS; ++row)
        for (unsigned column = 0; column < LEGACY_TEXT_COLUMNS; ++column)
            legacy_text_cell(t, column, row - 1, t->shadow[row][column]);
    for (unsigned column = 0; column < LEGACY_TEXT_COLUMNS; ++column)
        legacy_text_cell(t, column, LEGACY_TEXT_ROWS - 1, (uint16_t)(t->attribute << 8 | ' '));
}
static inline void legacy_text_newline(struct legacy_text *t)
{
    t->column = 0;
    if (t->row + 1 < LEGACY_TEXT_ROWS)
        ++t->row;
    else
        legacy_text_scroll(t);
}
/* One UCS-2 character with EFI OutputString semantics: CR, LF, BS; a
 * character in the last column wraps to the next line. */
static inline void legacy_text_put(struct legacy_text *t, uint16_t ch)
{
    if (ch == '\r')
        t->column = 0;
    else if (ch == '\n') {
        if (t->row + 1 < LEGACY_TEXT_ROWS)
            ++t->row;
        else
            legacy_text_scroll(t);
    } else if (ch == '\b') {
        if (t->column)
            --t->column;
    } else {
        legacy_text_cell(t, t->column, t->row,
                         (uint16_t)(t->attribute << 8 | legacy_text_cp437(ch)));
        if (++t->column == LEGACY_TEXT_COLUMNS)
            legacy_text_newline(t);
    }
}
#endif
