/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The host identity from SMBIOS (DSP0134 3.x §5.2, §7.2, §7.3): the Type 1
 * UUID when set, else the Type 1 and Type 2 manufacturer, product and
 * serial strings; entry points with bad checksums are ignored. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "persist/persist.h"

static uint8_t table[4096], ep32[0x1f], ep64[0x18];
static unsigned length;
static const uint8_t *map(uint64_t address, uint64_t size)
{
    return address == 0x1000 && size <= sizeof(table) ? table : NULL;
}
static void sum(uint8_t *b, unsigned n, unsigned at)
{
    uint8_t s = 0;
    b[at] = 0;
    for (unsigned i = 0; i < n; ++i)
        s += b[i];
    b[at] = (uint8_t)-s;
}
/* A structure with formatted area f (length n) and NUL-separated strings. */
static void add(const uint8_t *f, unsigned n, const char *strings, unsigned strings_length)
{
    memcpy(table + length, f, n);
    length += n;
    if (!strings_length) {
        table[length++] = 0;
        table[length++] = 0;
        return;
    }
    memcpy(table + length, strings, strings_length);
    length += strings_length;
    table[length++] = 0;
}
static void entry_points(void)
{
    memset(ep64, 0, sizeof(ep64));
    memcpy(ep64, "_SM3_", 5);
    ep64[6] = 0x18;
    ep64[7] = 3;
    ep64[0x0a] = 1;
    ep64[0x0c] = (uint8_t)length;
    ep64[0x0d] = (uint8_t)(length >> 8);
    ep64[0x11] = 0x10;
    sum(ep64, 0x18, 5);
    memset(ep32, 0, sizeof(ep32));
    memcpy(ep32, "_SM_", 4);
    ep32[5] = 0x1f;
    ep32[6] = 2;
    ep32[7] = 8;
    memcpy(ep32 + 0x10, "_DMI_", 5);
    ep32[0x16] = (uint8_t)length;
    ep32[0x17] = (uint8_t)(length >> 8);
    ep32[0x19] = 0x10;
    sum(ep32 + 0x10, 0x0f, 5);
    sum(ep32, 0x1f, 4);
}
static void build(const uint8_t uuid[16], bool type2)
{
    length = 0;
    uint8_t bios[0x12] = {0, 0x12, 0, 0, 1, 2, 0, 3};
    add(bios, sizeof(bios), "Vendor\0V1\0date", 15);
    uint8_t system[0x1b] = {1, 0x1b, 1, 0, 1, 2, 3, 4};
    memcpy(system + 8, uuid, 16);
    add(system, sizeof(system), "Maker\0Model X\0rev\0SN-123", 25);
    if (type2) {
        uint8_t board[0x0f] = {2, 0x0f, 2, 0, 1, 2, 0, 3};
        add(board, sizeof(board), "Board Co\0BX-1\0BSN-9", 20);
    }
    uint8_t end[4] = {127, 4, 3, 0};
    add(end, 4, NULL, 0);
    entry_points();
}

int main(void)
{
    static const uint8_t uuid[16] = {0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56,
                                     0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78};
    uint8_t zero[16] = {0}, ones[16];
    memset(ones, 0xff, 16);
    struct boot_identity id;

    /* A set UUID is the identity, through either entry point. */
    build(uuid, true);
    smbios_identity(NULL, ep64, map, &id);
    assert(id.kind == BOOT_IDENTITY_UUID && id.length == 16 && !memcmp(id.bytes, uuid, 16));
    smbios_identity(ep32, NULL, map, &id);
    assert(id.kind == BOOT_IDENTITY_UUID && !memcmp(id.bytes, uuid, 16));

    /* All 0 (not present) or all FF (not set): the strings, Type 1 then Type 2. */
    static const char expected[] = "Maker\0Model X\0SN-123\0Board Co\0BX-1\0BSN-9";
    for (unsigned k = 0; k < 2; ++k) {
        build(k ? ones : zero, true);
        smbios_identity(ep32, ep64, map, &id);
        assert(id.kind == BOOT_IDENTITY_STRINGS && id.length == sizeof(expected));
        assert(!memcmp(id.bytes, expected, sizeof(expected)));
    }
    /* Without Type 2 its three strings are empty. */
    build(zero, false);
    smbios_identity(ep32, ep64, map, &id);
    static const char t1only[] = "Maker\0Model X\0SN-123\0\0\0";
    assert(id.kind == BOOT_IDENTITY_STRINGS && id.length == sizeof(t1only) &&
           !memcmp(id.bytes, t1only, sizeof(t1only)));

    /* Bad checksums or anchors: no identity. */
    build(uuid, true);
    ep64[0x10] ^= 1;
    ep32[0] ^= 1;
    smbios_identity(ep32, ep64, map, &id);
    assert(id.kind == BOOT_IDENTITY_NONE);
    build(uuid, true);
    ep32[0x18] ^= 1; /* intermediate checksum */
    smbios_identity(ep32, NULL, map, &id);
    assert(id.kind == BOOT_IDENTITY_NONE);
    smbios_identity(NULL, NULL, map, &id);
    assert(id.kind == BOOT_IDENTITY_NONE);

    /* A table that ends inside a structure: no read past it, no identity
     * from the cut structure. */
    build(uuid, true);
    unsigned type1_end = 0x12 + 15 + 1 + 0x1b + 25 + 1;
    ep64[0x0c] = (uint8_t)(type1_end - 3);
    ep64[0x0d] = 0;
    sum(ep64, 0x18, 5);
    smbios_identity(NULL, ep64, map, &id);
    assert(id.kind == BOOT_IDENTITY_NONE);
    puts("SMBIOS identity: passed");
    return 0;
}
