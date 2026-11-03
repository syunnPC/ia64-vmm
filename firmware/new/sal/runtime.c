/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../platform.h"
#include "sal.h"
#include <Library/UefiRuntimeServicesTableLib.h>
struct sal_state sal_runtime;
struct sal_call_result sal_entry(uint64_t index, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                 uint64_t e, uint64_t f, uint64_t g)
{
    return sal_dispatch(&sal_runtime, index, a, b, c, d, e, f, g);
}
VOID EFIAPI sal_virtual(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    /* VmmTables publishes runtime UC ECAM through the GCD before EBS. */
    sal_runtime.ecam_available = !EFI_ERROR(gRT->ConvertPointer(0, (VOID **)&sal_runtime.ecam));
    /* OS_BOOT_RENDEZ may be registered in virtual mode; APs still read
     * ap_wake_block physically. Convert only the runtime access pointer. */
    if (sal_runtime.ap && EFI_ERROR(gRT->ConvertPointer(0, (VOID **)&sal_runtime.ap)))
        sal_runtime.ap = NULL;
    /* Likewise OS_MCA/OS_INIT registrations; SALE_ENTRY reads them physically. */
    if (sal_runtime.mc && EFI_ERROR(gRT->ConvertPointer(0, (VOID **)&sal_runtime.mc)))
        sal_runtime.mc = NULL;
}
