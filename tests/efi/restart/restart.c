/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * restart.efi: one in-VMM restart per reset source
 * within one VMM run. A nonvolatile variable carries the phase; each boot
 * first reads back the data the previous boot wrote, then requests the
 * next reset:
 *   phase 0  ResetSystem(EfiResetCold)        UEFI 2.11 §8.5.1.1
 *   phase 1  ResetSystem(EfiResetWarm)
 *   phase 2  ResetSystem(EfiResetPlatformSpecific, EFI_ABORTED), with
 *            ResetData a string and a GUID; a nonzero status still resets
 *   phase 3  FADT reset register: I/O 200Ch := 1 (ACPI 2.0 §4.7.3.6)
 *   phase 4  I/O CF9h := 06h
 *   phase 5  i8042 command FEh at 64h (pulse output port bit 0, the reset
 *            line), when the guest owns a keyboard controller
 *   phase 6  ResetSystem(EfiResetShutdown, EFI_SUCCESS)
 * A reset that does not happen leaves the boot spinning until timeout_sec.
 */
#include <efi.h>

#define PHASES 6u
#define RESET_PLATFORM_SPECIFIC ((EFI_RESET_TYPE)3) /* EfiResetPlatformSpecific; not in GNU-EFI */
#define PSR_DT (1ull << 17)
#define PHYS_UC (1ull << 63)                 /* physical-mode UC, SDM Vol.2 §4.4.2 */
#define LEGACY_IO_BASE 0x0003fffffc000000ull /* include/ia64vmm/platform.h */

static EFI_SYSTEM_TABLE *gST;
static EFI_GUID guid = {
    0x6a7f3c51, 0x2d19, 0x4c8e, {0x9b, 0x51, 0x3f, 0x0e, 0x72, 0xa4, 0xd6, 0x18}};
static const UINT32 attributes =
    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;

static void put(const char *s)
{
    CHAR16 buf[96];
    UINTN n = 0;
    while (*s && n < 95)
        buf[n++] = (CHAR16)(unsigned char)*s++;
    buf[n] = 0;
    gST->ConOut->OutputString(gST->ConOut, buf);
}
static void put_u(UINT64 v)
{
    char buf[21];
    int i = 20;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    put(buf + i);
}
static void data_for(UINT32 phase, UINT8 out[16])
{
    for (UINT32 i = 0; i < 16; ++i)
        out[i] = (UINT8)(phase * 37u + i * 11u + 5u);
}
static EFI_STATUS finish(EFI_STATUS status, const char *why)
{
    put("RESTART FAIL ");
    put(why);
    put("\r\n");
    gST->RuntimeServices->ResetSystem(EfiResetShutdown, status, 0, NULL);
    return status;
}
static UINT64 port_address(UINT32 port)
{
    UINT64 psr;
    __asm__ volatile("mov %0=psr;;" : "=r"(psr)::"memory");
    return (LEGACY_IO_BASE + (((UINT64)port >> 2) << 12 | (port & 0xfff))) |
           ((psr & PSR_DT) ? 0 : PHYS_UC);
}
static void out8(UINT32 port, UINT8 value)
{
    *(volatile UINT8 *)(UINTN)port_address(port) = value;
    __asm__ volatile("mf.a;; mf;;" ::: "memory");
}
static UINT8 in8(UINT32 port)
{
    UINT8 value = *(volatile UINT8 *)(UINTN)port_address(port);
    __asm__ volatile("mf.a;;" ::: "memory");
    return value;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    gST = st;
    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
    UINT32 phase = 0, attr = 0;
    UINTN size = sizeof(phase);
    EFI_STATUS status =
        st->RuntimeServices->GetVariable(L"RestartPhase", &guid, &attr, &size, &phase);
    if (status == EFI_NOT_FOUND)
        phase = 0;
    else if (EFI_ERROR(status) || size != sizeof(phase) || attr != attributes || phase > PHASES)
        return finish(EFI_VOLUME_CORRUPTED, "phase variable");
    put("RESTART boot phase=");
    put_u(phase);
    put("\r\n");
    if (phase) {
        UINT8 data[16], expected[16];
        size = sizeof(data);
        status = st->RuntimeServices->GetVariable(L"RestartData", &guid, &attr, &size, data);
        data_for(phase, expected);
        if (EFI_ERROR(status) || size != sizeof(data) || attr != attributes)
            return finish(EFI_NOT_FOUND, "data variable missing");
        for (UINTN i = 0; i < sizeof(data); ++i)
            if (data[i] != expected[i])
                return finish(EFI_CRC_ERROR, "data variable differs");
        put("RESTART read back phase=");
        put_u(phase);
        put("\r\n");
    }
    if (phase == PHASES) {
        put("RESTART PASS boots=");
        put_u(phase + 1);
        put("\r\n");
        st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }
    UINT32 next = phase + 1;
    UINT8 data[16];
    data_for(next, data);
    if (EFI_ERROR(st->RuntimeServices->SetVariable(L"RestartData", &guid, attributes, sizeof(data),
                                                   data)) ||
        EFI_ERROR(st->RuntimeServices->SetVariable(L"RestartPhase", &guid, attributes, sizeof(next),
                                                   &next)))
        return finish(EFI_DEVICE_ERROR, "SetVariable");
    switch (phase) {
    case 0:
        put("RESTART request ResetSystem cold\r\n");
        st->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        break;
    case 1:
        put("RESTART request ResetSystem warm\r\n");
        st->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
        break;
    case 2: {
        /* UEFI 2.11 §8.5.1.1: a null-terminated string, then a GUID. */
        static const CHAR16 reason[] = L"restart.efi";
        UINT8 data[sizeof(reason) + sizeof(EFI_GUID)];
        for (UINTN i = 0; i < sizeof(reason); ++i)
            data[i] = ((const UINT8 *)reason)[i];
        for (UINTN i = 0; i < sizeof(EFI_GUID); ++i)
            data[sizeof(reason) + i] = ((const UINT8 *)&guid)[i];
        put("RESTART request ResetSystem platform-specific, EFI_ABORTED\r\n");
        st->RuntimeServices->ResetSystem(RESET_PLATFORM_SPECIFIC, EFI_ABORTED, sizeof(data),
                                         (CHAR16 *)data);
        break;
    }
    case 3:
        put("RESTART request ACPI reset register\r\n");
        out8(0x200c, 1);
        break;
    case 4:
        put("RESTART request CF9h\r\n");
        out8(0xcf9, 0x06);
        break;
    default:
        if (in8(0x64) == 0xff) {
            /* No guest-owned keyboard controller: the phase has no source. */
            put("RESTART i8042 absent, ResetSystem warm instead\r\n");
            st->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
        }
        put("RESTART request i8042 FEh\r\n");
        out8(0x64, 0xfe);
        break;
    }
    for (;;)
        __asm__ volatile("hint @pause" ::: "memory");
}

/* crt0-efi-ia64.S calls _entry after self-relocation. */
EFI_STATUS EFIAPI _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    return efi_main(image, st);
}
