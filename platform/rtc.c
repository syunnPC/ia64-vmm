/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/platform.h"
#include "ia64vmm/hal.h"

__attribute__((weak)) uint64_t hal_wallclock_seconds(void)
{
    return 0;
}

static uint8_t cmos_read(uint8_t reg)
{
    /* Preserve the host NMI mask while selecting an MC146818 register. */
    uint8_t mask = hal_port_in(0x70, 1) & 0x80;
    hal_port_out(0x70, mask | reg, 1);
    return hal_port_in(0x71, 1);
}

static unsigned decode(uint8_t value, bool binary)
{
    if (binary)
        return value;
    if ((value & 15) > 9 || (value >> 4) > 9)
        return 255;
    return (value >> 4) * 10 + (value & 15);
}

static bool leap(unsigned year)
{
    return !(year % 4) && ((year % 100) || !(year % 400));
}

uint64_t platform_rtc_seconds(void)
{
    uint64_t wall = hal_wallclock_seconds();
    if (wall)
        return wall;
    static const uint8_t registers[] = {0, 2, 4, 7, 8, 9, 0xb};
    uint8_t first[7], second[7];
    bool stable = false;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (cmos_read(0xa) & 0x80)
            continue;
        for (unsigned i = 0; i < 7; ++i)
            first[i] = cmos_read(registers[i]);
        if (cmos_read(0xa) & 0x80)
            continue;
        for (unsigned i = 0; i < 7; ++i)
            second[i] = cmos_read(registers[i]);
        if (cmos_read(0xa) & 0x80)
            continue;
        stable = true;
        for (unsigned i = 0; i < 7; ++i)
            if (first[i] != second[i])
                stable = false;
        if (stable)
            break;
    }
    if (!stable)
        return 0;
    bool binary = (first[6] & 4) != 0;
    unsigned sec = decode(first[0], binary), min = decode(first[1], binary);
    unsigned hour = decode(first[2] & 0x7f, binary);
    if (!(first[6] & 2)) {
        if (!hour || hour > 12)
            return 0;
        hour = hour % 12 + ((first[2] & 0x80) ? 12 : 0);
    }
    unsigned day = decode(first[3], binary), month = decode(first[4], binary);
    unsigned year = decode(first[5], binary);
    if (year > 99 || sec > 59 || min > 59 || hour > 23 || month < 1 || month > 12)
        return 0;
    /* The CMOS century rule is 1970..2069; no ACPI century register yet. */
    year += year < 70 ? 2000 : 1900;
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned mdays = days[month - 1] + (month == 2 && leap(year));
    if (!day || day > mdays)
        return 0;
    uint64_t total = 0;
    for (unsigned y = 1970; y < year; ++y)
        total += 365 + leap(y);
    for (unsigned m = 1; m < month; ++m)
        total += days[m - 1] + (m == 2 && leap(year));
    return ((total + day - 1) * 24 + hour) * 3600 + min * 60 + sec;
}
