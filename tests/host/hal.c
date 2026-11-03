/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test.h"
#include "ia64vmm/hal.h"

static void expect_permissions(void *p, const char *expected)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    CHECK(maps);
    char line[512], permissions[5];
    unsigned long begin, end;
    bool found = false;
    while (fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%lx-%lx %4s", &begin, &end, permissions) == 3 && (uintptr_t)p >= begin &&
            (uintptr_t)p < end) {
            CHECK(permissions[0] == expected[0]);
            CHECK(permissions[1] == expected[1]);
            CHECK(permissions[2] == expected[2]);
            found = true;
            break;
        }
    }
    fclose(maps);
    CHECK(found);
}

void test_hal(void)
{
    CHECK(!hal_alloc_pages(0, 0) && !hal_alloc_pages(SIZE_MAX, 0));
    unsigned char *p = hal_alloc_pages(1, HAL_PAGE_EXEC | HAL_PAGE_ZERO);
    CHECK(p && (uintptr_t)p % IA64VMM_PAGE_SIZE == 0);
    for (size_t i = 0; i < IA64VMM_PAGE_SIZE; ++i)
        CHECK(p[i] == 0);
    expect_permissions(p, "rw-");
    p[0] = 0xc3;
    hal_code_set_executable(p, 1);
    expect_permissions(p, "r-x");
    CHECK(p[0] == 0xc3);
    hal_code_set_writable(p, 1);
    expect_permissions(p, "rw-");
    p[0] = 1;
    hal_free_pages(p, 1);
    CHECK(hal_counter_hz() == UINT64_C(1000000000));
    uint64_t before = hal_counter();
    hal_idle_until(before);
    CHECK(hal_counter() >= before);
    hal_idle_until(before + UINT64_C(1000000));
    CHECK(hal_counter() >= before);
    CHECK(hal_port_in(0x70, 1) == 0xff);
    CHECK(hal_port_in(0x70, 2) == 0xffff);
    CHECK(hal_port_in(0x70, 4) == UINT32_MAX);
    hal_port_out(0x70, 0, 1);
}
