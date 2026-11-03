/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/fb/fb_console.h"
#include "core/log_internal.h"
#include "ia64vmm/string.h"
#include "ia64vmm/hal.h"
#include "host/fb/font.h"

static struct boot_framebuffer screen;
static unsigned col, row, cols, rows, pixel_bytes;
static uint32_t foreground, background;
static bool active;
static uint8_t *cells;
static size_t cell_pages;
static uint8_t early_cells[65536];
/* The VGA text buffer while the text backend draws (vga_text.c). */
static volatile uint16_t *text;

static uint32_t channel(uint8_t value, unsigned bits, unsigned shift)
{
    return (uint32_t)(((uint64_t)value * ((UINT64_C(1) << bits) - 1) / 255) << shift);
}
static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return channel(r, screen.red_size, screen.red_shift) |
           channel(g, screen.green_size, screen.green_shift) |
           channel(b, screen.blue_size, screen.blue_shift);
}
static void pixel(unsigned x, unsigned y, uint32_t value)
{
    volatile uint8_t *p = screen.hva + (size_t)y * screen.pitch + x * pixel_bytes;
    for (unsigned i = 0; i < pixel_bytes; ++i)
        p[i] = value >> (i * 8);
}
static void cell(unsigned x, unsigned y, unsigned ch)
{
    if (text) {
        vga_text_cell(text, cols, x, y, ch, false);
        return;
    }
    unsigned index = ch >= 0x20 && ch <= 0x7e ? ch - 0x20 : 95;
    for (unsigned dy = 0; dy < 16; ++dy)
        for (unsigned dx = 0; dx < 8; ++dx)
            pixel(x * 8 + dx, y * 16 + dy,
                  console_font[index][dy] & (0x80 >> dx) ? foreground : background);
}
static void cursor(bool show)
{
    unsigned ch = cells[(size_t)row * cols + col];
    if (text) {
        vga_text_cell(text, cols, col, row, ch, show);
        return;
    }
    unsigned index = ch >= 0x20 && ch <= 0x7e ? ch - 0x20 : 95;
    for (unsigned x = 0; x < 8; ++x)
        pixel(col * 8 + x, row * 16 + 15,
              show || (console_font[index][15] & (0x80 >> x)) ? foreground : background);
}
static void redraw(void)
{
    for (unsigned y = 0; y < rows; ++y)
        for (unsigned x = 0; x < cols; ++x)
            cell(x, y, cells[(size_t)y * cols + x]);
}
static void newline(void)
{
    col = 0;
    if (++row < rows)
        return;
    /* Device-memory reads are prohibitively slow. Move only RAM cells,
     * amortizing a write-only redraw over a quarter-screen of new lines. */
    unsigned scroll = rows / 4;
    if (!scroll)
        scroll = 1;
    size_t kept = (size_t)(rows - scroll) * cols;
    memmove(cells, cells + (size_t)scroll * cols, kept);
    memset(cells + kept, ' ', (size_t)scroll * cols);
    row = rows - scroll;
    redraw();
}
static void flush_writes(void)
{
    /* Drain WC stores before ownership changes or a halt/screenshot. */
    __asm__ volatile("sfence" ::: "memory");
}
static void console_write(const char *s, size_t n)
{
    if (!active && !vmm_guest_serial_screen_enabled())
        return;
    cursor(false);
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') {
            newline();
            continue;
        }
        if (ch == '\r') {
            col = 0;
            continue;
        }
        if (ch == '\b') {
            if (col)
                --col;
            continue;
        }
        unsigned count = ch == '\t' ? 8 - (col & 7) : 1;
        for (unsigned j = 0; j < count; ++j) {
            cells[(size_t)row * cols + col] = ch == '\t' ? ' ' : ch;
            cell(col, row, cells[(size_t)row * cols + col]);
            if (++col == cols)
                newline();
        }
    }
    cursor(true);
    flush_writes();
}
static void init(const struct boot_framebuffer *fb, bool early)
{
    active = false;
    text = NULL;
    if (cells && cells != early_cells)
        hal_free_pages(cells, cell_pages);
    cells = NULL;
    cell_pages = 0;
    memset(&screen, 0, sizeof(screen));
    log_set_fb_sink(NULL);
    if (!fb || fb->format != BOOT_FB_RGB_MASKS || !fb->hva ||
        (fb->bpp != 16 && fb->bpp != 24 && fb->bpp != 32) || fb->width < 8 || fb->height < 16 ||
        fb->pitch < (uint64_t)fb->width * (fb->bpp / 8) ||
        fb->size < (uint64_t)fb->pitch * fb->height)
        return;
    const unsigned sizes[] = {fb->red_size, fb->green_size, fb->blue_size};
    const unsigned shifts[] = {fb->red_shift, fb->green_shift, fb->blue_shift};
    uint32_t used = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (!sizes[i] || sizes[i] > 8 || shifts[i] + sizes[i] > fb->bpp)
            return;
        uint32_t mask = (uint32_t)(((UINT64_C(1) << sizes[i]) - 1) << shifts[i]);
        if (used & mask)
            return;
        used |= mask;
    }
    size_t count = (size_t)(fb->width / 8) * (fb->height / 16);
    if (count > SIZE_MAX - (IA64VMM_PAGE_SIZE - 1))
        return;
    cell_pages = IA64VMM_ALIGN_UP(count, IA64VMM_PAGE_SIZE) / IA64VMM_PAGE_SIZE;
    if (early && count > sizeof(early_cells))
        return;
    cells = early ? early_cells : hal_alloc_pages(cell_pages, 0);
    if (!cells)
        return;
    memset(cells, ' ', count);
    screen = *fb;
    pixel_bytes = screen.bpp / 8;
    cols = screen.width / 8;
    rows = screen.height / 16;
    col = row = 0;
    foreground = rgb(220, 228, 240);
    background = rgb(12, 18, 28);
    for (unsigned y = 0; y < screen.height; ++y)
        for (unsigned x = 0; x < screen.width; ++x)
            pixel(x, y, background);
    active = true;
    log_set_fb_sink(console_write);
    vmm_log_fb_enable(true);
    cursor(true);
    flush_writes();
}
void fb_console_release(void)
{
    if (active)
        cursor(false);
    flush_writes();
    active = false;
    vmm_log_fb_enable(false);
}
void fb_console_reclaim(void)
{
    if (screen.hva || text) {
        /* core/log can suspend this sink without calling release; always
         * restore the RAM grid because guest drawing may replace pixels. */
        redraw();
        cursor(true);
        flush_writes();
        active = true;
        vmm_log_fb_enable(true);
        vmm_guest_serial_show_tail();
    }
}

void fb_console_init(const struct boot_framebuffer *fb)
{
    init(fb, false);
}
void fb_console_early(const struct boot_framebuffer *fb)
{
    init(fb, true);
}
void fb_console_clear(void)
{
    if (!cells || !active)
        return;
    memset(cells, ' ', (size_t)cols * rows);
    col = row = 0;
    redraw();
    cursor(true);
    flush_writes();
}
void fb_console_text(volatile uint16_t *buffer, unsigned columns, unsigned lines)
{
    init(NULL, false);
    if (!buffer || !columns || !lines || (size_t)columns * lines > 65536)
        return;
    size_t count = (size_t)columns * lines;
    cell_pages = IA64VMM_ALIGN_UP(count, IA64VMM_PAGE_SIZE) / IA64VMM_PAGE_SIZE;
    cells = hal_alloc_pages(cell_pages, 0);
    if (!cells) {
        cell_pages = 0;
        return;
    }
    memset(cells, ' ', count);
    text = buffer;
    cols = columns;
    rows = lines;
    col = row = 0;
    redraw();
    active = true;
    log_set_fb_sink(console_write);
    vmm_log_fb_enable(true);
    cursor(true);
    flush_writes();
}
