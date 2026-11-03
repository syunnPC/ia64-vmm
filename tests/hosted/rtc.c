/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "platform/platform.h"
#include "ia64vmm/hal.h"
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            fprintf(stderr, "FAIL RTC:%d: %s\n", __LINE__, #e);                                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint8_t regs[128], index_reg;
uint32_t hal_port_in(uint16_t port, unsigned size)
{
    CHECK(size == 1);
    CHECK(port == 0x70 || port == 0x71);
    return port == 0x70 ? index_reg : regs[index_reg & 127];
}
void hal_port_out(uint16_t port, uint32_t value, unsigned size)
{
    CHECK(port == 0x70 && size == 1 && (value & 0x80));
    index_reg = value;
}
int main(void)
{
    index_reg = 0x80;
    regs[0xb] = 2;
    regs[0] = 0x56;
    regs[2] = 0x34;
    regs[4] = 0x12;
    regs[7] = 0x29;
    regs[8] = 2;
    regs[9] = 0x24;
    CHECK(platform_rtc_seconds() == 1709210096);
    regs[0xb] = 6;
    regs[0] = 56;
    regs[2] = 34;
    regs[4] = 12;
    regs[7] = 29;
    regs[9] = 24;
    CHECK(platform_rtc_seconds() == 1709210096);
    regs[0xb] = 4;
    regs[4] = 0x80 | 12;
    CHECK(platform_rtc_seconds() == 1709210096);
    regs[4] = 1;
    CHECK(platform_rtc_seconds() == 1709170496);
    regs[9] = 23;
    CHECK(!platform_rtc_seconds());
    regs[0xa] = 0x80;
    CHECK(!platform_rtc_seconds());
    puts("test-platform: CMOS BCD/binary, 12/24-hour, leap date and UIP passed");
    return 0;
}
