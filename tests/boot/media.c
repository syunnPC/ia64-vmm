/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "boot/media.h"

static uint8_t memory[1024];
static const uint64_t base = UINT64_C(0x123400000);
static const struct {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
} guid = BOOT_MEDIA_GUID;
static void put(unsigned at, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        memory[at + i] = value >> (i * 8);
}
static const void *map(uint64_t pa, size_t size, void *context)
{
    assert(context == memory);
    return pa >= base && pa - base <= sizeof(memory) && size <= sizeof(memory) - (pa - base)
               ? memory + (pa - base)
               : NULL;
}
static void fixture(void)
{
    memset(memory, 0, sizeof(memory));
    /* UEFI 2.10: x64 EFI_SYSTEM_TABLE and EFI_CONFIGURATION_TABLE. */
    memcpy(memory, "IBI SYST", 8);
    put(12, 120, 4);
    put(104, 1, 8);
    put(112, base + 256, 8);
    memcpy(memory + 256, &guid, sizeof(guid));
    put(272, base + 512, 8);
    struct boot_media record = {BOOT_MEDIA_MAGIC,
                                BOOT_MEDIA_VERSION,
                                sizeof(record),
                                BOOT_MEDIA_PCI,
                                0,
                                0x1300,
                                0x010802,
                                0};
    memcpy(memory + 512, &record, sizeof(record));
}
static bool read_media(struct boot_media *out)
{
    return boot_media_read(out, base, map, memory);
}
int main(void)
{
    struct boot_media out;
    fixture();
    assert(read_media(&out));
    assert(out.status == BOOT_MEDIA_PCI && out.bdf == 0x1300 && out.class_code == 0x010802);
    memset(memory, 0, sizeof(memory));
    assert(out.bdf == 0x1300);
    fixture();
    put(512 + 16, BOOT_MEDIA_NONE, 4);
    assert(read_media(&out));
    assert(out.status == BOOT_MEDIA_NONE);
    fixture();
    memory[0] = 0;
    assert(!read_media(&out));
    fixture();
    put(12, 112, 4);
    assert(!read_media(&out));
    fixture();
    put(104, UINT64_MAX, 8);
    assert(!read_media(&out));
    fixture();
    put(112, base + 1010, 8);
    assert(!read_media(&out));
    fixture();
    put(272, UINT64_MAX - 4, 8);
    assert(!read_media(&out));
    fixture();
    put(272, base + 1010, 8);
    assert(!read_media(&out));
    fixture();
    memory[256] ^= 1;
    assert(!read_media(&out));
    fixture();
    put(104, 2, 8);
    memcpy(memory + 280, memory + 256, 24);
    assert(!read_media(&out));
    fixture();
    memory[512] ^= 1;
    assert(!read_media(&out));
    fixture();
    put(512 + 8, 2, 4);
    assert(!read_media(&out));
    fixture();
    put(512 + 12, 31, 4);
    assert(!read_media(&out));
    fixture();
    put(512 + 16, UINT32_MAX, 4);
    assert(!read_media(&out));
    fixture();
    put(512 + 28, 1, 4);
    assert(!read_media(&out));
    assert(out.status == BOOT_MEDIA_INVALID);
    assert(!boot_media_read(&out, 0, map, memory));
    puts("boot media: UEFI handoff, copy, bounds and invalid records passed");
    return 0;
}
