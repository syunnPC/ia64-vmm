/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Service text output is a separate console so ConSplitter mirrors the same
 * UEFI text to the log and GraphicsConsole/GOP without assuming a UART. */
#include "../platform.h"
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Guid/EventGroup.h>
#include "../vga-text.h"
#include "../legacy-bios.h"
/* QEMU-derived VGA glyphs. */
#include "../../include/ui/vgafont.c"
static struct ia64vmm_handoff handoff;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
static EFI_EVENT exit_event;
static VOID flush_framebuffer(UINTN x, UINTN y)
{
    /* BLT is synchronous. An endpoint read completes posted framebuffer
     * writes as well as ordering the CPU stores; mf alone is insufficient. */
    MemoryFence();
    UINT32 completed = *(volatile UINT32 *)(UINTN)(handoff.fb_base + y * handoff.fb_pitch + x * 4);
    (void)completed;
}
static EFI_STATUS EFIAPI query(EFI_GRAPHICS_OUTPUT_PROTOCOL *self, UINT32 number, UINTN *size,
                               EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **result)
{
    (void)self;
    if (!size || !result)
        return EFI_INVALID_PARAMETER;
    if (number)
        return EFI_INVALID_PARAMETER;
    *result = AllocateCopyPool(sizeof(info), &info);
    if (!*result)
        return EFI_OUT_OF_RESOURCES;
    *size = sizeof(info);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *self, UINT32 number)
{
    (void)self;
    if (number)
        return EFI_UNSUPPORTED;
    /* UEFI GOP SetMode clears the visible screen even for the current mode.
     * Preserve pitch padding owned by the inherited framebuffer. */
    UINT8 *fb = (VOID *)(UINTN)handoff.fb_base;
    for (UINTN row = 0; row < info.VerticalResolution; ++row) {
        volatile UINT32 *pixels = (VOID *)(fb + row * handoff.fb_pitch);
        for (UINTN column = 0; column < info.HorizontalResolution; ++column)
            pixels[column] = 0;
    }
    flush_framebuffer(info.HorizontalResolution - 1, info.VerticalResolution - 1);
    mode.Mode = 0;
    return EFI_SUCCESS;
}
static UINT32 component(UINT8 value, UINT32 mask)
{
    UINT32 bits = mask, result = 0, source = 1;
    UINTN count = 0;
    while (bits) {
        ++count;
        bits &= bits - 1;
    }
    UINT32 scaled = count ? ((UINT64)value * ((1ULL << count) - 1) + 127) / 255 : 0;
    while (mask) {
        UINT32 bit = mask & (0u - mask);
        if (scaled & source)
            result |= bit;
        source <<= 1;
        mask &= mask - 1;
    }
    return result;
}
static UINT8 extract(UINT32 value, UINT32 mask)
{
    UINT32 result = 0, source = 1;
    UINTN count = 0;
    while (mask) {
        UINT32 bit = mask & (0u - mask);
        if (value & bit)
            result |= source;
        source <<= 1;
        mask &= mask - 1;
        ++count;
    }
    return count ? (UINT64)result * 255 / ((1ULL << count) - 1) : 0;
}
static UINT32 encode(EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixel)
{
    /* Native 8-bit channels need no scaling division for every GOP pixel. */
    if (info.PixelFormat == PixelBlueGreenRedReserved8BitPerColor)
        return pixel.Blue | ((UINT32)pixel.Green << 8) | ((UINT32)pixel.Red << 16);
    if (info.PixelFormat == PixelRedGreenBlueReserved8BitPerColor)
        return pixel.Red | ((UINT32)pixel.Green << 8) | ((UINT32)pixel.Blue << 16);
    return component(pixel.Red, info.PixelInformation.RedMask) |
           component(pixel.Green, info.PixelInformation.GreenMask) |
           component(pixel.Blue, info.PixelInformation.BlueMask);
}
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL decode(UINT32 value)
{
    if (info.PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL p = {(UINT8)value, (UINT8)(value >> 8), (UINT8)(value >> 16),
                                           0};
        return p;
    }
    if (info.PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL p = {(UINT8)(value >> 16), (UINT8)(value >> 8), (UINT8)value,
                                           0};
        return p;
    }
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL p = {extract(value, info.PixelInformation.BlueMask),
                                       extract(value, info.PixelInformation.GreenMask),
                                       extract(value, info.PixelInformation.RedMask), 0};
    return p;
}
static BOOLEAN rectangle(UINTN x, UINTN y, UINTN width, UINTN height)
{
    return x < info.HorizontalResolution && width <= info.HorizontalResolution - x &&
           y < info.VerticalResolution && height <= info.VerticalResolution - y;
}
static EFI_STATUS EFIAPI blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
                             EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buffer,
                             EFI_GRAPHICS_OUTPUT_BLT_OPERATION operation, UINTN sx, UINTN sy,
                             UINTN dx, UINTN dy, UINTN width, UINTN height, UINTN delta)
{
    (void)self;
    if (!width || !height || operation >= EfiGraphicsOutputBltOperationMax ||
        (operation != EfiBltVideoToVideo && !buffer))
        return EFI_INVALID_PARAMETER;
    if (operation != EfiBltVideoToBltBuffer && !rectangle(dx, dy, width, height))
        return EFI_INVALID_PARAMETER;
    if ((operation == EfiBltVideoToBltBuffer || operation == EfiBltVideoToVideo) &&
        !rectangle(sx, sy, width, height))
        return EFI_INVALID_PARAMETER;
    if (!delta)
        delta = width * sizeof(*buffer);
    if (operation == EfiBltBufferToVideo || operation == EfiBltVideoToBltBuffer) {
        UINTN bx = operation == EfiBltBufferToVideo ? sx : dx,
              by = operation == EfiBltBufferToVideo ? sy : dy;
        if (width > MAX_UINTN / 4 || bx > MAX_UINTN / 4 - width || delta < (bx + width) * 4 ||
            by > MAX_UINTN - height || by + height > MAX_UINTN / delta)
            return EFI_INVALID_PARAMETER;
    }
    UINT8 *fb = (VOID *)(UINTN)handoff.fb_base;
    UINT32 fill = operation == EfiBltVideoFill ? encode(*buffer) : 0;
    /* memmove ordering for overlapping video rectangles, including a row's
     * horizontal overlap. Pitch comes from the handoff, not width*4. */
    BOOLEAN reverse = operation == EfiBltVideoToVideo && (dy > sy || (dy == sy && dx > sx));
    for (UINTN row = 0; row < height; ++row) {
        UINTN y = reverse ? height - 1 - row : row;
        if (operation == EfiBltVideoToVideo) {
            volatile UINT32 *dest = (VOID *)(fb + (dy + y) * handoff.fb_pitch);
            volatile UINT32 *source = (VOID *)(fb + (sy + y) * handoff.fb_pitch);
            for (UINTN column = 0; column < width; ++column) {
                UINTN x = dx > sx ? width - 1 - column : column;
                dest[dx + x] = source[sx + x];
            }
            continue;
        }
        volatile UINT32 *pixels =
            (VOID *)(fb + ((operation == EfiBltVideoToBltBuffer ? sy : dy) + y) * handoff.fb_pitch);
        for (UINTN x = 0; x < width; ++x) {
            if (operation == EfiBltVideoFill)
                pixels[dx + x] = fill;
            else if (operation == EfiBltBufferToVideo)
                pixels[dx + x] =
                    encode(*(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)((UINT8 *)buffer + (sy + y) * delta +
                                                              (sx + x) * 4));
            else
                *(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)((UINT8 *)buffer + (dy + y) * delta +
                                                   (dx + x) * 4) = decode(pixels[sx + x]);
        }
    }
    if (operation != EfiBltVideoToBltBuffer)
        flush_framebuffer(dx + width - 1, dy + height - 1);
    return EFI_SUCCESS;
}
static EFI_GRAPHICS_OUTPUT_PROTOCOL gop = {query, set_mode, blt, &mode};
/* Shared transition context; "vga: " lines go to the service
 * console without formatting or allocation (safe in the exit callback). */
static struct legacy_vga_device vga;
static VOID vga_trace(const char *line)
{
    for (; *line; ++line)
        platform_service(IA64VMM_SVC_CONSOLE, (UINT8)*line);
    platform_service(IA64VMM_SVC_CONSOLE, '\n');
}
/* Legacy VGA: with display=vbios the VMM
 * POSTed the card and left text mode 3; this console then draws the UEFI
 * text into the VGA text buffer (as EDK2's VgaClassDxe does over its
 * VgaMiniPort), the firmware has no GOP, and nothing changes at
 * ExitBootServices. */
static struct legacy_text vga_text;
static BOOLEAN vga_console;
static VOID EFIAPI exit_boot(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    if (legacy_handoff_text_console(&handoff))
        return;
    if (!(handoff.flags & IA64VMM_HF_LEGACY_VGA) || (handoff.flags & IA64VMM_HF_UART_CONSOLE))
        return;
    if (!legacy_vga_can_leave_gop(&handoff)) {
        DebugPrint(DEBUG_INFO, "VGA GOP leave sequence needs controller-specific support\r\n");
        return;
    }
    /* Avoid boot-services calls or allocations here: the loader already
     * holds its final map key. The aperture's UC MMIO HOB remains intact. */
    if (handoff.fb_format != IA64VMM_FB_NONE)
        set_mode(&gop, 0);
    vga.h = &handoff;
    vga.trace = vga_trace;
    if (!legacy_vga_enter_text(&vga, vgafont16)) {
        DebugPrint(DEBUG_INFO, "VGA text mode not entered; GOP retained\r\n");
        return;
    }
    volatile UINT16 *cells = (VOID *)(UINTN)(handoff.vga_base + 0x18000);
    for (UINTN i = 0; i < 80 * 25; ++i)
        cells[i] = 0x0720;
    MemoryFence();
    (void)cells[80 * 25 - 1];
    DebugPrint(DEBUG_INFO, "Assigned VGA text mode ready\r\n");
}
static EFI_SIMPLE_TEXT_OUTPUT_MODE text_mode = {1, 0, 7, 0, 0, TRUE};
static VOID vga_follow(VOID)
{
    text_mode.CursorColumn = (INT32)vga_text.column;
    text_mode.CursorRow = (INT32)vga_text.row;
    legacy_text_cursor(&vga_text);
}
static EFI_STATUS EFIAPI text_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, BOOLEAN extended)
{
    (void)self;
    (void)extended;
    text_mode.CursorColumn = 0;
    text_mode.CursorRow = 0;
    if (vga_console) {
        vga_text.attribute = (UINT8)text_mode.Attribute;
        legacy_text_clear(&vga_text);
        vga_follow();
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, CHAR16 *string)
{
    (void)self;
    if (!string)
        return EFI_INVALID_PARAMETER;
    for (; *string; ++string) {
        UINT8 ch = *string < 128 ? *string : '?';
        if (handoff.flags & IA64VMM_HF_UART_CONSOLE)
            *(volatile UINT8 *)(UINTN)handoff.uart_base = ch;
        else
            platform_service(IA64VMM_SVC_CONSOLE, ch);
        if (vga_console)
            legacy_text_put(&vga_text, *string);
        else if (*string == '\r')
            text_mode.CursorColumn = 0;
        else if (*string == '\n') {
            if (text_mode.CursorRow < 24)
                ++text_mode.CursorRow;
        } else if (++text_mode.CursorColumn == 80) {
            text_mode.CursorColumn = 0;
            if (text_mode.CursorRow < 24)
                ++text_mode.CursorRow;
        }
    }
    if (vga_console)
        vga_follow();
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI test_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, CHAR16 *string)
{
    (void)self;
    if (!string)
        return EFI_INVALID_PARAMETER;
    for (; *string; ++string)
        if (*string >= 128)
            return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI text_query(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, UINTN number,
                                    UINTN *columns, UINTN *rows)
{
    (void)self;
    if (!columns || !rows)
        return EFI_INVALID_PARAMETER;
    if (number)
        return EFI_UNSUPPORTED;
    *columns = 80;
    *rows = 25;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI text_set(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, UINTN number)
{
    if (number)
        return EFI_UNSUPPORTED;
    return text_reset(self, FALSE);
}
static EFI_STATUS EFIAPI attribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, UINTN value)
{
    (void)self;
    if (value > 127)
        return EFI_UNSUPPORTED;
    text_mode.Attribute = (INT32)value;
    vga_text.attribute = (UINT8)value; /* EFI_TEXT_ATTR is the VGA attribute byte */
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI clear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self)
{
    return text_reset(self, FALSE);
}
static EFI_STATUS EFIAPI position(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, UINTN column, UINTN row)
{
    (void)self;
    if (column >= 80 || row >= 25)
        return EFI_UNSUPPORTED;
    text_mode.CursorColumn = (INT32)column;
    text_mode.CursorRow = (INT32)row;
    if (vga_console) {
        vga_text.column = (unsigned)column;
        vga_text.row = (unsigned)row;
        vga_follow();
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, BOOLEAN visible)
{
    (void)self;
    text_mode.CursorVisible = visible;
    if (vga_console) {
        vga_text.cursor_visible = visible;
        legacy_text_cursor(&vga_text);
    }
    return EFI_SUCCESS;
}
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL text = {text_reset, output,    test_string, text_query,
                                               text_set,   attribute, clear,       position,
                                               cursor,     &text_mode};
static EFI_STATUS EFIAPI input_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self, BOOLEAN extended)
{
    (void)self;
    (void)extended;
    return EFI_SUCCESS;
}
static BOOLEAN serial_key_ready(VOID)
{
    return (handoff.flags & IA64VMM_HF_UART_CONSOLE) &&
           (*(volatile UINT8 *)(UINTN)(handoff.uart_base + 5) & 1);
}
static EFI_STATUS EFIAPI input_read(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self, EFI_INPUT_KEY *key)
{
    (void)self;
    if (!key)
        return EFI_INVALID_PARAMETER;
    if (!serial_key_ready())
        return EFI_NOT_READY;
    UINT8 ch = *(volatile UINT8 *)(UINTN)handoff.uart_base;
    key->ScanCode = 0;
    key->UnicodeChar = ch == 0x7f ? '\b' : ch;
    return EFI_SUCCESS;
}
static VOID EFIAPI input_wait(EFI_EVENT event, VOID *context)
{
    (void)context;
    if (serial_key_ready())
        gBS->SignalEvent(event);
}
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL input = {input_reset, input_read, NULL};
static EFI_STATUS EFIAPI input_reset_ex(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *self, BOOLEAN extended)
{
    (void)self;
    return input_reset(&input, extended);
}
static EFI_STATUS EFIAPI input_read_ex(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *self, EFI_KEY_DATA *key)
{
    (void)self;
    if (!key)
        return EFI_INVALID_PARAMETER;
    ZeroMem(key, sizeof(*key));
    return input_read(&input, &key->Key);
}
static EFI_STATUS EFIAPI input_state(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *self,
                                     EFI_KEY_TOGGLE_STATE *state)
{
    (void)self;
    return state ? EFI_UNSUPPORTED : EFI_INVALID_PARAMETER;
}
struct key_notification {
    struct key_notification *next;
    EFI_KEY_DATA key;
    EFI_KEY_NOTIFY_FUNCTION function;
};
static struct key_notification *key_notifications;
static EFI_STATUS EFIAPI input_register(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *self, EFI_KEY_DATA *key,
                                        EFI_KEY_NOTIFY_FUNCTION function, VOID **handle)
{
    (void)self;
    if (!key || !function || !handle)
        return EFI_INVALID_PARAMETER;
    EFI_TPL tpl = gBS->RaiseTPL(TPL_NOTIFY);
    for (struct key_notification *n = key_notifications; n; n = n->next) {
        if (n->function == function && n->key.Key.ScanCode == key->Key.ScanCode &&
            n->key.Key.UnicodeChar == key->Key.UnicodeChar &&
            n->key.KeyState.KeyShiftState == key->KeyState.KeyShiftState &&
            n->key.KeyState.KeyToggleState == key->KeyState.KeyToggleState) {
            *handle = n;
            gBS->RestoreTPL(tpl);
            return EFI_SUCCESS;
        }
    }
    struct key_notification *n = AllocatePool(sizeof(*n));
    if (!n) {
        gBS->RestoreTPL(tpl);
        return EFI_OUT_OF_RESOURCES;
    }
    n->key = *key;
    n->function = function;
    n->next = key_notifications;
    key_notifications = n;
    *handle = n;
    gBS->RestoreTPL(tpl);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI input_unregister(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *self, VOID *handle)
{
    (void)self;
    EFI_TPL tpl = gBS->RaiseTPL(TPL_NOTIFY);
    struct key_notification **link = &key_notifications;
    while (*link && *link != handle)
        link = &(*link)->next;
    if (!*link) {
        gBS->RestoreTPL(tpl);
        return EFI_INVALID_PARAMETER;
    }
    struct key_notification *n = *link;
    *link = n->next;
    FreePool(n);
    gBS->RestoreTPL(tpl);
    return EFI_SUCCESS;
}
/* UDK2018 ConSplitter returns the OpenProtocol(InputEx) status from Start;
 * expose both protocols for the diagnostic or optional serial input. */
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL input_ex = {
    input_reset_ex, input_read_ex, NULL, input_state, input_register, input_unregister};
struct console_path {
    VENDOR_DEVICE_PATH vendor;
    EFI_DEVICE_PATH_PROTOCOL end;
};
static struct console_path paths[3];
static struct {
    MEMMAP_DEVICE_PATH memory;
    UART_DEVICE_PATH uart;
    EFI_DEVICE_PATH_PROTOCOL end;
} __attribute__((packed)) serial_path;
EFI_STATUS EFIAPI VmmConsoleEntry(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    (void)image;
    (void)system;
    CopyMem(&handoff, platform_handoff_hob(), sizeof(handoff));
    /* A serial console boot keeps the UART as the console (debug serial policy). */
    vga_console =
        legacy_handoff_text_console(&handoff) && !(handoff.flags & IA64VMM_HF_UART_CONSOLE);
    if (vga_console) {
        vga_text.cells = handoff.vga_base + 0x18000;
        vga_text.io_base = handoff.legacy_io_base;
        vga_text.attribute = (UINT8)text_mode.Attribute;
        vga_text.cursor_visible = TRUE;
        legacy_text_clear(&vga_text);
        DebugPrint(DEBUG_INFO, "VGA text console 80x25 (VBIOS POST by the VMM), no GOP\n");
    }
    static const EFI_GUID exit_guid = EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
    EFI_STATUS status =
        gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, exit_boot, NULL, &exit_guid, &exit_event);
    if (EFI_ERROR(status))
        return status;
    static const EFI_GUID guid = {
        0x3d0866a4, 0x2517, 0x4aa3, {0xb3, 0x27, 0x47, 0x32, 0x43, 0x4f, 0x4e, 0x00}};
    for (UINTN i = 0; i < 3; ++i) {
        paths[i].vendor.Header.Type = HARDWARE_DEVICE_PATH;
        paths[i].vendor.Header.SubType = HW_VENDOR_DP;
        paths[i].vendor.Header.Length[0] = sizeof(VENDOR_DEVICE_PATH);
        paths[i].vendor.Guid = guid;
        paths[i].vendor.Guid.Data4[7] = i;
        paths[i].end.Type = END_DEVICE_PATH_TYPE;
        paths[i].end.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
        paths[i].end.Length[0] = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    }
    EFI_HANDLE handles[3] = {NULL, NULL, NULL};
    serial_path.memory.Header.Type = HARDWARE_DEVICE_PATH;
    serial_path.memory.Header.SubType = HW_MEMMAP_DP;
    serial_path.memory.Header.Length[0] = sizeof(MEMMAP_DEVICE_PATH);
    serial_path.memory.MemoryType = EfiMemoryMappedIO;
    serial_path.memory.StartingAddress = handoff.uart_base;
    serial_path.memory.EndingAddress = handoff.uart_base + 7;
    serial_path.uart.Header.Type = MESSAGING_DEVICE_PATH;
    serial_path.uart.Header.SubType = MSG_UART_DP;
    serial_path.uart.Header.Length[0] = sizeof(UART_DEVICE_PATH);
    serial_path.uart.BaudRate = 115200;
    serial_path.uart.DataBits = 8;
    serial_path.uart.Parity = 1;
    serial_path.uart.StopBits = 1;
    serial_path.end = paths[0].end;
    VOID *out_path =
        handoff.flags & IA64VMM_HF_UART_CONSOLE ? (VOID *)&serial_path : (VOID *)&paths[0];
    VOID *in_path =
        handoff.flags & IA64VMM_HF_UART_CONSOLE ? (VOID *)&serial_path : (VOID *)&paths[1];
    status =
        gBS->InstallMultipleProtocolInterfaces(&handles[0], &gEfiDevicePathProtocolGuid, out_path,
                                               &gEfiSimpleTextOutProtocolGuid, &text, NULL);
    if (EFI_ERROR(status))
        return status;
    status = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_NOTIFY, input_wait, NULL, &input.WaitForKey);
    if (EFI_ERROR(status))
        return status;
    status =
        gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_NOTIFY, input_wait, NULL, &input_ex.WaitForKeyEx);
    if (EFI_ERROR(status))
        return status;
    static const EFI_GUID input_ex_guid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
    if (handoff.flags & IA64VMM_HF_UART_CONSOLE) {
        /* One full-duplex UART handle; duplicate physical device paths cause
         * ambiguous LocateDevicePath matches in the PI core. */
        handles[1] = handles[0];
        status = gBS->InstallMultipleProtocolInterfaces(&handles[1], &gEfiSimpleTextInProtocolGuid,
                                                        &input, (EFI_GUID *)&input_ex_guid,
                                                        &input_ex, NULL);
    } else
        status = gBS->InstallMultipleProtocolInterfaces(
            &handles[1], &gEfiDevicePathProtocolGuid, in_path, &gEfiSimpleTextInProtocolGuid,
            &input, (EFI_GUID *)&input_ex_guid, &input_ex, NULL);
    if (EFI_ERROR(status))
        return status;
    if (handoff.fb_format == IA64VMM_FB_NONE)
        return EFI_SUCCESS;
    if (!handoff.fb_width || !handoff.fb_height || handoff.fb_pitch % 4 ||
        handoff.fb_width > handoff.fb_pitch / 4 ||
        (UINT64)handoff.fb_height * handoff.fb_pitch > handoff.fb_size ||
        handoff.fb_size > MAX_UINT64 - handoff.fb_base || handoff.fb_format > IA64VMM_FB_BITMASK)
        return EFI_UNSUPPORTED;
    if (handoff.fb_format == IA64VMM_FB_BITMASK) {
        UINT32 red = handoff.fb_red_mask, green = handoff.fb_green_mask,
               blue = handoff.fb_blue_mask;
        if (!red || !green || !blue || (red & green) || (red & blue) || (green & blue) ||
            ((red | green | blue) & handoff.fb_reserved_mask))
            return EFI_UNSUPPORTED;
    }
    info.HorizontalResolution = handoff.fb_width;
    info.VerticalResolution = handoff.fb_height;
    info.PixelsPerScanLine = handoff.fb_pitch / 4;
    info.PixelFormat =
        handoff.fb_format == IA64VMM_FB_RGBX8888   ? PixelRedGreenBlueReserved8BitPerColor
        : handoff.fb_format == IA64VMM_FB_BGRX8888 ? PixelBlueGreenRedReserved8BitPerColor
                                                   : PixelBitMask;
    info.PixelInformation.RedMask = handoff.fb_red_mask;
    info.PixelInformation.GreenMask = handoff.fb_green_mask;
    info.PixelInformation.BlueMask = handoff.fb_blue_mask;
    info.PixelInformation.ReservedMask = handoff.fb_reserved_mask;
    mode.MaxMode = 1;
    mode.Info = &info;
    mode.SizeOfInfo = sizeof(info);
    mode.FrameBufferBase = handoff.fb_base;
    mode.FrameBufferSize = handoff.fb_size;
    status =
        gBS->InstallMultipleProtocolInterfaces(&handles[2], &gEfiDevicePathProtocolGuid, &paths[2],
                                               &gEfiGraphicsOutputProtocolGuid, &gop, NULL);
    DebugPrint(DEBUG_INFO, "GOP %ux%u pitch=%u: %r\n", handoff.fb_width, handoff.fb_height,
               handoff.fb_pitch, status);
    if (!EFI_ERROR(status))
        platform_service(IA64VMM_SVC_GOP_OWNED, 1);
    /* VMM option vga_probe=N: enter VGA text, hold a test page,
     * restore the saved GOP state before any console draws into it. */
    if (handoff.vga_probe_sec) {
        vga.h = &handoff;
        vga.trace = vga_trace;
        if (!legacy_vga_probe(&vga, vgafont16))
            DebugPrint(DEBUG_ERROR, "VGA probe could not restore the GOP display\n");
    }
    return status;
}
