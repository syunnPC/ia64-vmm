/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The host identity from SMBIOS (DMTF DSP0134 3.x): the 64-bit entry point
 * (_SM3_, §5.2.2) or the 32-bit one (_SM_, §5.2.1), then the Type 1 System
 * Information (§7.2) and Type 2 Baseboard Information (§7.3) structures. */
#include "persist/persist.h"
#include "ia64vmm/string.h"

static uint64_t le(const uint8_t *b, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)b[i] << (i * 8);
    return v;
}
static bool checksum(const uint8_t *b, unsigned n)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < n; ++i)
        sum += b[i];
    return !sum;
}
/* String number index (1-based) of the structure at s, which ends before
 * end; "" for 0 or a missing string. */
static const char *string_at(const uint8_t *s, const uint8_t *end, unsigned index, unsigned *length)
{
    const uint8_t *at = s + s[1];
    *length = 0;
    for (unsigned n = 1; index && at < end && *at; ++n) {
        const uint8_t *start = at;
        while (at < end && *at)
            ++at;
        if (at == end)
            return "";
        if (n == index) {
            *length = (unsigned)(at - start);
            return (const char *)start;
        }
        ++at;
    }
    return "";
}
/* The end of the structure at s (after its string set's double NUL), or
 * NULL when the table ends first. */
static const uint8_t *next_structure(const uint8_t *s, const uint8_t *end)
{
    if (end - s < 4 || s[1] < 4 || end - s < s[1])
        return NULL;
    const uint8_t *at = s + s[1];
    while (end - at >= 2 && (at[0] || at[1]))
        ++at;
    return end - at >= 2 ? at + 2 : NULL;
}

void smbios_identity(const uint8_t *entry32, const uint8_t *entry64,
                     const uint8_t *(*table)(uint64_t address, uint64_t length),
                     struct boot_identity *out)
{
    memset(out, 0, sizeof(*out));
    uint64_t address = 0, length = 0;
    if (entry64 && !memcmp(entry64, "_SM3_", 5) && entry64[6] >= 0x18 &&
        checksum(entry64, entry64[6])) {
        address = le(entry64 + 0x10, 8);
        length = le(entry64 + 0x0c, 4);
    } else if (entry32 && !memcmp(entry32, "_SM_", 4) && entry32[5] >= 0x1f &&
               checksum(entry32, entry32[5]) && !memcmp(entry32 + 0x10, "_DMI_", 5) &&
               checksum(entry32 + 0x10, 0x0f)) {
        address = le(entry32 + 0x18, 4);
        length = le(entry32 + 0x16, 2);
    }
    const uint8_t *base = address && length ? table(address, length) : NULL;
    if (!base)
        return;
    const uint8_t *end = base + length, *type1 = NULL, *type2 = NULL;
    for (const uint8_t *s = base, *next; s && (next = next_structure(s, end)); s = next) {
        if (s[0] == 1 && !type1)
            type1 = s;
        if (s[0] == 2 && !type2)
            type2 = s;
        if (s[0] == 127)
            break;
    }
    /* §7.2.1: all zero bytes, the ID is not present; all 0xFF, it is not
     * set. Both mean no UUID. */
    if (type1 && type1[1] >= 0x18) {
        bool zero = true, ones = true;
        for (unsigned i = 0; i < 16; ++i) {
            zero = zero && !type1[8 + i];
            ones = ones && type1[8 + i] == 0xff;
        }
        if (!zero && !ones) {
            out->kind = BOOT_IDENTITY_UUID;
            out->length = 16;
            memcpy(out->bytes, type1 + 8, 16);
            return;
        }
    }
    /* Manufacturer, product and serial number (string offsets 4, 5, 7 in
     * both structures). */
    static const unsigned fields[] = {4, 5, 7};
    unsigned at = 0;
    bool any = false;
    for (unsigned k = 0; k < 2; ++k) {
        const uint8_t *s = k ? type2 : type1;
        for (unsigned f = 0; f < 3; ++f) {
            unsigned n = 0;
            const char *text = s && s[1] > fields[f] ? string_at(s, end, s[fields[f]], &n) : "";
            if (n > 63)
                n = 63;
            memcpy(out->bytes + at, text, n);
            at += n + 1;
            any = any || n;
        }
    }
    if (any) {
        out->kind = BOOT_IDENTITY_STRINGS;
        out->length = at;
    }
}
