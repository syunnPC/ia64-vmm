/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test.h"
#include <string.h>
#include "core/dump-va.h"

void test_dump_va(void)
{
    const struct {
        const char *text;
        uint64_t address, bytes;
    } valid[] = {
        {"e000000088603280:48", UINT64_C(0xe000000088603280), 48},
        {"b000:8", 0xb000, 8},
        {"b000000000000000:8", UINT64_C(0x0b000000000000000), 8},
        {"40:65536", 0x40, 65536},
        {"ffffffffffffffef:16", UINT64_MAX - 16, 16},
        {"0:0", 0, 0},
    };
    for (unsigned n = 0; n < sizeof(valid) / sizeof(valid[0]); ++n) {
        size_t cursor = 0;
        uint64_t address, bytes;
        const char *error = NULL;
        CHECK(vmm_dump_va_range(valid[n].text, strlen(valid[n].text), &cursor, &address, &bytes,
                                &error));
        CHECK(address == valid[n].address && bytes == valid[n].bytes &&
              cursor == strlen(valid[n].text));
    }
    const char *invalid[] = {"",
                             ":8",
                             "g:8",
                             "0",
                             "G40:8",
                             "10000000000000000:8",
                             "0:",
                             "0:-1",
                             "0:65537",
                             "0:8x",
                             "fffffffffffffff0:32",
                             "bfffffffffffffffff:8",
                             "bsp::8",
                             "bsp:40",
                             "bsp:40:65537"};
    for (unsigned n = 0; n < sizeof(invalid) / sizeof(invalid[0]); ++n) {
        size_t cursor = 0;
        uint64_t address, bytes;
        const char *error = NULL;
        CHECK(
            !vmm_dump_va_range(invalid[n], strlen(invalid[n]), &cursor, &address, &bytes, &error) &&
            error != NULL);
    }
    const char *multiple = "1040:8,2080:16,3040:24";
    size_t cursor = 0;
    uint64_t address, bytes;
    const char *error = NULL;
    for (unsigned n = 0; n < 3; ++n) {
        CHECK(vmm_dump_va_range(multiple, strlen(multiple), &cursor, &address, &bytes, &error));
        CHECK(address == (n + 1) * 0x1000 + (n == 1 ? 0x80 : 0x40) && bytes == (n + 1) * 8);
    }
    CHECK(cursor == strlen(multiple));
    const char *mixed = "g40:8,xyz:8,1000:8,bsp:40:8,2000:16";
    cursor = 0;
    CHECK(!vmm_dump_va_range(mixed, strlen(mixed), &cursor, &address, &bytes, &error));
    CHECK(!vmm_dump_va_range(mixed, strlen(mixed), &cursor, &address, &bytes, &error));
    CHECK(vmm_dump_va_range(mixed, strlen(mixed), &cursor, &address, &bytes, &error) &&
          address == 0x1000);
    CHECK(!vmm_dump_va_range(mixed, strlen(mixed), &cursor, &address, &bytes, &error));
    CHECK(vmm_dump_va_range(mixed, strlen(mixed), &cursor, &address, &bytes, &error) &&
          address == 0x2000);
    CHECK(cursor == strlen(mixed));
    cursor = 0;
    CHECK(!vmm_dump_va_range("g40:8", 5, &cursor, &address, &bytes, &error));
    cursor = 0;
    CHECK(vmm_dump_va_range("b40:8", 5, &cursor, &address, &bytes, &error) && address == 0xb40);
}
