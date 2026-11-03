/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
/* vga-text.h accessors for a host build; the exit callback is not run here. */
#define LEGACY_VGA_HOOKS
#include <stdint.h>
static inline uint8_t legacy_vga_rd8(uint64_t a)
{
    return *(volatile uint8_t *)(uintptr_t)a;
}
static inline uint16_t legacy_vga_rd16(uint64_t a)
{
    return *(volatile uint16_t *)(uintptr_t)a;
}
static inline uint32_t legacy_vga_rd32(uint64_t a)
{
    return *(volatile uint32_t *)(uintptr_t)a;
}
static inline void legacy_vga_wr8(uint64_t a, uint8_t v)
{
    *(volatile uint8_t *)(uintptr_t)a = v;
}
static inline void legacy_vga_wr16(uint64_t a, uint16_t v)
{
    *(volatile uint16_t *)(uintptr_t)a = v;
}
static inline void legacy_vga_wr32(uint64_t a, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)a = v;
}
static inline void legacy_vga_wr64(uint64_t a, uint64_t v)
{
    *(volatile uint64_t *)(uintptr_t)a = v;
}
static uint64_t host_itc;
static inline uint64_t legacy_vga_ticks(void)
{
    return host_itc += 1000;
}
#include "firmware/new/dxe/console.c"
#include "firmware/console-tables.h"

static union {
    UINT64 alignment;
    UINT8 bytes[2048];
} pool;
static UINTN used;
VOID *EFIAPI AllocatePool(UINTN size)
{
    size = (size + 7) & ~(UINTN)7;
    assert(size <= sizeof(pool.bytes) - used);
    VOID *result = pool.bytes + used;
    used += size;
    return result;
}
VOID *EFIAPI CopyMem(VOID *dest, const VOID *source, UINTN size)
{
    return memmove(dest, source, size);
}
VOID *EFIAPI ZeroMem(VOID *dest, UINTN size)
{
    return memset(dest, 0, size);
}
VOID EFIAPI MemoryFence(VOID)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
VOID *EFIAPI AllocateCopyPool(UINTN size, const VOID *source)
{
    return CopyMem(AllocatePool(size), source, size);
}
VOID EFIAPI FreePool(VOID *buffer)
{
    (void)buffer;
}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL tpl)
{
    (void)tpl;
    return TPL_APPLICATION;
}
static VOID EFIAPI restore_tpl(EFI_TPL tpl)
{
    (void)tpl;
}
static EFI_BOOT_SERVICES services = {.RaiseTPL = raise_tpl, .RestoreTPL = restore_tpl};
EFI_BOOT_SERVICES *gBS = &services;
static EFI_STATUS EFIAPI key_notify(EFI_KEY_DATA *key)
{
    (void)key;
    return EFI_SUCCESS;
}

int main(void)
{
    for (unsigned serial = 0; serial < 3; ++serial)
        for (unsigned vga = 0; vga < 2; ++vga) {
            struct ia64vmm_handoff h = {0};
            struct console_tables tables;
            h.flags = (serial ? IA64VMM_HF_UART_PRESENT : 0) |
                      (serial == 2 ? IA64VMM_HF_UART_CONSOLE : 0) |
                      (vga ? IA64VMM_HF_LEGACY_VGA : 0);
            h.uart_base = IA64VMM_UART_BASE;
            h.legacy_io_base = IA64VMM_LEGACY_IO_BASE;
            h.vga_base = IA64VMM_VGA_MEMORY_BASE;
            h.vga_size = IA64VMM_VGA_MEMORY_SIZE;
            console_tables_build(&tables, &h);
            assert(!!tables.uart_size == !!serial && !!tables.spcr_size == (serial == 2));
            assert(!!tables.hcdp_size == !!(vga || serial == 2));
            const UINT8 *bytes[] = {tables.uart, tables.spcr, tables.hcdp};
            UINTN sizes[] = {tables.uart_size, tables.spcr_size, tables.hcdp_size};
            for (unsigned i = 0; i < 3; ++i) {
                unsigned sum = 0;
                for (UINTN j = 0; j < sizes[i]; ++j)
                    sum += bytes[i][j];
                assert(!(sum & 255));
            }
            if (tables.hcdp_size) {
                assert(tables.hcdp[36] == (serial == 2));
                if (vga) {
                    UINTN at = 40 + (serial == 2 ? 48 : 0);
                    assert(tables.hcdp[at] == 10 && tables.hcdp[at + 1] == (serial != 2));
                    assert(tables.hcdp[at + 39] == 2 && tables.hcdp[at + 40] == 2);
                }
            }
        }
    UINT32 pixels[3][5];
    memset(pixels, 0xa5, sizeof(pixels));
    handoff.fb_base = (UINTN)pixels;
    handoff.fb_pitch = sizeof(pixels[0]);
    info.HorizontalResolution = 3;
    info.VerticalResolution = 3;
    info.PixelsPerScanLine = 5;
    info.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL red = {0, 0, 255, 0}, output_pixels[6];
    assert(blt(&gop, &red, EfiBltVideoFill, 0, 0, 0, 0, 3, 3, 0) == EFI_SUCCESS);
    assert(pixels[0][0] == 0xff0000 && pixels[2][2] == 0xff0000 && pixels[1][3] == 0xa5a5a5a5);
    assert(blt(&gop, output_pixels, EfiBltVideoToBltBuffer, 1, 1, 0, 0, 2, 2, 12) == EFI_SUCCESS);
    assert(output_pixels[0].Red == 255 && output_pixels[4].Red == 255);
    assert(blt(&gop, &red, EfiBltVideoFill, 0, 0, 2, 0, 2, 1, 0) == EFI_INVALID_PARAMETER);
    assert(blt(&gop, output_pixels, EfiBltBufferToVideo, 1, 0, 0, 0, 2, 1, 4) ==
           EFI_INVALID_PARAMETER);
    assert(blt(&gop, NULL, EfiBltVideoFill, 0, 0, 0, 0, 1, 1, 0) == EFI_INVALID_PARAMETER);
    for (UINTN row = 0; row < 3; ++row)
        for (UINTN column = 0; column < 3; ++column)
            pixels[row][column] = 1 + row * 3 + column;
    assert(blt(&gop, NULL, EfiBltVideoToVideo, 0, 0, 1, 0, 2, 1, 0) == EFI_SUCCESS);
    assert(pixels[0][0] == 1 && pixels[0][1] == 1 && pixels[0][2] == 2);
    assert(blt(&gop, NULL, EfiBltVideoToVideo, 0, 0, 0, 1, 3, 2, 0) == EFI_SUCCESS);
    assert(pixels[2][0] == 4 && pixels[1][2] == 2);
    assert(set_mode(&gop, 1) == EFI_UNSUPPORTED && pixels[2][0] == 4);
    assert(set_mode(&gop, 0) == EFI_SUCCESS && pixels[2][0] == 0 && pixels[2][3] == 0xa5a5a5a5);
    UINTN size;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *copy;
    assert(query(&gop, 0, &size, &copy) == EFI_SUCCESS && copy != &info && size == sizeof(info));
    assert(copy->PixelsPerScanLine == 5 && query(&gop, 1, &size, &copy) == EFI_INVALID_PARAMETER);
    info.PixelFormat = PixelRedGreenBlueReserved8BitPerColor;
    assert(encode(red) == 255 && decode(255).Red == 255);
    info.PixelFormat = PixelBitMask;
    info.PixelInformation.RedMask = 0xf800;
    info.PixelInformation.GreenMask = 0x7e0;
    info.PixelInformation.BlueMask = 0x1f;
    assert(encode(red) == 0xf800 && decode(0xf800).Red == 255 && decode(0x7e0).Green == 255);
    EFI_INPUT_KEY key;
    EFI_KEY_DATA ex_key = {0};
    assert(input_reset(&input, FALSE) == EFI_SUCCESS && input_read(&input, &key) == EFI_NOT_READY);
    assert(input_read(&input, NULL) == EFI_INVALID_PARAMETER &&
           input_read_ex(&input_ex, &ex_key) == EFI_NOT_READY);
    assert(input_read_ex(&input_ex, NULL) == EFI_INVALID_PARAMETER &&
           input_state(&input_ex, NULL) == EFI_INVALID_PARAMETER);
    VOID *first, *second;
    assert(input_register(&input_ex, NULL, key_notify, &first) == EFI_INVALID_PARAMETER);
    assert(input_register(&input_ex, &ex_key, key_notify, &first) == EFI_SUCCESS);
    assert(input_register(&input_ex, &ex_key, key_notify, &second) == EFI_SUCCESS &&
           first == second);
    assert(input_unregister(&input_ex, first) == EFI_SUCCESS);
    assert(input_unregister(&input_ex, first) == EFI_INVALID_PARAMETER);
    puts("console: color packing, pitch, overlapping BLTs, mode clearing and empty InputEx passed");
    return 0;
}
