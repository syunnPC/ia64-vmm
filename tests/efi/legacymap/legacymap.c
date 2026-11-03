/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Check the UC MMIO map of legacy PCI memory (0xa0000-0xfffff) and
 * complete RAM backing for every advertised 8 KiB granule. */
#include <efi.h>
#include "../../../include/ia64vmm/platform.h"

static unsigned failures;
static unsigned char buffer[64 * 1024];
static void say(const char *s)
{
    while (*s)
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE) = (unsigned char)*s++;
}
static void hex(UINT64 v)
{
    char b[17];
    for (int i = 15; i >= 0; --i, v >>= 4)
        b[i] = "0123456789abcdef"[v & 15];
    b[16] = 0;
    say(b);
}
static void check(int ok, const char *what, UINT64 value)
{
    say(ok ? "LEGACYMAP PASS " : "LEGACYMAP FAIL ");
    say(what);
    say(" = ");
    hex(value);
    say("\n");
    failures += !ok;
}
/* WB memory of a type other than MMIO and port space: RAM. */
static int ram_at(const unsigned char *map, UINTN size, UINTN stride, UINT64 page)
{
    for (UINTN off = 0; off + stride <= size; off += stride) {
        const EFI_MEMORY_DESCRIPTOR *d = (const void *)(map + off);
        if (page >= d->PhysicalStart && page - d->PhysicalStart < d->NumberOfPages * 4096)
            return d->Type != EfiMemoryMappedIO && d->Type != EfiMemoryMappedIOPortSpace &&
                   (d->Attribute & EFI_MEMORY_WB);
    }
    return 0;
}

EFI_STATUS _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    UINTN size = sizeof(buffer), key = 0, stride = 0;
    UINT32 version = 0;
    EFI_STATUS status = st->BootServices->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)buffer, &key,
                                                       &stride, &version);
    check(status == EFI_SUCCESS && stride >= sizeof(EFI_MEMORY_DESCRIPTOR), "GetMemoryMap", status);
    if (status != EFI_SUCCESS || stride < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        say("LEGACYMAP FAILED\n");
        return EFI_SUCCESS;
    }
#ifdef LEGACYMAP_REMOVE_DESCRIPTOR
    /* Broken input: remove the descriptor containing C0000 from the actual
     * GetMemoryMap result. The ordinary coverage checks must reject it. */
    for (UINTN off = 0; off + stride <= size; off += stride) {
        const EFI_MEMORY_DESCRIPTOR *d = (const void *)(buffer + off);
        if (d->PhysicalStart <= 0xc0000 && 0xc0000 < d->PhysicalStart + d->NumberOfPages * 4096) {
            for (UINTN j = off; j + stride < size; ++j)
                buffer[j] = buffer[j + stride];
            size -= stride;
            say("LEGACYMAP removed descriptor containing C0000\n");
            break;
        }
    }
#endif
#ifdef LEGACYMAP_ADD_CONVENTIONAL
    /* A reserved page-zero descriptor must not mask a second, allocatable
     * descriptor overlapping it. Exercise this independently of C0000. */
    if (stride > sizeof(buffer) - size) {
        say("LEGACYMAP FAILED buffer full\n");
        return EFI_SUCCESS;
    }
    for (UINTN j = 0; j < stride; ++j)
        buffer[size + j] = 0;
    EFI_MEMORY_DESCRIPTOR *duplicate = (void *)(buffer + size);
    duplicate->Type = EfiConventionalMemory;
    duplicate->NumberOfPages = 2;
    duplicate->Attribute = EFI_MEMORY_WB;
    size += stride;
    say("LEGACYMAP added conventional page 0 overlap\n");
#endif
    unsigned page0 = 0, conventional0 = 0;
    for (UINTN off = 0; off + stride <= size; off += stride) {
        const EFI_MEMORY_DESCRIPTOR *d = (const void *)(buffer + off);
        say("LEGACYMAP descriptor ");
        hex(d->PhysicalStart);
        say(" ");
        hex(d->NumberOfPages);
        say(" ");
        hex(d->Type);
        say(" ");
        hex(d->Attribute);
        say(" ");
        hex(d->VirtualStart);
        say("\n");
        if (d->Type == EfiConventionalMemory && d->NumberOfPages && d->PhysicalStart < 4096)
            ++conventional0;
        if (d->PhysicalStart == 0 && d->NumberOfPages == 1 && d->Type == EfiReservedMemoryType &&
            d->Attribute == EFI_MEMORY_WB)
            ++page0;
    }
    check(page0 == 1, "page 0 reserved WB", page0);
    check(!conventional0, "no conventional page 0 overlap", conventional0);
    /* Every 8 KiB granule of the window (IA-64 OSes use 8 KiB EFI pages)
     * is covered by exactly one descriptor, of type MMIO with UC. */
    for (UINT64 at = 0xa0000; at < 0x100000; at += 0x2000) {
        unsigned covering = 0, mmio = 0;
        for (UINTN off = 0; off + stride <= size; off += stride) {
            const EFI_MEMORY_DESCRIPTOR *d = (const void *)(buffer + off);
            UINT64 end = d->PhysicalStart + d->NumberOfPages * 4096;
            if (at < d->PhysicalStart || at + 0x2000 > end)
                continue;
            ++covering;
            mmio += d->Type == EfiMemoryMappedIO && (d->Attribute & EFI_MEMORY_UC);
        }
        if (covering != 1 || mmio != 1)
            check(0, "legacy window granule", at);
    }
    check(!failures, "legacy window 00000000000a0000-00000000000fffff UC MMIO", failures);
    /* Loader, boot services and conventional memory become the OS's: the
     * 4 KiB pages that complete their edge granules must be RAM too. */
    unsigned granules = 0;
    for (UINTN off = 0; off + stride <= size; off += stride) {
        const EFI_MEMORY_DESCRIPTOR *d = (const void *)(buffer + off);
        if (d->Type != EfiLoaderCode && d->Type != EfiLoaderData &&
            d->Type != EfiBootServicesCode && d->Type != EfiBootServicesData &&
            d->Type != EfiConventionalMemory)
            continue;
        UINT64 end = d->PhysicalStart + d->NumberOfPages * 4096;
        UINT64 edge[2] = {d->PhysicalStart & ~(UINT64)0x1fff, end & 0x1fff ? end : 0};
        for (unsigned i = 0; i < 2; ++i)
            if ((i ? edge[i] != 0 : edge[i] != d->PhysicalStart) &&
                !ram_at(buffer, size, stride, edge[i])) {
                check(0, "RAM granule", edge[i]);
                ++granules;
            }
    }
    check(!granules, "RAM granules whole 8 KiB", granules);
    say(failures ? "LEGACYMAP FAILED\n" : "LEGACYMAP PASSED\n");
    return EFI_SUCCESS;
}
