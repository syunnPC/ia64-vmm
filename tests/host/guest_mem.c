/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/engine.h"

static unsigned notifications;
static gpa_t notified_gpa;
static uint64_t notified_len;
static unsigned char *before_write;
static unsigned char expected_old;

void code_notify_vmm_write(gpa_t gpa, uint64_t len)
{
    ++notifications;
    notified_gpa = gpa;
    notified_len = len;
    if (before_write)
        CHECK(*before_write == expected_old);
}

void test_guest_mem(void)
{
    static unsigned char low[32], middle[32], high[32];
    static const struct gm_mmio_ops ops = {0};
    CHECK(!gm_lookup(0) && gm_region_count() == 0);
    struct gm_region r = {
        .base = 0x2000, .size = 32, .kind = GM_FIRMWARE, .name = "high", .hva = high};
    CHECK(gm_add_region(&r));
    r = (struct gm_region){.base = 0, .size = 32, .kind = GM_RAM, .name = "low", .hva = low};
    CHECK(gm_add_region(&r));
    r = (struct gm_region){
        .base = 32, .size = 32, .kind = GM_DIRECT_MMIO, .name = "middle", .hva = middle};
    CHECK(gm_add_region(&r));
    CHECK(gm_region_count() == 3);
    CHECK(gm_region_at(0)->base == 0 && gm_region_at(1)->base == 32);
    CHECK(gm_region_at(2)->base == 0x2000 && !gm_region_at(3));
    CHECK(gm_lookup(0) == gm_region_at(0));
    CHECK(gm_lookup(31) == gm_region_at(0));
    CHECK(gm_lookup(32) == gm_region_at(1));
    CHECK(gm_lookup(63) == gm_region_at(1));
    CHECK(!gm_lookup(64) && !gm_lookup(0x1fff));
    CHECK(gm_lookup(0x2000) == gm_region_at(2));
    CHECK(gm_lookup(0x201f) == gm_region_at(2));
    CHECK(!gm_lookup(0x2020) && !gm_lookup(UINT64_MAX));
    r.base = 16;
    CHECK(!gm_add_region(&r));
    r.base = 0;
    CHECK(!gm_add_region(&r));
    r.base = 0x1ff0;
    CHECK(!gm_add_region(&r));
    r.base = 0x1ff0;
    r.size = 128;
    CHECK(!gm_add_region(&r));
    r.base = UINT64_MAX - 4;
    r.size = 8;
    CHECK(!gm_add_region(&r));
    r.size = 0;
    CHECK(!gm_add_region(&r));
    CHECK(!gm_add_region(NULL));
    CHECK(gm_region_count() == 3);
    CHECK(gm_direct_ptr(0, 32) == low);
    CHECK(gm_direct_ptr(31, 1) == low + 31);
    CHECK(gm_direct_ptr(31, 0) == low + 31);
    CHECK(!gm_direct_ptr(31, 2));
    CHECK(!gm_direct_ptr(0, UINT64_MAX));
    CHECK(gm_direct_ptr(32, 32) == middle);
    CHECK(gm_direct_ptr(0x2000, 32) == high);
    CHECK(!gm_direct_ptr(64, 0));
    const char data[] = "write";
    before_write = low + 4;
    expected_old = 0;
    CHECK(gm_write(4, data, sizeof(data)));
    CHECK(notifications == 1 && notified_gpa == 4 && notified_len == sizeof(data));
    CHECK(!memcmp(low + 4, data, sizeof(data)));
    before_write = NULL;
    char out[sizeof(data)] = {0};
    CHECK(gm_read(4, out, sizeof(out)) && !memcmp(out, data, sizeof(out)));
    CHECK(!gm_write(31, data, sizeof(data)) && notifications == 1);
    CHECK(!gm_read(31, out, sizeof(out)));
    CHECK(!gm_write(0, NULL, 1) && !gm_read(0, NULL, 1));
    CHECK(gm_write(0, NULL, 0) && gm_read(0, NULL, 0));
    r = (struct gm_region){
        .base = 0x3000, .size = 4096, .kind = GM_EMULATED, .name = "emulated", .ops = &ops};
    CHECK(gm_add_region(&r));
    CHECK(gm_lookup(0x3000)->kind == GM_EMULATED);
    CHECK(!gm_direct_ptr(0x3000, 1));
    CHECK(!gm_write(0x3000, data, 1) && !gm_read(0x3000, out, 1));
    while (gm_region_count() < GM_MAX_REGIONS) {
        r.base += 4096;
        CHECK(gm_add_region(&r));
    }
    r.base += 4096;
    CHECK(!gm_add_region(&r));
    CHECK(gm_region_count() == GM_MAX_REGIONS);
}
