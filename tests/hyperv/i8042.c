/* SPDX-License-Identifier: GPL-2.0-or-later */
/* IBM PS/2 controller/keyboard protocol and native EDK2 InitKeyboard sequence. */
#include <assert.h>
#include <stdio.h>
#include "platform/i8042.h"

static bool irq_high;
static unsigned rises, falls;
static void irq(void *context, bool high)
{
    assert(context == &irq_high && high != irq_high);
    irq_high = high;
    if (high)
        ++rises;
    else
        ++falls;
}
static void command(unsigned value)
{
    platform_i8042_write(0x64, value);
}
static void data(unsigned value)
{
    platform_i8042_write(0x60, value);
}
static unsigned status(void)
{
    return platform_i8042_read(0x64);
}
static unsigned read_data(void)
{
    assert(status() & 1);
    return platform_i8042_read(0x60);
}
static void expect(const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        assert(read_data() == p[i]);
    assert(!(status() & 1) && !irq_high);
}
static void config(unsigned value)
{
    command(0x60);
    data(value);
}
static void ack(unsigned value)
{
    data(value);
    assert(read_data() == 0xfa);
}
static void scancode_set(unsigned value)
{
    ack(0xf0);
    ack(value);
}
static void native_init(void)
{
    assert(!(status() & (1 | 2 | 4)));
    command(0xad);
    command(0xa7);
    command(0xaa);
    assert(read_data() == 0x55);
    assert(status() & 4);
    config(0x67);
    command(0x20);
    assert(read_data() == 0x67);
    ack(0xf4);
    command(0xab);
    assert(read_data() == 0);
    data(0xff);
    expect((const uint8_t[]){0xfa, 0xaa}, 2);
    scancode_set(2);
    ack(0xf4);
    ack(0xed);
    ack(0);
    assert(!(status() & (1 | 2 | 0xe0)));
}
int main(void)
{
    assert(!platform_i8042_configured() && platform_i8042_read(0x64) == 0xff);
    assert(!platform_i8042_init(irq, &irq_high));
    platform_i8042_configure(true);
    assert(platform_i8042_configured());
    assert(platform_i8042_init(irq, &irq_high));
    native_init();
    assert(platform_i8042_input(0x48, false, true, false));
    assert(platform_i8042_input(0x48, true, true, false));
    expect((const uint8_t[]){0xe0, 0x48, 0xe0, 0xc8}, 4);
    assert(platform_i8042_input(0x1c, false, false, false));
    assert(platform_i8042_input(0x1c, true, false, false));
    expect((const uint8_t[]){0x1c, 0x9c}, 2);
    assert(platform_i8042_input(0x2a, false, false, false));
    assert(platform_i8042_input(0x1e, false, false, false));
    assert(platform_i8042_input(0x1e, true, false, false));
    assert(platform_i8042_input(0x2a, true, false, false));
    expect((const uint8_t[]){0x2a, 0x1e, 0x9e, 0xaa}, 4);
    /* Native set 2, followed by direct set 1 without controller translation. */
    config(0x27);
    scancode_set(2);
    assert(platform_i8042_input(0x48, false, true, false));
    assert(platform_i8042_input(0x48, true, true, false));
    assert(platform_i8042_input(0x1c, false, false, false));
    assert(platform_i8042_input(0x1c, true, false, false));
    expect((const uint8_t[]){0xe0, 0x75, 0xe0, 0xf0, 0x75, 0x5a, 0xf0, 0x5a}, 8);
    ack(0xf0);
    ack(0);
    expect((const uint8_t[]){2}, 1);
    scancode_set(1);
    assert(platform_i8042_input(0x1d, false, true, false));
    assert(platform_i8042_input(0x1d, true, true, false));
    expect((const uint8_t[]){0xe0, 0x1d, 0xe0, 0x9d}, 4);
    scancode_set(2);
    config(0x67);
    ack(0xf0);
    ack(0);
    expect((const uint8_t[]){0x41}, 1);
    data(0xf2);
    expect((const uint8_t[]){0xfa, 0xab, 0x41}, 3);
    config(0x27);
    data(0xf2);
    expect((const uint8_t[]){0xfa, 0xab, 0x83}, 3);
    data(0xee);
    expect((const uint8_t[]){0xee}, 1);
    data(0xfe);
    expect((const uint8_t[]){0xee}, 1);
    /* LED transactions delay key events without losing modifier releases. */
    config(0x67);
    ack(0xed);
    assert(platform_i8042_input(0x2a, false, false, false));
    assert(platform_i8042_input(0x1e, false, false, false));
    assert(platform_i8042_input(0x1e, true, false, false));
    assert(platform_i8042_input(0x2a, true, false, false));
    assert(!(status() & 1));
    ack(7);
    expect((const uint8_t[]){0x2a, 0x1e, 0x9e, 0xaa}, 4);
    config(0x27);
    ack(0xf3);
    ack(0x2b);
    ack(0xed);
    ack(7);
    /* Unsupported scan set and malformed parameters request retransmission. */
    ack(0xf0);
    data(3);
    expect((const uint8_t[]){0xfe}, 1);
    ack(2);
    data(0);
    expect((const uint8_t[]){0xfe}, 1);
    ack(0xed);
    data(0x80);
    expect((const uint8_t[]){0xfe}, 1);
    ack(0);
    /* A command aborts a keyboard parameter transaction. */
    ack(0xf3);
    data(0xee);
    expect((const uint8_t[]){0xee}, 1);
    ack(0xf5);
    assert(!platform_i8042_input(0x1c, false, false, false));
    ack(0xf4);
    command(0xad);
    assert(!platform_i8042_input(0x1c, false, false, false));
    command(0xae);
    /* Input is delivered while IRQ1 is masked, and asserts when unmasked. */
    config(0x26);
    unsigned before = rises;
    assert(platform_i8042_input(0x1c, false, false, false));
    assert(!irq_high);
    config(0x27);
    assert(irq_high && rises == before + 1);
    expect((const uint8_t[]){0x5a}, 1);
    command(0xa8);
    command(0x20);
    assert(read_data() & 0x20);
    command(0xa9);
    expect((const uint8_t[]){1}, 1);
    command(0xd4);
    data(0xff);
    assert(!(status() & 1));
    command(0xd2);
    data(0x1e);
    expect((const uint8_t[]){0x1e}, 1);
    command(0xd0);
    assert(read_data() & 1);
    /* Pause is one complete sequence; Print Screen includes artificial shifts. */
    assert(platform_i8042_input(0x45, false, false, true));
    expect((const uint8_t[]){0xe1, 0x14, 0x77, 0xe1, 0xf0, 0x14, 0xf0, 0x77}, 8);
    assert(platform_i8042_input(0x45, true, false, true));
    assert(!(status() & 1));
    assert(platform_i8042_input(0x37, false, true, false));
    assert(platform_i8042_input(0x37, true, true, false));
    expect((const uint8_t[]){0xe0, 0x12, 0xe0, 0x7c, 0xe0, 0xf0, 0x7c, 0xe0, 0xf0, 0x12}, 10);
    config(0x67);
    assert(platform_i8042_input(0x45, false, false, true));
    expect((const uint8_t[]){0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5}, 6);
    assert(!platform_i8042_input(0x100, false, false, false));
    assert(!platform_i8042_input(0x48, false, true, true));
    /* Overflow drops complete key events; replies retain reserved capacity. */
    unsigned accepted = 0;
    while (platform_i8042_input(0x48, true, true, false)) {
        ++accepted;
        assert(accepted < 256);
    }
    data(0xf4);
    for (unsigned i = 0; i < accepted; ++i) {
        assert(read_data() == 0xe0);
        assert(read_data() == 0xc8);
    }
    expect((const uint8_t[]){0xfa}, 1);
    platform_i8042_reset();
    assert(!irq_high && !(status() & 1));
    native_init();
    assert(platform_i8042_input(0x1c, false, false, false));
    assert(irq_high && rises && falls);
    platform_i8042_cleanup();
    assert(!irq_high && platform_i8042_configured());
    assert(platform_i8042_read(0x60) == 0xff);
    platform_i8042_configure(false);
    assert(!platform_i8042_configured());
    puts("guest i8042: passed");
}
