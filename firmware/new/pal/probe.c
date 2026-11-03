/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PAL vectors ported from tests/ia64/pal-tests.h to actual firmware calls.
 * Calls execute native guest PAL code, including its hardware service paths.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Guid/Acpi.h>
#include <Guid/SmBios.h>
#include <Guid/SalSystemTable.h>
#include "../acpi/tables.h"
#include "../sal/sal.h"
#include "../platform.h"
#include "../ivt-layout.h"
extern UINT64 probe_unaligned(VOID *buffer);
extern VOID gate_enable_translation(VOID);
extern UINT64 probe_unaligned_stacked(VOID *buffer);
extern VOID probe_unaligned_fp(VOID *buffer);
extern VOID probe_raw_static(UINT64, UINT64, UINT64, UINT64, UINT64, UINT64 *);
static bool madison(void)
{
    UINT64 index = 3, id;
    __asm__ volatile("mov %0=cpuid[%1]" : "=r"(id) : "r"(index));
    return ((id >> 24) & 255) == 31;
}
static const char *model_brand(void)
{
    return madison() ? "VMM Madison-compatible IA-64 CPU 1.60GHz 3MB"
                     : "VMM Montecito-compatible IA-64 CPU 1.60GHz 12MB";
}

static UINT64 sal_descriptor[2], pci_id, pci_address, sal_tr_address, sal_tr_itir;
static volatile struct ap_wake_block *ap;
static void text(const char *s)
{
    while (*s)
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE) = (UINT8)*s++;
}
static void check(bool value, const char *reason)
{
    if (value)
        return;
    text("PAL probe FAIL: ");
    text(reason);
    text("\n");
    gRT->ResetSystem(EfiResetShutdown, EFI_ABORTED, 0, NULL);
    for (;;)
        __asm__ volatile("hint @pause");
}
static void raw_test(UINT64 entry, UINT64 index, UINT64 a, UINT64 b, UINT64 c)
{
    UINT64 preserved[256], psr;
    SetMem(preserved, sizeof(preserved), 0);
    /* SDM Vol.2 Table 11-56: PAL is entered with PSR.i = 0. An ITM tick in
     * the raw window would also reuse the bank-zero GRs checked below. */
    __asm__ volatile("mov %0=psr;; rsm psr.i;; srlz.d;;" : "=r"(psr)::"memory");
    probe_raw_static(entry, index, a, b, c, preserved);
    if (psr & (1u << 14))
        __asm__ volatile("ssm psr.i;; srlz.d;;" ::: "memory");
    check(CompareMem(preserved + 32, preserved + 128, 96 * 8) == 0, "raw PAL rotating stacked GRs");
    const unsigned gr[] = {1, 4, 5, 6, 7, 12, 13};
    for (unsigned j = 0; j < 7; ++j)
        check(preserved[224 + j] == gr[j], "raw PAL static preserved GR");
    check((preserved[231] & 0xffffffffffff003eULL) ==
              (0xaaaaaaaaaaaaaaabULL & 0xffffffffffff003eULL),
          "raw PAL preserved PR");
    for (unsigned j = 0; j < 5; ++j)
        check(preserved[232 + j] == (j + 1) * 256, "raw PAL preserved BR");
    for (unsigned j = 0; j < 8; ++j)
        check(preserved[240 + j] == 4016 + j, "raw PAL bank-zero GR");
}
/* Locate ap_wake_block like DXE does, through the handoff HOB and the
 * control block; the HOB list is an EFI configuration table. */
static volatile struct ap_wake_block *ap_block(UINT32 *cpus)
{
    static const EFI_GUID hob_guid = {
        0x7739f24c, 0x93d7, 0x11d4, {0x9a, 0x3a, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
    static const EFI_GUID handoff_guid = PLATFORM_HANDOFF_GUID;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; ++i) {
        if (!CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &hob_guid))
            continue;
        EFI_PEI_HOB_POINTERS hob = {.Raw = gST->ConfigurationTable[i].VendorTable};
        for (; !END_OF_HOB_LIST(hob); hob.Raw = GET_NEXT_HOB(hob)) {
            if (GET_HOB_TYPE(hob) != EFI_HOB_TYPE_GUID_EXTENSION ||
                !CompareGuid(&hob.Guid->Name, &handoff_guid))
                continue;
            const struct ia64vmm_handoff *h = GET_GUID_HOB_DATA(hob.Guid);
            *cpus = h->cpu_count;
            return (VOID *)(UINTN)((
                const UINT64 *)(UINTN)(h->fw_data_base +
                                       PLATFORM_CONTROL_OFFSET))[CONTROL_ApBlock / 8];
        }
    }
    return NULL;
}
/* Loader-side map setup reused from firmware/new/gate/behaviour.c.
 * Calls gate/virtual.S for translation activation; see its Asteria citation. */
static void halt_test(UINT64 pal)
{
    UINT64 psr, tpr, itv, itm, itc, after, vector;
    __asm__ volatile("mov %0=psr; mov %1=cr.tpr; mov %2=cr.itv; mov %3=cr.itm;;"
                     : "=r"(psr), "=r"(tpr), "=r"(itv), "=r"(itm));
    __asm__ volatile(
        "rsm psr.i;; srlz.d;; mov cr.tpr=r0; mov cr.itv=%0;; srlz.d;;" ::"r"(UINT64_C(1) << 16)
        : "memory");
    /* A boot-services timer may already be pending when this app starts.
     * Consume it before arming our vector; HALT correctly wakes for either.
     * Leave enough time to program ITM on a slow backend, then make the
     * interrupt pending before testing the light-halt exit condition. */
    do {
        __asm__ volatile("mov %0=cr.ivr;;" : "=r"(vector));
        if (vector != 15)
            __asm__ volatile("mov cr.eoi=r0;;" ::: "memory");
    } while (vector != 15);
    __asm__ volatile("mov %0=ar.itc;;" : "=r"(itc));
    UINT64 target = itc + 40000000, value = 0xff;
    __asm__ volatile("mov cr.itv=%0; mov cr.itm=%1;; srlz.d;;" ::"r"(value), "r"(target)
                     : "memory");
    do {
        __asm__ volatile("mov %0=ar.itc;;" : "=r"(itc));
    } while (itc < target);
    PAL_CALL_RETURN r = AsmPalCall(pal, 29, 0, 0, 0);
    raw_test(pal, 29, 0, 0, 0);
    __asm__ volatile("mov %0=psr; mov %1=cr.ivr;;" : "=r"(after), "=r"(vector));
    if (after != (psr & ~(UINT64_C(1) << 14)) || vector != 0xff) {
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_DEBUG) = psr;
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_DEBUG) = after;
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_DEBUG) = vector;
    }
    check(r.Status == 0 && !r.r9 && !r.r10 && !r.r11 && after == (psr & ~(UINT64_C(1) << 14)) &&
              vector == 0xff,
          "HALT_LIGHT timer and exact PSR");
    __asm__ volatile(
        "mov cr.eoi=r0; mov cr.tpr=%0; mov cr.itv=%1; mov cr.itm=%2;; srlz.d;; mov psr.l=%3;; srlz.d;;" ::
            "r"(tpr),
        "r"(itv), "r"(itm), "r"(psr)
        : "memory");
}
static void virtual_calls(EFI_HANDLE image, UINT64 pal)
{
    UINTN size = 0, key, stride;
    UINT32 version;
    check(gBS->GetMemoryMap(&size, NULL, &key, &stride, &version) == EFI_BUFFER_TOO_SMALL,
          "virtual map size");
    size += 16 * stride;
    VOID *map;
    check(gBS->AllocatePool(EfiLoaderData, size, &map) == EFI_SUCCESS, "virtual map pool");
    check(gBS->GetMemoryMap(&size, map, &key, &stride, &version) == EFI_SUCCESS, "virtual map");
    check(gBS->ExitBootServices(image, key) == EFI_SUCCESS, "ExitBootServices");
    for (UINTN at = 0; at < size; at += stride) {
        EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)map + at);
        d->VirtualStart = (d->Attribute & EFI_MEMORY_RUNTIME)
                              ? d->PhysicalStart + (d->Type == EfiMemoryMappedIO &&
                                                            d->PhysicalStart == IA64VMM_ECAM_BASE
                                                        ? 0xc000000000000000ULL
                                                        : 0xe000000000000000ULL)
                              : d->PhysicalStart;
        if (d->PhysicalStart == IA64VMM_ECAM_BASE)
            check(d->Type == EfiMemoryMappedIO &&
                      (d->Attribute & (EFI_MEMORY_UC | EFI_MEMORY_RUNTIME)) ==
                          (EFI_MEMORY_UC | EFI_MEMORY_RUNTIME),
                  "runtime UC ECAM map");
    }
    /* Bare-metal EFI allocations may be above 1 GiB. Pin all low 4 GiB
     * for this loader fixture's RAM aliases, stack and backing store. */
    UINT64 region = 0xe000000000000000ULL, rr = (7 << 8) | (12 << 2), itir = 32 << 2, pte = 0x661,
           tr = 0, iva;
    __asm__ volatile("mov %0=cr.iva" : "=r"(iva));
    __asm__ volatile("rsm psr.ic;; srlz.d;;" ::: "memory");
    __asm__ volatile("mov rr[%0]=%1;; srlz.d;;" ::"r"(region), "r"(rr) : "memory");
    __asm__ volatile(
        "mov cr.ifa=%0; mov cr.itir=%1;; itr.i itr[%2]=%3;; srlz.i;; itr.d dtr[%2]=%3;; srlz.d;;" ::
            "r"(region),
        "r"(itir), "r"(tr), "r"(pte)
        : "memory");
    region += IA64VMM_SERVICE_BASE & ~0xffffffULL;
    itir = 24 << 2;
    pte = (IA64VMM_SERVICE_BASE & ~0xffffffULL) | 0x671;
    tr = 1;
    __asm__ volatile("mov cr.ifa=%0; mov cr.itir=%1;; itr.d dtr[%2]=%3;; srlz.d;;" ::"r"(region),
                     "r"(itir), "r"(tr), "r"(pte)
                     : "memory");
    region = iva & ~0x3ffffULL;
    itir = 18 << 2;
    pte = region | 0x661;
    tr = 2;
    __asm__ volatile("mov cr.ifa=%0; mov cr.itir=%1;; itr.i itr[%2]=%3;; srlz.i;;" ::"r"(region),
                     "r"(itir), "r"(tr), "r"(pte)
                     : "memory");
    region = 0;
    itir = 32 << 2;
    pte = 0x661;
    __asm__ volatile("mov cr.ifa=%0; mov cr.itir=%1;; itr.d dtr[%2]=%3;; srlz.d;;" ::"r"(region),
                     "r"(itir), "r"(tr), "r"(pte)
                     : "memory");
    region = 0xc000000000000000ULL;
    rr = (6 << 8) | (28 << 2);
    __asm__ volatile("mov rr[%0]=%1;; srlz.d;;" ::"r"(region), "r"(rr) : "memory");
    region += IA64VMM_ECAM_BASE;
    itir = 28 << 2;
    pte = IA64VMM_ECAM_BASE | 0x671;
    tr = 3;
    __asm__ volatile("mov cr.ifa=%0; mov cr.itir=%1;; itr.d dtr[%2]=%3;; srlz.d;;" ::"r"(region),
                     "r"(itir), "r"(tr), "r"(pte)
                     : "memory");
    __asm__ volatile("ssm psr.ic;; srlz.d;;" ::: "memory");
    check(gRT->SetVirtualAddressMap(size, stride, version, map) == EFI_SUCCESS,
          "SetVirtualAddressMap");
    gate_enable_translation();
    UINT64 alias = 0xe000000000000000ULL;
    PAL_CALL_RETURN r = AsmPalCall(pal + alias, 19, 0, 0, 0);
    check(r.Status == 0 && r.r9 == 96, "virtual static PAL");
    CHAR8 brand[128];
    r = AsmPalCall(pal + alias, 274, 0, (UINTN)brand + alias, 0);
    check(r.Status == 0 && AsciiStrCmp(brand, model_brand()) == 0,
          "virtual stacked PAL and output buffer");
    volatile UINT64 descriptor[2] = {sal_descriptor[0] + alias, sal_descriptor[1] + alias};
    typedef struct sal_call_result (*sal_call)(UINT64, UINT64, UINT64, UINT64, UINT64, UINT64,
                                               UINT64, UINT64);
    struct sal_call_result s = ((sal_call)(VOID *)descriptor)(0x01000012, 0, 0, 0, 0, 0, 0, 0);
    check(s.status == 0 && s.v0 == IA64VMM_PLATFORM_BASE_HZ, "virtual SAL entry and GP");
    s = ((sal_call)(VOID *)descriptor)(0x01000010, pci_address, 4, 0, 0, 0, 0, 0);
    check(s.status == 0 && s.v0 == pci_id, "virtual SAL PCI vendor/device through region 6 UC");
    /* Register OS_BOOT_RENDEZ after SetVirtualAddressMap; APs still read
     * ap_wake_block physically through the region-zero identity DTR. */
    s = ((sal_call)(VOID *)descriptor)(0x01000000, 2, 0x240000, 0x201000, 0, 0, 0, 0);
    check(s.status == 0 && ap->rendez_entry == 0x240000 && ap->rendez_gp == 0x201000,
          "virtual SAL OS_BOOT_RENDEZ reaches ap_wake_block");
    s = ((sal_call)(VOID *)descriptor)(0x01000000, 2, 0, 0, 0, 0, 0, 0);
    check(s.status == 0 && !ap->rendez_entry, "virtual SAL OS_BOOT_RENDEZ clear");
    halt_test(pal + alias);
    text("PAL probe: nonidentity virtual PAL and runtime SAL calls passed\n");
    gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
}
EFI_STATUS EFIAPI GateHello(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    (void)image;
    (void)system;
    UINTN bytes = 0, key, stride;
    UINT32 version;
    check(gBS->GetMemoryMap(&bytes, NULL, &key, &stride, &version) == EFI_BUFFER_TOO_SMALL,
          "map size");
    bytes += 4096;
    VOID *map;
    check(gBS->AllocatePool(EfiBootServicesData, bytes, &map) == EFI_SUCCESS, "map pool");
    check(gBS->GetMemoryMap(&bytes, map, &key, &stride, &version) == EFI_SUCCESS, "map");
    UINT64 entry = 0;
    for (UINTN at = 0; at < bytes; at += stride) {
        EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)map + at);
        if (d->Type == EfiPalCode)
            entry = d->PhysicalStart;
    }
    gBS->FreePool(map);
    check(entry != 0, "EfiPalCode entry");
    struct vector {
        unsigned index;
        UINT64 a, b, c;
        INT64 status;
        UINT64 x, y, z;
    } vectors[] = {
        {20, 0, 0, 0, 0, 0x96801000968ULL, 0x96801000968ULL, 0},
        {19, 0, 0, 0, 0, 96, 0, 0},
        {13, 0, 0, 0, 0, 100000000, 0, 0},
        {14, 0, 0, 0, 0, 0x1000000001ULL, 0x400000001ULL, 0x400000001ULL},
        {8, 0, 0, 0, 0, 0x02041f1f080f1865ULL, 0x183c, 0},
        {7, 0, 1, 0, 0, 0x202001, 0x1000, 0},
        {7, 0, 2, 0, 0, 0x202001, 0x1000, 0},
        {7, 1, 1, 0, 0, 0x500808001ULL, 0x155557000ULL, 0},
        {7, 1, 2, 0, 0, 0x500808001ULL, 0x155557000ULL, 0},
        {7, 2, 1, 0, -2, 0, 0, 0},
        {7, 2, 2, 0, -2, 0, 0, 0},
        {7, 0, 0, 0, -2, 0, 0, 0},
        {7, 0, 3, 0, -2, 0, 0, 0},
        {34, 0, 0, 0, 0, 0x155557000ULL, 0x155557000ULL, 0},
        {6, 0, 0, 0, 0, 0, 0x100000001ULL, 0},
        {5, 0, 0, 0, 0, 0x71, 0, 0},
        {4, 0, 0, 0, 0, 3, 5, 0},
        {11, 0, 0, 0, 0, 4, 4, 0},
        {12, 0, 0, 0, 0, 0, 0, 0},
        {42, 0, 0, 0, 0, 0x100010001ULL, 0, 0},
        {42, ~0ULL, 0, 0, 0, 0x100010001ULL, 0, 0},
        {42, 1, 0, 0, -2, 0, 0, 0},
        {39, 0, 0, 0, 0, 0x11117f2f00ffULL, 7, 0},
        {39, 1, 0, 0, 0, 0, 0, 0},
        {39, 2, 0, 0, 0, 0x3fb0107, 0x307ff, 0},
        {39, 3, 0, 0, 0, 0, 2, 0},
        {39, 4, 0, 0, -2, 0, 0, 0},
        {30, 0, 0, 0, 0, 32768, 8192, 0},
        {30, 1, 0, 0, -2, 0, 0, 0},
        {30, 2, 0, 0, -2, 0, 0, 0},
        {41, 0, 0, 0, 1, 0, 0, 0},
        {41, 1, 0, 0, 1, 0, 0, 0},
        {41, 2, 0, 0, -2, 0, 0, 0},
        {37, 0, 0, 0, 0, 0, 1, 0},
        {37, 1, 0, 0, 0, 0, 1, 0},
        {37, 2, 0, 0, -2, 0, 0, 0},
        {21, 0, 0, 0, 0, 0, 0, 0},
        {22, 0, 0, 0, 0, 0, 0, 0},
        {25, 0, 0, 0, -6, 0, 0, 0},
        {25, 1, 0, 0, -6, 0, 0, 0},
        {25, 2, 0x100, 4, -6, 0, 0, 0},
        {25, 2, 0x100, 5, -2, 0, 0, 0},
        {25, 2, 0x300, 0, -2, 0, 0, 0},
        {25, 2, 0x101, 0, -2, 0, 0, 0},
        {24, 0, 0, 0, -1, 0, 0, 0},
        {24, 8, 0, 0, -2, 0, 0, 0},
        {24, 1, 1, 0, -2, 0, 0, 0},
        {26, 0, 0, 0, -2, 0, 0, 0},
        {258, 0, 4096, 0, -2, 0, 0, 0},
        {258, 0, 4096, 0x10001, -2, 0, 0, 0},
        {258, 0, 4096, 1, 0, 4, 0, 0},
        {274, 16, 0, 0, 0, 1600000000, 0, 0},
        {274, 17, 0, 0, 0, 12582912, 0, 0},
        {274, 18, 0, 0, 0, 400000000, 0, 0},
        {274, 19, 0, 0, -2, 0, 0, 0},
        {3, 0, 3, 1, 0, 0, 0, 0},
        {3, 3, 1, 0, -2, 0, 0, 0},
        {3, 0, 0, 0, -2, 0, 0, 0},
        {1, 0, 0, 0, -2, 0, 0, 0},
        {1, 1, 0, 0, 0, 0, 0, 0},
        {28, 1, 0, 0, -1, 0, 0, 0},
        {261, 32, 0, 0, -2, 0, 0, 0},
        {261, 0, 2, 0, -2, 0, 0, 0},
        {261, 0, 0, 1, -2, 0, 0, 0},
        {48, 0, 0, 0, -1, 0, 0, 0},
        {262, 0, 0, 0, -1, 0, 0, 0},
    };
    if (madison())
        for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
            struct vector *v = &vectors[i];
            if (v->index == 8)
                v->x = 0x02043f3f080f1865ULL;
            if (v->index == 261 && v->a == 32)
                v->a = 64;
            if (v->index == 20)
                v->x = v->y = 0x57301000573ULL;
            if (v->index == 4)
                v->y = 4;
            if (v->index == 42) {
                v->status = -1;
                v->x = v->y = v->z = 0;
            }
            if (v->index == 274 && v->a == 17)
                v->x = 3145728;
        }
    const struct {
        unsigned level, type;
        UINT64 x[2], y[2];
    } caches[] = {{0, 1, {0x01ff06060400, 0x01ff06060400}, {0x310c0c00004000, 0x310c0c00004000}},
                  {0, 2, {0x010106060400, 0x010106060400}, {0x310c0c00004000, 0x310c0c00004000}},
                  {1, 1, {0x07ff07070800, 0}, {0x31110c00100000, 0}},
                  {1, 2, {0x050107070802, 0x050107070803}, {0x310f0c00040000, 0x310f0c00040000}},
                  {2, 2, {0x0e0107070c03, 0x0e0107070c03}, {0x31140c00c00000, 0x31120c00300000}}};
    unsigned model = madison();
    for (unsigned i = 0; i < sizeof(caches) / sizeof(caches[0]); ++i) {
        PAL_CALL_RETURN r = AsmPalCall(entry, 2, caches[i].level, caches[i].type, 0);
        check((INT64)r.Status == (model && i == 2 ? -2 : 0) && r.r9 == caches[i].x[model] &&
                  r.r10 == caches[i].y[model] && !r.r11,
              "selected cache words");
    }
    for (unsigned set = 0; set < 21; ++set) {
        INT64 status = !set        ? 0
                       : set < 16  ? -2
                       : model     ? (set == 16 ? 0 : -8)
                       : set < 18  ? 1
                       : set == 18 ? 0
                                   : -8;
        UINT64 bits = !set ? (UINT64_C(1) << 41) : 0;
        PAL_CALL_RETURN r = AsmPalCall(entry, 17, 0, set, 0);
        check((INT64)r.Status == status && r.r9 == bits && r.r10 == bits && !r.r11,
              "selected feature words");
    }
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        struct vector *v = &vectors[i];
        PAL_CALL_RETURN r = AsmPalCall(entry, v->index, v->a, v->b, v->c);
        if (v->index < 256)
            raw_test(entry, v->index, v->a, v->b, v->c);
        if ((INT64)r.Status != v->status || r.r9 != v->x || r.r10 != v->y || r.r11 != v->z) {
            *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_DEBUG) = v->index;
            check(false, "PAL result vector");
        }
    }
    const unsigned reserved[] = {20, 19, 8, 34, 6, 13, 14, 5, 4, 11, 12, 21, 22, 9, 29};
    for (unsigned i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
        for (unsigned a = 0; a < 3; ++a)
            check((INT64)AsmPalCall(entry, reserved[i], a == 0, a == 1, a == 2).Status == -2,
                  "PAL reserved arguments");
    halt_test(entry);
    UINT64 buffer[16];
    SetMem(buffer, sizeof(buffer), 0xa5);
    {
        /* SDM Vol.2 PAL_MC_REGISTER_MEM: UC, 512-byte aligned, whole 4 KB
         * area implemented; size 0 or 4 KB, else -2 with req_size 4; reserved 0. */
        const UINT64 uc = 1ULL << 63, top = 1ULL << 50;
        check(AsmPalCall(entry, 27, uc | (top - 4096), 0, 0).Status == 0,
              "MC_REGISTER_MEM last page");
        check(AsmPalCall(entry, 27, uc | (top - 3584), 0, 0).Status == (UINT64)-2,
              "MC_REGISTER_MEM crosses limit");
        check(AsmPalCall(entry, 27, uc | (top - 512), 0, 0).Status == (UINT64)-2,
              "MC_REGISTER_MEM last 512 bytes");
        check(AsmPalCall(entry, 27, uc | top, 0, 0).Status == (UINT64)-2,
              "MC_REGISTER_MEM beyond limit");
        check(AsmPalCall(entry, 27, uc | (1ULL << 62), 0, 0).Status == (UINT64)-2,
              "MC_REGISTER_MEM reserved address bit");
        check(AsmPalCall(entry, 27, top - 4096, 0, 0).Status == (UINT64)-2,
              "MC_REGISTER_MEM cacheable");
        check(AsmPalCall(entry, 27, uc | ((UINTN)buffer & ~511ULL) | 256, 0, 0).Status ==
                  (UINT64)-2,
              "MC_REGISTER_MEM alignment");
        PAL_CALL_RETURN mc = AsmPalCall(entry, 27, uc | ((UINTN)buffer & ~511ULL), 4, 0);
        check(mc.Status == 0 && mc.r9 == 0, "MC_REGISTER_MEM size 4");
        mc = AsmPalCall(entry, 27, uc | (((UINTN)buffer & ~511ULL) + 512), 8, 0);
        check(mc.Status == (UINT64)-2 && mc.r9 == 4, "MC_REGISTER_MEM size 8");
        mc = AsmPalCall(entry, 27, uc | (((UINTN)buffer & ~511ULL) + 512), 1, 0);
        check(mc.Status == (UINT64)-2 && mc.r9 == 4, "MC_REGISTER_MEM size 1");
        check(AsmPalCall(entry, 27, uc | ((UINTN)buffer & ~511ULL), 0, 1).Status == (UINT64)-2,
              "MC_REGISTER_MEM reserved argument");
        check(AsmPalCall(entry, 27, uc | ((UINTN)buffer & ~511ULL), 0, 0).Status == 0,
              "MC_REGISTER_MEM valid buffer");
    }

    check(AsmPalCall(entry, 15, (UINTN)buffer, 0, 0).Status == 0 && buffer[0] == 0x3fff &&
              buffer[4] == 0x3ffff && buffer[8] == 0xf0 && buffer[12] == 0xf0,
          "PMU buffer");
    check(AsmPalCall(entry, 257, (UINTN)buffer, 0, 0).Status == 0 &&
              buffer[0] == 0x3000000000000000ULL && buffer[1] == 0 && buffer[7] == 0,
          "halt buffer");
    /* ITR(0) is the firmware's code mapping: the SAL type-3 descriptor names
     * it (SAL 245359-005 §3.2.5). Whether IFA carries the region number
     * (SDM Vol.2 Fig. 4-5, §4.1.1.4) is implementation-dependent. */
    const UINT64 vpn_mask = (UINT64_C(1) << 61) - 1;
    UINT64 rr0;
    __asm__ volatile("mov %0=rr[%1]" : "=r"(rr0) : "r"(UINT64_C(0)));
    /* ITR(1) is never inserted. */
    PAL_CALL_RETURN empty = AsmPalCall(entry, 261, 1, 0, (UINTN)buffer);
    check(empty.Status == 0 && empty.r9 == 0 && buffer[0] == 0 && buffer[2] == 0, "empty TR read");
    /* The VA is above the fixture's 4 GiB DTR so the insert does not overlap
     * it (SDM Vol.2 Table 4-1). */
    UINT64 saved_psr, va = 0x123400000ULL, itir = 12 << 2, slot = madison() ? 63 : 31,
                      pte = 0x1200661;
    __asm__ volatile("mov %0=psr;; rsm psr.ic;; srlz.d;;" : "=r"(saved_psr)::"memory");
    __asm__ volatile(
        "mov cr.ifa=%0; mov cr.itir=%1;; itr.d dtr[%2]=%3;; srlz.d;; mov psr.l=%4;; srlz.d;;" ::"r"(
            va),
        "r"(itir), "r"(slot), "r"(pte), "r"(saved_psr)
        : "memory");
    PAL_CALL_RETURN tr_result = AsmPalCall(entry, 261, slot, 1, (UINTN)buffer);
    check(tr_result.Status == 0 && tr_result.r9 == 15 && buffer[0] == pte && buffer[1] == itir &&
              (buffer[2] & vpn_mask) == (va | 1) && (buffer[3] >> 8) == ((rr0 >> 8) & 0xffffff) &&
              (buffer[3] & 0xfc) == itir,
          "real DTR snapshot");
    check(AsmPalCall(entry, 261, 0, 0, IA64VMM_SERVICE_BASE).Status == (UINT64)-2,
          "TR bad MMIO buffer");
    check(AsmPalCall(entry, 261, 0, 0, UINT64_C(1) << 50).Status == (UINT64)-2,
          "TR unimplemented buffer");
    _Alignas(128) UINT64 line[16];
    SetMem(line, sizeof(line), 0);
    /* Which bytes a later load sees is implementation-dependent. */
    check(AsmPalCall(entry, 31, (UINTN)line + 7, 0x123456789abcdef0ULL, 0).Status == 0,
          "cache line init");
    check((INT64)AsmPalCall(entry, 31, UINT64_C(1) << 50, 0, 0).Status == -2,
          "cache line init address");
    check(AsmPalCall(entry, 32, entry, 0, 0).Status == 0, "PMI entry registration");
    const struct {
        unsigned index;
        UINT64 a, b, c;
    } raw_calls[] = {{1, 3, 3, 0},
                     {2, 0, 2, 0},
                     {3, ~0ULL, 0, 0},
                     {9, 0, 0, 0},
                     {10, 0x60000000, 0, 0},
                     {15, (UINTN)buffer, 0, 0},
                     {16, 0, IA64VMM_PIB_BASE, 0},
                     {17, 0, 18, 0},
                     {18, 0xa0, 18, 0},
                     {23, 1, 0, 0},
                     {27, 0x8000000000000000ULL | ((UINTN)line & ~511ULL), 0, 0},
                     {31, (UINTN)line, 0x123456789abcdef0ULL, 0},
                     {32, entry, 0, 0},
                     {38, 0, 2, 0},
                     {43, 0, 2, 0}};
    for (unsigned i = 0; i < sizeof(raw_calls) / sizeof(raw_calls[0]); ++i)
        raw_test(entry, raw_calls[i].index, raw_calls[i].a, raw_calls[i].b, raw_calls[i].c);
    text("PAL probe: raw static PAL GR/PR/BR and rotating frame preservation passed\n");
    CHAR8 brand[128];
    PAL_CALL_RETURN r = AsmPalCall(entry, 274, 0, (UINTN)brand, 0);
    check(r.Status == 0 && AsciiStrCmp(brand, model_brand()) == 0, "brand buffer");
    EFI_PHYSICAL_ADDRESS copy = 0;
    check(gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesCode, 10, &copy) == EFI_SUCCESS,
          "copy pages");
    copy = (copy + 8191) & ~8191ULL;
    check(AsmPalCall(entry, 256, copy, 32768, 0).Status == 0, "COPY_PAL");
    check(AsmPalCall(copy, 19, 0, 0, 0).r9 == 96 &&
              AsmPalCall(copy, 274, 17, 0, 0).r9 == (madison() ? 3145728 : 12582912),
          "copied PAL");
    check(AsmPalCall(copy, 8, 0, 0, 0).r9 ==
              (madison() ? 0x02043f3f080f1865ULL : 0x02041f1f080f1865ULL),
          "copied TR counts");
    check(AsmPalCall(copy, 7, 1, 2, 0).r9 == 0x500808001ULL, "copied TC counts");
    check(AsmPalCall(copy, 261, slot, 1, (UINTN)buffer).Status == 0 && buffer[0] == pte,
          "copied last DTR");
    /* Leave no probe-owned DTR behind. */
    __asm__ volatile("ptr.d %0,%1;; srlz.d;;" ::"r"(va), "r"(itir) : "memory");
    check(AsmPalCall(copy, 256, copy + 8, 32768, 0).Status == (UINT64)-2, "copy alignment");
    text("PAL probe: native PAL static/stacked vectors, buffers and copied entry passed\n");
    bool acpi = false, smbios = false, sal = false;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *t = &gST->ConfigurationTable[i];
        static const EFI_GUID acpi_guid = EFI_ACPI_20_TABLE_GUID;
        static const EFI_GUID smbios_guid = SMBIOS_TABLE_GUID;
        static const EFI_GUID sal_guid = SAL_SYSTEM_TABLE_GUID;
        if (CompareGuid(&t->VendorGuid, &acpi_guid)) {
            const uint8_t *root = t->VendorTable;
            check(table_sum(root, 20) == 0 && table_sum(root, 36) == 0, "RSDP checksum");
            const uint8_t *xsdt = (VOID *)(UINTN)table_get(root + 24, 8);
            check(table_sum(xsdt, (size_t)table_get(xsdt + 4, 4)) == 0, "XSDT checksum");
            acpi = true;
        }
        if (CompareGuid(&t->VendorGuid, &smbios_guid)) {
            check(table_sum(t->VendorTable, 31) == 0, "SMBIOS checksum");
            smbios = true;
        }
        if (CompareGuid(&t->VendorGuid, &sal_guid)) {
            const UINT8 *table = t->VendorTable;
            UINTN length = (UINTN)table_get(table + 4, 4);
            check(length >= 128 && length <= SAL_TABLE_CAPACITY && table_sum(table, length) == 0,
                  "SAL checksum");
            check(table[96] == 0 && table_get(table + 104, 8) == entry, "SAL PAL entry");
            static const UINT8 entry_size[] = {48, 32, 16, 32, 16, 16};
            const UINT8 *e = table + 96;
            for (UINTN n = table_get(table + 10, 2); n--;) {
                check(e[0] < sizeof(entry_size), "SAL entry type");
                if (e[0] == 3 && !sal_tr_itir) {
                    sal_tr_address = table_get(e + 8, 8);
                    sal_tr_itir = table_get(e + 16, 8);
                }
                e += entry_size[e[0]];
            }
            /* The IA-64 call expander reads a descriptor after optimization;
             * keep its stores observable to the ordinary C optimizer. */
            volatile UINT64 descriptor[2] = {table_get(table + 112, 8), table_get(table + 120, 8)};
            sal_descriptor[0] = descriptor[0];
            sal_descriptor[1] = descriptor[1];
            typedef struct sal_call_result (*sal_call)(UINT64, UINT64, UINT64, UINT64, UINT64,
                                                       UINT64, UINT64, UINT64);
            sal_call call = (sal_call)(VOID *)descriptor;
            struct sal_call_result result = call(0x01000012, 0, 0, 0, 0, 0, 0, 0);
            check(result.status == 0 && result.v0 == IA64VMM_PLATFORM_BASE_HZ, "SAL FREQ_BASE");
            check(call(0x01000001, 0, 0, (UINTN)buffer, 0, 0, 0, 0).status == -5, "SAL empty log");
            check(call(0x01000000, 2, descriptor[0], descriptor[1], 16, 0, 0, 0).status == 0,
                  "SAL OS_BOOT_RENDEZ vector");
            UINT32 cpus = 0;
            ap = ap_block(&cpus);
            check(ap && ap->rendez_entry == descriptor[0] && ap->rendez_gp == descriptor[1] &&
                      ap->count == cpus && ap->percpu_base && ap->percpu_stride == PERCPU_STRIDE,
                  "SAL OS_BOOT_RENDEZ and published AP areas in ap_wake_block");
            check(call(0x01000010, UINT64_C(1) << 28, 4, 1, 0, 0, 0, 0).status == -2,
                  "SAL PCI segment validation");
            check(call(0x01000008, 1, 0, 0, 0, 0, 0, 0).status == 0, "SAL cache flush");
            check(call(0x01000009, 0, 0, 0, 0, 0, 0, 0).status == 0, "SAL cache init");
            check(call(0x01000005, 1, 1, 0xe0, 100, 3, 0, 0).status == 0, "SAL MCA parameters");
            pci_address = 0;
            for (unsigned devfn = 0; devfn < 256; ++devfn) {
                result = call(0x01000010, (UINT64)devfn << 8, 4, 0, 0, 0, 0, 0);
                check(result.status == 0, "physical SAL PCI read");
                pci_id = result.v0;
                if ((pci_id & 0xffff) != 0xffff) {
                    pci_address = (UINT64)devfn << 8;
                    break;
                }
            }
            text((pci_id & 0xffff) == 0xffff ? "PAL probe: hosted empty PCI bus\n"
                                             : "PAL probe: real PCI vendor/device captured\n");
            sal = true;
        }
    }
    check(acpi && smbios, "configuration tables");
    text("PAL probe: published ACPI and SMBIOS tables passed\n");
    check(sal, "SAL configuration table");
    PAL_CALL_RETURN itr0 = AsmPalCall(entry, 261, 0, 0, (UINTN)buffer);
    check(sal_tr_itir && itr0.Status == 0 && itr0.r9 == 15 &&
              buffer[0] == ((sal_tr_address & ~UINT64_C(0xfff)) | 0x661) &&
              buffer[1] == sal_tr_itir &&
              (buffer[2] & vpn_mask) == ((sal_tr_address & vpn_mask) | 1) &&
              (buffer[3] >> 8) == ((rr0 >> 8) & 0xffffff) && (buffer[3] & 0xfc) == sal_tr_itir,
          "ITR(0) matches the SAL translation descriptor");
    text("PAL probe: SAL table, procedure ABI, vectors and empty logs passed\n");
    UINT8 unaligned[32];
    SetMem(unaligned, sizeof(unaligned), 0xa5);
    for (unsigned i = 0; i < 8; ++i)
        unaligned[1 + i] = (UINT8)(i + 1);
    check(probe_unaligned(unaligned + 1) == 0x0807060504030201ULL && unaligned[9] == 1 &&
              unaligned[10] == 2 && unaligned[11] == 3 && unaligned[12] == 4 &&
              unaligned[0] == 0xa5 && unaligned[13] == 0xa5,
          "unaligned IVT load/store/update");
    text("PAL probe: physical static-GR unaligned IVT accesses passed\n");
    UINT8 stacked[32];
    SetMem(stacked, sizeof(stacked), 0xa5);
    for (unsigned i = 0; i < 8; ++i)
        stacked[1 + i] = (UINT8)(i + 1);
    check(probe_unaligned_stacked(stacked + 1) == 0x0807060504030201ULL &&
              CompareMem(stacked + 1, stacked + 9, 8) == 0,
          "stacked GR unaligned and speculative NaT");
    UINT8 fp[80];
    SetMem(fp, sizeof(fp), 0xa5);
    const UINT64 patterns[] = {0x3ff0000000000000ULL, 0, 0x0123456789abcdefULL, 0};
    CopyMem(fp + 1, patterns, sizeof(patterns));
    UINT32 one = 0x3f800000;
    CopyMem(fp + 33, &one, 4);
    UINT64 ext[2] = {0x8000000000000000ULL, 0x3fff};
    CopyMem(fp + 41, ext, 10);
    probe_unaligned_fp(fp + 1);
    check(CompareMem(fp + 1, fp + 9, 8) == 0 && CompareMem(fp + 17, fp + 25, 8) == 0 &&
              CompareMem(fp + 33, fp + 37, 4) == 0 && CompareMem(fp + 41, fp + 57, 10) == 0,
          "FP unaligned representations and updates");
    text("PAL probe: stacked GR, speculative NaT and FP unaligned IVT accesses passed\n");
    virtual_calls(image, entry);
    return EFI_SUCCESS;
}
