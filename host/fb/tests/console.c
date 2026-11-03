/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "host/fb/fb_console.h"
#include "ia64vmm/hal.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "ia64vmm/string.h"
#include "ia64vmm/log.h"
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            return __LINE__;                                                                       \
    } while (0)
static void (*sink)(const char *, size_t);
static bool enabled;
static bool fail_alloc;
static size_t allocated_pages;
void *hal_alloc_pages(size_t pages, unsigned flags)
{
    (void)flags;
    if (fail_alloc)
        return NULL;
    void *p = aligned_alloc(4096, pages * 4096);
    if (p)
        allocated_pages += pages;
    return p;
}
void hal_free_pages(void *p, size_t pages)
{
    allocated_pages -= pages;
    free(p);
}
void log_set_fb_sink(void (*fn)(const char *, size_t))
{
    sink = fn;
}
void vmm_log_fb_enable(bool enable)
{
    enabled = enable;
}
bool vmm_guest_serial_screen_enabled(void)
{
    return false;
}
void vmm_guest_serial_show_tail(void) {}
static uint8_t buffer[64 + 32 * (32 * 4 + 5) + 64];
static uint8_t reference[32 * (32 * 4 + 5)];
static void output(const char *s)
{
    if (sink && enabled)
        sink(s, strlen(s));
}
static unsigned sample(unsigned x, unsigned y, const struct boot_framebuffer *fb)
{
    const uint8_t *p = fb->hva + y * fb->pitch + x * (fb->bpp / 8);
    unsigned v = 0;
    for (unsigned i = 0; i < fb->bpp / 8; ++i)
        v |= (unsigned)p[i] << (8 * i);
    return v;
}
static int format(unsigned bpp)
{
    struct boot_framebuffer fb = {
        .format = BOOT_FB_RGB_MASKS,
        .hva = buffer + 64,
        .width = 32,
        .height = 32,
        .pitch = 32 * (bpp / 8) + 5,
        .bpp = bpp,
        .red_size = bpp == 16 ? 5 : 8,
        .red_shift = bpp == 16 ? 11 : 16,
        .green_size = bpp == 16 ? 6 : 8,
        .green_shift = bpp == 16 ? 5 : 8,
        .blue_size = bpp == 16 ? 5 : 8,
        .blue_shift = 0,
    };
    fb.size = fb.pitch * fb.height;
    memset(buffer, 0xa5, sizeof(buffer));
    fb_console_init(&fb);
    CHECK(sink && enabled);
    unsigned bg =
        bpp == 16 ? ((12 * 31 / 255) << 11) | ((18 * 63 / 255) << 5) | (28 * 31 / 255) : 0x0c121c;
    unsigned fg = bpp == 16 ? ((220 * 31 / 255) << 11) | ((228 * 63 / 255) << 5) | (240 * 31 / 255)
                            : 0xdce4f0;
    CHECK(sample(20, 20, &fb) == bg);
    CHECK(sample(0, 15, &fb) == fg);
    output("A");
    unsigned ink = 0;
    for (unsigned y = 0; y < 15; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            unsigned v = sample(x, y, &fb);
            CHECK(v == bg || v == fg);
            ink += v == fg;
        }
    CHECK(ink > 10 && ink < 80);
    fb_console_release();
    memcpy(reference, fb.hva, fb.size);
    output("ignored");
    CHECK(!memcmp(reference, fb.hva, fb.size));
    fb_console_reclaim();
    output("B");
    CHECK(memcmp(reference, fb.hva, fb.size));
    fb_console_init(&fb);
    output("B");
    memcpy(reference, fb.hva, fb.pitch * 15);
    fb_console_init(&fb);
    output("A\nB\n");
    CHECK(!memcmp(reference, fb.hva, fb.pitch * 15));
    output("\t\r\b\x01"
           "abcdefghijklmnopqrstuvwxyz\n");
    for (unsigned i = 0; i < 64; ++i)
        CHECK(buffer[i] == 0xa5);
    for (size_t i = 64 + fb.size; i < sizeof(buffer); ++i)
        CHECK(buffer[i] == 0xa5);
    for (unsigned y = 0; y < fb.height; ++y)
        for (unsigned x = fb.width * (bpp / 8); x < fb.pitch; ++x)
            CHECK(fb.hva[y * fb.pitch + x] == 0xa5);
    /* Reclaim must restore from RAM even after the guest overwrites pixels. */
    fb_console_release();
    memcpy(reference, fb.hva, fb.size);
    for (unsigned y = 0; y < fb.height; ++y)
        memset(fb.hva + y * fb.pitch, 0x33, fb.width * (bpp / 8));
    fb_console_reclaim();
    fb_console_release();
    CHECK(!memcmp(reference, fb.hva, fb.size));
    fail_alloc = true;
    fb_console_init(&fb);
    CHECK(!sink && !allocated_pages);
    fb_console_reclaim();
    CHECK(!sink);
    fail_alloc = false;
    fb.pitch = 1;
    fb_console_init(&fb);
    CHECK(!sink);
    fb_console_reclaim();
    CHECK(!sink);
    return 0;
}
/* PROT_NONE and x86 single-step observe the executed framebuffer accesses,
 * including accidental loads/memmove, without instrumenting the console.
 * Its volatile byte stores mean each write fault is exactly one byte. */
static uint8_t *watched;
static size_t watched_size;
static volatile sig_atomic_t fb_reads, fb_writes;
static void access_fault(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    if (sig == SIGSEGV) {
        uintptr_t address = (uintptr_t)info->si_addr;
        if (address < (uintptr_t)watched || address - (uintptr_t)watched >= watched_size)
            _exit(250);
        if (uc->uc_mcontext.gregs[REG_ERR] & 2)
            ++fb_writes;
        else
            ++fb_reads;
        if (mprotect(watched, watched_size, PROT_READ | PROT_WRITE))
            _exit(251);
        uc->uc_mcontext.gregs[REG_EFL] |= 0x100;
    } else {
        if (mprotect(watched, watched_size, PROT_NONE))
            _exit(252);
        uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    }
}
static int performance(unsigned bpp)
{
    struct boot_framebuffer fb = {
        .format = BOOT_FB_RGB_MASKS,
        .width = 35,
        .height = 133,
        .pitch = 35 * (bpp / 8) + 5,
        .bpp = bpp,
        .red_size = bpp == 16 ? 5 : 8,
        .red_shift = bpp == 16 ? 11 : 16,
        .green_size = bpp == 16 ? 6 : 8,
        .green_shift = bpp == 16 ? 5 : 8,
        .blue_size = bpp == 16 ? 5 : 8,
    };
    fb.size = (size_t)fb.pitch * fb.height;
    watched_size = (fb.size + 8191) & ~(size_t)4095;
    watched = mmap(NULL, watched_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(watched != MAP_FAILED);
    fb.hva = watched + 64;
    memset(watched, 0xa5, watched_size);
    struct sigaction action = {.sa_sigaction = access_fault, .sa_flags = SA_SIGINFO};
    struct sigaction old_segv, old_trap;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGSEGV, &action, &old_segv));
    CHECK(!sigaction(SIGTRAP, &action, &old_trap));
    fb_reads = fb_writes = 0;
    CHECK(!mprotect(watched, watched_size, PROT_NONE));
    volatile uint8_t *probe = watched;
    unsigned char value = probe[0];
    probe[1] = value;
    CHECK(fb_reads == 1 && fb_writes == 1);
    CHECK(!mprotect(watched, watched_size, PROT_READ | PROT_WRITE));
    memset(watched, 0xa5, watched_size);
    CHECK(!mprotect(watched, watched_size, PROT_NONE));
    fb_reads = fb_writes = 0;
    fb_console_init(&fb);
    CHECK(sink && enabled);
    CHECK(!fb_reads);
    CHECK((size_t)fb_writes == ((size_t)fb.width * fb.height + 8) * (bpp / 8));
    size_t bytes = 0, redraws = 0;
    for (unsigned line = 1; line <= 24; ++line) {
        fb_reads = fb_writes = 0;
        output("A\n");
        size_t expected = (128 + 16) * (bpp / 8);
        /* Eight text rows, scrolling two at a time: 8, 10, 12, ... */
        if (line >= 8 && !(line & 1)) {
            expected += (size_t)32 * 128 * (bpp / 8);
            ++redraws;
        }
        CHECK(!fb_reads && (size_t)fb_writes == expected);
        bytes += fb_writes;
    }
    CHECK(redraws == 9);
    fb_reads = fb_writes = 0;
    fb_console_release();
    CHECK(!fb_reads && fb_writes == 8 * (int)(bpp / 8));
    fb_reads = fb_writes = 0;
    output("ignored\n");
    CHECK(!fb_reads && !fb_writes);
    CHECK(!mprotect(watched, watched_size, PROT_READ | PROT_WRITE));
    /* Overwrite text as a guest would, keeping padding and partial margins. */
    for (unsigned y = 0; y < 128; ++y)
        memset(fb.hva + y * fb.pitch, 0x33, 32 * (bpp / 8));
    CHECK(!mprotect(watched, watched_size, PROT_NONE));
    fb_reads = fb_writes = 0;
    fb_console_reclaim();
    CHECK(!fb_reads && (size_t)fb_writes == (32 * 128 + 8) * (bpp / 8));
    fb_console_init(NULL);
    CHECK(!allocated_pages);
    CHECK(!mprotect(watched, watched_size, PROT_READ | PROT_WRITE));
    for (size_t i = 0; i < 64; ++i)
        CHECK(watched[i] == 0xa5);
    for (size_t i = 64 + fb.size; i < watched_size; ++i)
        CHECK(watched[i] == 0xa5);
    for (unsigned y = 0; y < fb.height; ++y)
        for (unsigned x = fb.width * (bpp / 8); x < fb.pitch; ++x)
            CHECK(fb.hva[y * fb.pitch + x] == 0xa5);
    CHECK(!sigaction(SIGSEGV, &old_segv, NULL));
    CHECK(!sigaction(SIGTRAP, &old_trap, NULL));
    CHECK(!munmap(watched, watched_size));
    printf("fb %u-bpp: 0 framebuffer reads, %zu bytes/24 lines (%zu bytes/line), %zu redraws\n",
           bpp, bytes, bytes / 24, redraws);
    return 0;
}
/* VGA text backend: cells are
 * character | attribute << 8 (07h text, 70h cursor), scrolling and
 * reclaim come from the RAM grid, and the buffer is never read. */
static bool text_row(const volatile uint16_t *t, unsigned row, const char *s, unsigned cursor_col)
{
    for (unsigned x = 0; x < 80; ++x) {
        unsigned ch = x < strlen(s) ? (unsigned char)s[x] : ' ';
        if (t[row * 80 + x] != (uint16_t)(ch | (x == cursor_col ? 0x70u : 0x07u) << 8))
            return false;
    }
    return true;
}
static int text_mode(void)
{
    watched_size = 8192;
    watched = mmap(NULL, watched_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(watched != MAP_FAILED);
    volatile uint16_t *t = (volatile uint16_t *)watched;
    memset(watched, 0xa5, watched_size);
    struct sigaction action = {.sa_sigaction = access_fault, .sa_flags = SA_SIGINFO}, old_segv,
                     old_trap;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGSEGV, &action, &old_segv) && !sigaction(SIGTRAP, &action, &old_trap));
    CHECK(!mprotect(watched, watched_size, PROT_NONE));
    fb_reads = fb_writes = 0;
    fb_console_text(t, 80, 25);
    output("VMM\tA\x01\n");
    CHECK(sink && enabled && !fb_reads);
    CHECK(!mprotect(watched, watched_size, PROT_READ | PROT_WRITE));
    CHECK(text_row(t, 0, "VMM     A?", 80) && text_row(t, 1, "", 0) && text_row(t, 24, "", 80));
    for (size_t i = 80 * 25 * 2; i < watched_size; ++i)
        CHECK(watched[i] == 0xa5);
    /* Broken variant of the row check: a wrong attribute is caught. */
    t[0] = 'V' | 0x1f << 8;
    CHECK(!text_row(t, 0, "VMM     A?", 80));
    /* 25 rows scroll by a quarter (6 rows) from RAM, without reads. */
    char line[8];
    CHECK(!mprotect(watched, watched_size, PROT_NONE));
    fb_reads = 0;
    for (unsigned i = 0; i < 30; ++i) {
        snprintf(line, sizeof(line), "L%u\n", i);
        output(line);
    }
    CHECK(!fb_reads);
    CHECK(!mprotect(watched, watched_size, PROT_READ | PROT_WRITE));
    /* 31 lines on 25 rows: two batched scrolls leave L11 at the top, L29
     * on row 18 and the cursor on row 19. */
    CHECK(text_row(t, 18, "L29", 80) && text_row(t, 19, "", 0) && text_row(t, 0, "L11", 80) &&
          text_row(t, 24, "", 80));
    /* Reclaim redraws the grid after the guest wrote the buffer. */
    fb_console_release();
    for (unsigned i = 0; i < 80 * 25; ++i)
        t[i] = 0x4e41;
    fb_console_reclaim();
    CHECK(text_row(t, 18, "L29", 80) && text_row(t, 19, "", 0) && text_row(t, 0, "L11", 80) &&
          enabled);
    fb_console_clear();
    CHECK(text_row(t, 0, "", 0) && text_row(t, 23, "", 80));
    fb_console_init(NULL);
    CHECK(!sink && !allocated_pages);
    CHECK(!sigaction(SIGSEGV, &old_segv, NULL) && !sigaction(SIGTRAP, &old_trap, NULL));
    CHECK(!munmap(watched, watched_size));
    printf("fb text: 80x25 cells and attributes, cursor, 0 buffer reads, scroll, reclaim, clear\n");
    return 0;
}
int main(void)
{
    int result;
    if ((result = format(16)) || (result = format(24)) || (result = format(32)) ||
        (result = performance(16)) || (result = performance(24)) || (result = performance(32)) ||
        (result = text_mode())) {
        fprintf(stderr, "framebuffer test failed at line %d\n", result);
        return result;
    }
    return 0;
}
