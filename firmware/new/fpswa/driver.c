/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include "fpswa.h"

static EFI_GUID guid = FPSWA_GUID;
static struct fpswa_interface interface = {FPSWA_REVISION, 0, fpswa_emulation_entry};
static EFI_RUNTIME_SERVICES *runtime;
static EFI_EVENT virtual_event;

static VOID EFIAPI convert(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    /* RuntimeDxe relocates code, GOT and the descriptor's entry/GP. Its
     * relocations do not convert our protocol's pointer to that descriptor. */
    runtime->ConvertPointer(0, (VOID **)&interface.fpswa);
    runtime->ConvertPointer(0, (VOID **)&runtime);
}
EFI_STATUS EFIAPI _ModuleEntryPoint(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    EFI_BOOT_SERVICES *bs = system->BootServices;
    struct fpswa_interface *existing;
    if (!EFI_ERROR(bs->LocateProtocol(&guid, NULL, (VOID **)&existing)))
        return EFI_ALREADY_STARTED;
    runtime = system->RuntimeServices;
    EFI_STATUS status = bs->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, convert, NULL,
                                          &gEfiEventVirtualAddressChangeGuid, &virtual_event);
    if (EFI_ERROR(status))
        return status;
    status = bs->InstallProtocolInterface(&image, &guid, EFI_NATIVE_INTERFACE, &interface);
    if (EFI_ERROR(status))
        bs->CloseEvent(virtual_event);
    return status;
}
