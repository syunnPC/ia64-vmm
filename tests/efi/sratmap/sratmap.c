/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ACPI §5.2.16.2 Memory Affinity Structure and UEFI §7.2.3 memory types:
 * compare both unions, so neither a hole nor omitted RAM can be hidden. */
#include <efi.h>
#include "../../../include/ia64vmm/platform.h"
static unsigned char map[65536], srat[32768];
static void say(const char *s)
{
    while (*s)
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE) = (unsigned char)*s++;
}
static UINT64 le(const unsigned char *p, unsigned n)
{
    UINT64 v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (UINT64)p[i] << (i * 8);
    return v;
}
static int same(const void *a, const void *b, unsigned n)
{
    const unsigned char *p = a, *q = b;
    while (n--)
        if (*p++ != *q++)
            return 0;
    return 1;
}
static int valid(const unsigned char *p, UINTN capacity)
{
    if (!p || le(p + 4, 4) < 36 || le(p + 4, 4) > capacity)
        return 0;
    unsigned sum = 0;
    for (UINTN i = 0; i < le(p + 4, 4); ++i)
        sum += p[i];
    return !(sum & 255);
}
static int ram(UINT32 type)
{
    /* UEFI §7.2.3, Memory Type Usage after ExitBootServices(): available,
     * reclaimable or preserved RAM. The four no-use/device categories are excluded;
     * undefined types are not a claim of system memory. */
    return type > EfiReservedMemoryType && type <= EfiPalCode && type != EfiUnusableMemory &&
           type != EfiMemoryMappedIO && type != EfiMemoryMappedIOPortSpace;
}
static int map_covers(UINT64 base, UINT64 end, UINTN size, UINTN stride)
{
    while (base < end) {
        UINT64 next = base;
        for (UINTN at = 0; at + stride <= size; at += stride) {
            const EFI_MEMORY_DESCRIPTOR *d = (const void *)(map + at);
            if (ram(d->Type) && d->PhysicalStart <= base &&
                base - d->PhysicalStart < d->NumberOfPages * 4096)
                next = d->PhysicalStart + d->NumberOfPages * 4096;
        }
        if (next <= base)
            return 0;
        base = next;
    }
    return 1;
}
static int srat_covers(UINT64 base, UINT64 end, UINTN length)
{
    while (base < end) {
        UINT64 next = base;
        for (UINTN at = 48; at < length; at += srat[at + 1]) {
            const unsigned char *m = srat + at;
            if (m[0] != 1 || !(le(m + 28, 4) & 1))
                continue;
            UINT64 start = le(m + 8, 8), size = le(m + 16, 8);
            if (start <= base && base - start < size)
                next = start + size;
        }
        if (next <= base)
            return 0;
        base = next;
    }
    return 1;
}
EFI_STATUS _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    static const EFI_GUID guid = {
        0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
    const unsigned char *root = 0, *source = 0;
    say("SRATMAP BEGIN\n");
    for (UINTN i = 0; i < st->NumberOfTableEntries; ++i)
        if (same(&guid, &st->ConfigurationTable[i].VendorGuid, 16)) {
            const unsigned char *rsdp = st->ConfigurationTable[i].VendorTable;
            root = (void *)(UINTN)le(rsdp + 24, 8);
        }
    if (!valid(root, 65536))
        goto failed;
    for (UINTN at = 36; at + 8 <= le(root + 4, 4); at += 8) {
        const unsigned char *p = (void *)(UINTN)le(root + at, 8);
        if (same(p, "SRAT", 4))
            source = p;
    }
    if (!valid(source, sizeof(srat)) || le(source + 4, 4) < 48)
        goto failed;
    UINTN length = le(source + 4, 4);
    for (UINTN i = 0; i < length; ++i)
        srat[i] = source[i];
    UINTN size = sizeof(map), key, stride;
    UINT32 version;
    if (st->BootServices->GetMemoryMap(&size, (void *)map, &key, &stride, &version) !=
            EFI_SUCCESS ||
        version != EFI_MEMORY_DESCRIPTOR_VERSION || stride < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        size % stride)
        goto failed;
    unsigned affinities = 0;
    for (UINTN at = 48; at < length; at += srat[at + 1]) {
        unsigned char *m = srat + at;
        if (at + 2 > length || m[1] < 2 || m[1] > length - at)
            goto failed;
        if (m[0] != 1)
            continue;
        if (m[1] != 40 || le(m + 28, 4) != 1)
            goto failed;
#ifdef SRATMAP_BROKEN_HOLE
        if (!affinities) {
            /* A checksum-correct affinity over VGA device space must fail
             * the ordinary RAM coverage check, rather than table syntax. */
            for (unsigned i = 0; i < 8; ++i) {
                m[8 + i] = (unsigned char)(0xa0000ULL >> (8 * i));
                m[16 + i] = (unsigned char)(0x2000ULL >> (8 * i));
            }
            unsigned sum = 0;
            srat[9] = 0;
            for (UINTN i = 0; i < length; ++i)
                sum += srat[i];
            srat[9] = (unsigned char)-sum;
            say("SRATMAP inserted affinity over VGA hole\n");
        }
#endif
        UINT64 base = le(m + 8, 8), bytes = le(m + 16, 8);
        if (!bytes || bytes > ~(UINT64)0 - base || !map_covers(base, base + bytes, size, stride))
            goto failed;
        ++affinities;
    }
    if (!affinities)
        goto failed;
    for (UINTN at = 0; at < size; at += stride) {
        const EFI_MEMORY_DESCRIPTOR *d = (const void *)(map + at);
        if (ram(d->Type) &&
            !srat_covers(d->PhysicalStart, d->PhysicalStart + d->NumberOfPages * 4096, length))
            goto failed;
    }
    say("SRATMAP PASSED EFI RAM and SRAT affinity unions agree\n");
    return EFI_SUCCESS;
failed:
    say("SRATMAP FAILED EFI RAM and SRAT affinity unions differ\n");
    return EFI_SUCCESS;
}
