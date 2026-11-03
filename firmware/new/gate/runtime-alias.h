/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_RUNTIME_ALIAS_H
#define VMM_RUNTIME_ALIAS_H
/* UEFI GetMemoryMap identifies the framebuffer and assigned PCI windows.
 * A large WB TR must never alias a descriptor for device memory. */
static BOOLEAN alias_safe(UINT64 base, UINT64 bytes, const VOID *map, UINTN map_size, UINTN stride)
{
    for (UINTN at = 0; at < map_size; at += stride) {
        const EFI_MEMORY_DESCRIPTOR *d = (const VOID *)((const UINT8 *)map + at);
        if (d->Type != EfiMemoryMappedIO && d->Type != EfiMemoryMappedIOPortSpace)
            continue;
        UINT64 start = d->PhysicalStart, length = d->NumberOfPages * 4096;
        if (length && start < base + bytes && base < start + length)
            return FALSE;
    }
    return TRUE;
}
/* SDM Vol.2 section 4.1.1.3: supported TR sizes in this loader, from 1 GiB
 * down to 4 KiB. Select the largest device-free span containing the address. */
static UINTN alias_shift(UINT64 address, const VOID *map, UINTN size, UINTN stride)
{
    for (UINTN shift = 30;; shift -= 2) {
        UINT64 bytes = 1ULL << shift;
        if (alias_safe(address & ~(bytes - 1), bytes, map, size, stride))
            return shift;
        if (shift == 12)
            return 0;
    }
}
#endif
