/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the DXE producer with a boot-services memory-map fixture.
 * SAL 3.2 §3.3.2.3: the type-3 entry describes firmware's live ITR(0). */
#include <assert.h>
#include <string.h>
#undef NULL
#ifndef SAL_DRIVER_SOURCE
#define SAL_DRIVER_SOURCE "firmware/new/acpi/driver.c"
#endif
#include SAL_DRIVER_SOURCE

struct sal_state sal_runtime;
/* On IPF a function pointer addresses this descriptor, not code bytes. */
_Alignas(16) const UINT64 loader_sal_descriptor[2] = {0x200000, 0x201000};
__asm__(".global sal_entry\n.set sal_entry,loader_sal_descriptor");
VOID EFIAPI sal_virtual(EFI_EVENT e, VOID *p)
{
    (void)e;
    (void)p;
}
static _Alignas(8192) UINT8 output[16384];
static UINT64 control[1024];
static EFI_MEMORY_DESCRIPTOR map;
static VOID *installed;

static EFI_STATUS EFIAPI pages(EFI_ALLOCATE_TYPE how, EFI_MEMORY_TYPE type, UINTN count,
                               EFI_PHYSICAL_ADDRESS *base)
{
    assert(how == AllocateAnyPages && type == EfiRuntimeServicesData && count == 4);
    memset(output, 0xcc, sizeof(output));
    *base = (UINTN)output;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI memory_map(UINTN *size, EFI_MEMORY_DESCRIPTOR *buffer, UINTN *key,
                                    UINTN *stride, UINT32 *version)
{
    *stride = sizeof(map);
    *version = 1;
    *key = 1;
    if (!buffer) {
        *size = sizeof(map);
        return EFI_BUFFER_TOO_SMALL;
    }
    assert(*size >= sizeof(map));
    *buffer = map;
    *size = sizeof(map);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI pool(EFI_MEMORY_TYPE type, UINTN bytes, VOID **buffer)
{
    assert(type == EfiBootServicesData && bytes >= sizeof(map));
    *buffer = &map;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_pool(VOID *p)
{
    assert(p == &map);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_pages(EFI_PHYSICAL_ADDRESS base, UINTN count)
{
    (void)base;
    (void)count;
    assert(0);
    return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI event(UINT32 type, EFI_TPL tpl, EFI_EVENT_NOTIFY fn, const VOID *context,
                               const EFI_GUID *guid, EFI_EVENT *out)
{
    (void)context;
    (void)guid;
    assert(type == EVT_NOTIFY_SIGNAL && tpl == TPL_NOTIFY && fn == sal_virtual);
    *out = (VOID *)1;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_event(EFI_EVENT e)
{
    (void)e;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI install(EFI_GUID *guid, VOID *table)
{
    assert(!memcmp(guid, &gEfiSalSystemTableGuid, sizeof(*guid)));
    installed = table;
    return EFI_SUCCESS;
}
EFI_GUID gEfiSalSystemTableGuid = SAL_SYSTEM_TABLE_GUID;
EFI_GUID gEfiEventVirtualAddressChangeGuid = EFI_EVENT_GROUP_VIRTUAL_ADDRESS_CHANGE;
static EFI_BOOT_SERVICES services = {.AllocatePages = pages,
                                     .FreePages = free_pages,
                                     .GetMemoryMap = memory_map,
                                     .AllocatePool = pool,
                                     .FreePool = free_pool,
                                     .CreateEventEx = event,
                                     .CloseEvent = close_event,
                                     .InstallConfigurationTable = install};
EFI_BOOT_SERVICES *gBS = &services;

int main(void)
{
    struct ia64vmm_handoff h = {.firmware_base = 0x900000,
                                .cpu_count = 2,
                                .fw_data_base = (UINTN)control - PLATFORM_CONTROL_OFFSET};
    struct ap_wake_block ap = {0};
    /* SALE_ENTRY's block (sal/mc.h) gets the PAL_PROC, SAL_PROC and SAL gp
     * that OS_MCA and OS_INIT receive (SAL §4.8, §5.3). */
    static struct sal_mc mc;
    map = (EFI_MEMORY_DESCRIPTOR){
        .Type = EfiRuntimeServicesCode, .PhysicalStart = h.firmware_base, .NumberOfPages = 256};
    const uint64_t addresses[] = {0x900000, 0x40300000}, itirs[] = {18u << 2, 20u << 2};
    for (unsigned n = 0; n < 2; ++n) {
        memset(control, 0xa5, sizeof(control));
        control[CONTROL_BootItrAddress / 8] = addresses[n];
        control[CONTROL_BootItrItir / 8] = itirs[n];
        memset(&mc, 0, sizeof(mc));
        control[CONTROL_SalMc / 8] = n ? 0 : (UINTN)&mc;
        assert(sal_table(&h, &ap) == EFI_SUCCESS && installed == output);
        if (!n)
            assert(sal_runtime.mc == &mc && mc.pal_proc == h.firmware_base &&
                   mc.sal_proc == 0x200000 && mc.sal_gp == 0x201000 && !mc.active &&
                   !mc.generation);
        else
            assert(!sal_runtime.mc);
        assert(table_sum(output, (size_t)table_get(output + 4, 4)) == 0);
        const unsigned sizes[] = {48, 32, 16, 32, 16, 16};
        const UINT8 *tr = NULL;
        for (size_t at = 96; at < table_get(output + 4, 4); at += sizes[output[at]]) {
            assert(output[at] < 6);
            if (output[at] == 3) {
                assert(!tr);
                tr = output + at;
            }
        }
        assert(tr && tr[1] == 0 && tr[2] == 0);
        assert(table_get(tr + 8, 8) == addresses[n] && table_get(tr + 16, 8) == itirs[n]);
        assert(sal_runtime.ap == &ap);
    }
    return 0;
}
