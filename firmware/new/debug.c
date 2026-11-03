/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DebugLib sink: PI diagnostics go through the existing platform ABI. */
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <ia64vmm/platform.h>

static void console(const CHAR8 *text)
{
    volatile UINT64 *reg = (volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE);
    while (*text)
        *reg = (UINT8)*text++;
}

VOID EFIAPI DebugPrint(UINTN level, const CHAR8 *format, ...)
{
    CHAR8 buffer[512];
    VA_LIST args;
    /* Pool/page tracing interleaves application OutputString fragments and
     * overwhelms the shared console. Keep dispatch, protocol and error logs. */
    if (!(level & (DEBUG_ERROR | DEBUG_WARN)) && (level & (DEBUG_POOL | DEBUG_PAGE)))
        return;
    VA_START(args, format);
    AsciiVSPrint(buffer, sizeof(buffer), format, args);
    VA_END(args);
    console(buffer);
}

VOID EFIAPI DebugAssert(const CHAR8 *file, UINTN line, const CHAR8 *description)
{
    DebugPrint(DEBUG_ERROR, "ASSERT %a(%u): %a\n", file, line, description);
    *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_PANIC) = 0x47314153;
    for (;;)
        __asm__ volatile("hint @pause");
}

VOID *EFIAPI DebugClearMemory(VOID *buffer, UINTN length)
{
    volatile UINT8 *p = buffer;
    while (length--)
        *p++ = 0xaf;
    return buffer;
}
/* The release build (MDEPKG_NDEBUG) behaves as PcdDebugPropertyMask 0: no
 * ASSERT, DEBUG() or DEBUG_CODE. Direct DebugPrint calls remain. */
#ifdef MDEPKG_NDEBUG
#define DEBUG_MACROS FALSE
#else
#define DEBUG_MACROS TRUE
#endif
BOOLEAN EFIAPI DebugAssertEnabled(VOID)
{
    return DEBUG_MACROS;
}
BOOLEAN EFIAPI DebugPrintEnabled(VOID)
{
    return DEBUG_MACROS;
}
BOOLEAN EFIAPI DebugCodeEnabled(VOID)
{
    return DEBUG_MACROS;
}
BOOLEAN EFIAPI DebugClearMemoryEnabled(VOID)
{
    return FALSE;
}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN level)
{
    return (level & (DEBUG_ERROR | DEBUG_WARN)) || !(level & (DEBUG_POOL | DEBUG_PAGE));
}
