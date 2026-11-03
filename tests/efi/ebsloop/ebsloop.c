/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A loader the VMM does not recognise. After ExitBootServices
 * it keeps running in its own image (never in another loader allocation,
 * which is what the VMM takes for the kernel entry), computes for a while
 * and shuts down through ResetSystem. tests/qemu/apps.py expects the
 * VMM's post-EBS single-step observation to stop at its bound and normal
 * budgets to resume.
 */
#include <efi.h>
#include "../../../include/ia64vmm/platform.h"
static void say(const char *s)
{
    while (*s)
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE) = (unsigned char)*s++;
}
static void hex(UINT64 n)
{
    const char *digits = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        char s[2] = {digits[(n >> shift) & 15], 0};
        say(s);
    }
}
EFI_STATUS EFIAPI _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    UINTN size, key, stride;
    UINT32 version;
    VOID *map;
    EFI_STATUS status = st->BootServices->AllocatePool(EfiLoaderData, 65536, &map);
    if (EFI_ERROR(status))
        return status;
    for (unsigned retry = 0; retry < 2; ++retry) {
        size = 65536;
        status = st->BootServices->GetMemoryMap(&size, map, &key, &stride, &version);
        if (EFI_ERROR(status))
            return status;
        status = st->BootServices->ExitBootServices(image, key);
        if (!EFI_ERROR(status))
            break;
    }
    if (EFI_ERROR(status))
        return status;
    say("ebsloop: running in the loader image after ExitBootServices\n");
    volatile UINT64 sum = 0;
    for (UINT64 i = 0; i < 2000000; ++i)
        sum += i ^ (sum >> 3);
    say("ebsloop: done ");
    hex(sum);
    say("\n");
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, 0);
    return EFI_SUCCESS;
}
