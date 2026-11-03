/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Legacy VGA register-model test:
 * firmware/new/legacy-handoff.h, the check of the handoff's legacy tail
 * both firmwares run (every rule, each with the variant that breaks it);
 * firmware/new/legacy-bios.h, the VGA text console core; and the new
 * firmware's VmmConsole (firmware/new/dxe/console.c) under display=vbios,
 * entered through its driver entry point. A model of the 80x25 text buffer at
 * B8000h and of the CRTC behind the sparse legacy I/O window counts every
 * device access: scrolling must not read device memory, the console must
 * install no GOP, and ExitBootServices must not touch the device.
 * tests/firmware/legacy_bios_mutants.py shows broken firmware variants fail. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#define LEGACY_VGA_HOOKS
#include <stdint.h>
static uint8_t legacy_vga_rd8(uint64_t a);
static uint16_t legacy_vga_rd16(uint64_t a);
static uint32_t legacy_vga_rd32(uint64_t a);
static void legacy_vga_wr8(uint64_t a, uint8_t v);
static void legacy_vga_wr16(uint64_t a, uint16_t v);
static void legacy_vga_wr32(uint64_t a, uint32_t v);
static void legacy_vga_wr64(uint64_t a, uint64_t v);
static uint64_t legacy_vga_ticks(void);
/* firmware/new/platform.h for a host: the handoff from the test, the VMM
 * service page (0x3fffff8000000, beyond a 47-bit host address space)
 * recorded instead of written. */
#define VMM_PLATFORM_H
#include <PiDxe.h>
#include <Library/HobLib.h>
#include <Library/DebugLib.h>
#include <ia64vmm/platform.h>
static struct ia64vmm_handoff test_handoff;
static char service_console[4096];
static unsigned service_console_used, service_other;
static inline const struct ia64vmm_handoff *platform_handoff_hob(VOID)
{
    return &test_handoff;
}
static inline VOID platform_service(UINTN offset, UINT64 value)
{
    if (offset == IA64VMM_SVC_CONSOLE && service_console_used + 1 < sizeof(service_console))
        service_console[service_console_used++] = (char)value;
    else
        ++service_other;
}
#include "firmware/new/dxe/console.c"

#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #e);                                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* --- The device model ----------------------------------------------------- */
#define CELLS 0xb8000ull
static uint16_t screen[25 * 80];
static uint8_t crtc[0x20], crtc_index;
static struct {
    unsigned cell_reads, cell_writes, crtc_reads, crtc_writes, stray;
} seen;
static uint64_t ticks;
static int port_at(uint64_t a)
{
    for (unsigned p = 0x3b0; p < 0x3e0; ++p)
        if (legacy_vga_port_address(IA64VMM_LEGACY_IO_BASE, p) == a)
            return (int)p;
    return -1;
}
static int cell_at(uint64_t a)
{
    return a >= CELLS && a < CELLS + sizeof(screen) && !(a & 1) ? (int)((a - CELLS) / 2) : -1;
}
static uint8_t legacy_vga_rd8(uint64_t a)
{
    int port = port_at(a);
    if (port == 0x3d4)
        return crtc_index;
    if (port == 0x3d5) {
        ++seen.crtc_reads;
        return crtc[crtc_index];
    }
    ++seen.stray;
    return 0xff;
}
static uint16_t legacy_vga_rd16(uint64_t a)
{
    int cell = cell_at(a);
    if (cell >= 0) {
        ++seen.cell_reads;
        return screen[cell];
    }
    ++seen.stray;
    return 0xffff;
}
static uint32_t legacy_vga_rd32(uint64_t a)
{
    (void)a;
    ++seen.stray;
    return ~0u;
}
static void legacy_vga_wr8(uint64_t a, uint8_t v)
{
    int port = port_at(a);
    if (port == 0x3d4)
        crtc_index = v & 0x1f;
    else if (port == 0x3d5) {
        ++seen.crtc_writes;
        crtc[crtc_index] = v;
    } else
        ++seen.stray;
}
static void legacy_vga_wr16(uint64_t a, uint16_t v)
{
    int cell = cell_at(a);
    if (cell >= 0) {
        ++seen.cell_writes;
        screen[cell] = v;
    } else
        ++seen.stray;
}
static void legacy_vga_wr32(uint64_t a, uint32_t v)
{
    (void)a;
    (void)v;
    ++seen.stray;
}
static void legacy_vga_wr64(uint64_t a, uint64_t v)
{
    (void)a;
    (void)v;
    ++seen.stray;
}
static uint64_t legacy_vga_ticks(void)
{
    return ticks += 1000;
}
static unsigned accesses(void)
{
    return seen.cell_reads + seen.cell_writes + seen.crtc_reads + seen.crtc_writes + seen.stray;
}
static unsigned cursor_offset(void)
{
    return (unsigned)crtc[0x0e] << 8 | crtc[0x0f];
}
static uint16_t cell(unsigned column, unsigned row)
{
    return screen[row * 80 + column];
}
static void reset_model(void)
{
    memset(screen, 0x5a, sizeof(screen));
    memset(crtc, 0, sizeof(crtc));
    crtc[0x0a] = 0x0d;
    crtc[0x0b] = 0x0e; /* mode 3's underline cursor, as POST left it */
    memset(&seen, 0, sizeof(seen));
}

/* --- EDK2 services VmmConsoleEntry uses -------------------------------------- */
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
VOID *EFIAPI AllocatePool(UINTN size)
{
    return malloc(size);
}
VOID *EFIAPI AllocateCopyPool(UINTN size, const VOID *source)
{
    return CopyMem(AllocatePool(size), source, size);
}
VOID EFIAPI FreePool(VOID *buffer)
{
    free(buffer);
}
VOID EFIAPI DebugPrint(UINTN level, CONST CHAR8 *format, ...)
{
    (void)level;
    (void)format;
}
VOID EFIAPI DebugAssert(CONST CHAR8 *file, UINTN line, CONST CHAR8 *text)
{
    printf("ASSERT %s:%u: %s\n", file, (unsigned)line, text);
    exit(1);
}
BOOLEAN EFIAPI DebugAssertEnabled(VOID)
{
    return TRUE;
}
BOOLEAN EFIAPI DebugPrintEnabled(VOID)
{
    return TRUE;
}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN level)
{
    (void)level;
    return TRUE;
}
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiSimpleTextOutProtocolGuid = EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiSimpleTextInProtocolGuid = EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static unsigned installed_gop, installed_text_out;
static EFI_STATUS EFIAPI install(EFI_HANDLE *handle, ...)
{
    va_list list;
    va_start(list, handle);
    for (EFI_GUID *guid; (guid = va_arg(list, EFI_GUID *)) != NULL;) {
        (void)va_arg(list, VOID *);
        installed_gop += !memcmp(guid, &gEfiGraphicsOutputProtocolGuid, sizeof(*guid));
        installed_text_out += !memcmp(guid, &gEfiSimpleTextOutProtocolGuid, sizeof(*guid));
    }
    va_end(list);
    *handle = (EFI_HANDLE)(UINTN)1;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI create_event(UINT32 type, EFI_TPL tpl, EFI_EVENT_NOTIFY notify,
                                      VOID *context, EFI_EVENT *event)
{
    (void)type;
    (void)tpl;
    (void)notify;
    (void)context;
    *event = (EFI_EVENT)(UINTN)2;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI create_event_ex(UINT32 type, EFI_TPL tpl, EFI_EVENT_NOTIFY notify,
                                         CONST VOID *context, CONST EFI_GUID *group,
                                         EFI_EVENT *event)
{
    (void)type;
    (void)tpl;
    (void)notify;
    (void)context;
    (void)group;
    *event = (EFI_EVENT)(UINTN)3;
    return EFI_SUCCESS;
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
static EFI_BOOT_SERVICES services = {.RaiseTPL = raise_tpl,
                                     .RestoreTPL = restore_tpl,
                                     .CreateEvent = create_event,
                                     .CreateEventEx = create_event_ex,
                                     .InstallMultipleProtocolInterfaces = install};
EFI_BOOT_SERVICES *gBS = &services;

/* --- The handoff ------------------------------------------------------------- */
static struct ia64vmm_handoff valid_tail(void)
{
    struct ia64vmm_handoff h = {0};
    h.flags = IA64VMM_HF_LEGACY_VGA;
    h.vga_bdf = 0x0010;
    h.vga_base = IA64VMM_VGA_MEMORY_BASE;
    h.vga_size = IA64VMM_VGA_MEMORY_SIZE;
    h.legacy_io_base = IA64VMM_LEGACY_IO_BASE;
    h.legacy_io_size = IA64VMM_LEGACY_IO_SIZE;
    h.ram_range_count = 2;
    h.ram[0] = (struct ia64vmm_ram_range){0, 0xa0000};
    h.ram[1] = (struct ia64vmm_ram_range){0x100000, 0x1f000000};
    h.legacy_version = IA64VMM_HANDOFF_LEGACY_VERSION;
    h.legacy_bdf = 0x0010;
    h.legacy_flags =
        IA64VMM_LEGACY_TEXT_CONSOLE | IA64VMM_LEGACY_IVT | IA64VMM_LEGACY_ROM | IA64VMM_LEGACY_STUB;
    h.legacy_ivt_base = IA64VMM_LEGACY_IVT_BASE;
    h.legacy_ivt_size = IA64VMM_LEGACY_IVT_SIZE;
    h.legacy_rom_base = IA64VMM_LEGACY_ROM_BASE;
    h.legacy_rom_size = 0x9000;
    h.legacy_stub_base = IA64VMM_LEGACY_STUB_BASE;
    h.legacy_stub_size = IA64VMM_LEGACY_STUB_SIZE;
    return h;
}

static void test_check(void)
{
    struct ia64vmm_handoff zero = {0}, h = valid_tail();
    CHECK(!legacy_handoff_check(&zero) && !legacy_handoff_text_console(&zero));
    CHECK(!legacy_handoff_check(&h) && legacy_handoff_text_console(&h));
    /* Optional parts may be absent as long as their fields are zero. */
    h.legacy_flags = IA64VMM_LEGACY_TEXT_CONSOLE;
    h.legacy_ivt_size = h.legacy_rom_base = h.legacy_rom_size = h.legacy_stub_base =
        h.legacy_stub_size = 0;
    CHECK(!legacy_handoff_check(&h));
    h.legacy_rom_size = IA64VMM_LEGACY_ROM_MAX;
    h.legacy_rom_base = IA64VMM_LEGACY_ROM_BASE;
    h.legacy_flags |= IA64VMM_LEGACY_ROM;
    CHECK(!legacy_handoff_check(&h));
    static const struct {
        const char *reason;
        unsigned rule;
    } broken[] = {
        {"legacy fields without legacy_version", 0},
        {"legacy fields without legacy_version", 1},
        {"unknown legacy_version", 2},
        {"unknown legacy_flags", 3},
        {"legacy state without the VGA text console", 4},
        {"VGA text console without the card's assigned legacy VGA", 5},
        {"VGA text console without the card's assigned legacy VGA", 6},
        {"VGA text console without the card's assigned legacy VGA", 7},
        {"a framebuffer under the VGA text console", 8},
        {"a framebuffer under the VGA text console", 9},
        {"IVT/BDA location", 10},
        {"IVT/BDA location", 11},
        {"option ROM image location", 12},
        {"option ROM image location", 13},
        {"option ROM image location", 14},
        {"option ROM image location", 15},
        {"option ROM image location", 16},
        {"stub segment location", 17},
        {"stub segment location", 18},
        {"legacy reserved fields", 19},
        {"IVT/BDA outside RAM page 0", 20},
        {"IVT/BDA outside RAM page 0", 21},
    };
    for (unsigned i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) {
        struct ia64vmm_handoff b = valid_tail();
        switch (broken[i].rule) {
        case 0:
            b = zero;
            b.legacy_flags = IA64VMM_LEGACY_TEXT_CONSOLE;
            break;
        case 1:
            b = zero;
            b.legacy_reserved[2] = 1;
            break;
        case 2:
            b.legacy_version = 2;
            break;
        case 3:
            b.legacy_flags |= 1u << 4;
            break;
        case 4:
            b.legacy_flags &= ~IA64VMM_LEGACY_TEXT_CONSOLE;
            break;
        case 5:
            b.flags &= ~IA64VMM_HF_LEGACY_VGA;
            break;
        case 6:
            b.legacy_bdf = 0x0018;
            break;
        case 7:
            b.vga_size = 0x10000;
            break;
        case 8:
            b.fb_format = IA64VMM_FB_BGRX8888;
            break;
        case 9:
            b.fb_base = 0x80000000;
            break;
        case 10:
            b.legacy_ivt_size = 0x400;
            break;
        case 11:
            b.legacy_flags &= ~IA64VMM_LEGACY_IVT;
            break; /* fields left */
        case 12:
            b.legacy_rom_size = 0x9001;
            break;
        case 13:
            b.legacy_rom_size = IA64VMM_LEGACY_ROM_MAX + 512;
            break;
        case 14:
            b.legacy_rom_size = 0;
            break;
        case 15:
            b.legacy_rom_base = 0xd0000;
            break;
        case 16:
            b.legacy_flags &= ~IA64VMM_LEGACY_ROM;
            break;
        case 17:
            b.legacy_stub_size = 0x8000;
            break;
        case 18:
            b.legacy_flags &= ~IA64VMM_LEGACY_STUB;
            break;
        case 19:
            b.legacy_reserved[0] = 1;
            break;
        case 20:
            b.ram[0].base = 0x1000;
            b.ram[0].size = 0x9f000;
            break;
        case 21:
            b.ram_range_count = 0;
            break;
        }
        const char *reason = legacy_handoff_check(&b);
        if (!reason || strcmp(reason, broken[i].reason)) {
            printf("FAIL rule %u: got \"%s\", want \"%s\"\n", broken[i].rule,
                   reason ? reason : "(accepted)", broken[i].reason);
            exit(1);
        }
    }
    /* The policy needs version 1 and the text console flag. */
    h = valid_tail();
    h.legacy_flags = IA64VMM_LEGACY_IVT;
    CHECK(!legacy_handoff_text_console(&h));
    h = valid_tail();
    h.legacy_version = 2;
    CHECK(!legacy_handoff_text_console(&h));
}

static void test_core(void)
{
    CHECK(legacy_text_cp437('A') == 'A' && legacy_text_cp437(' ') == ' ' &&
          legacy_text_cp437('~') == '~');
    CHECK(legacy_text_cp437(0x2500) == 0xc4 && legacy_text_cp437(0x2554) == 0xc9 &&
          legacy_text_cp437(0x2588) == 0xdb && legacy_text_cp437(0x2191) == 0x18 &&
          legacy_text_cp437(0x25bc) == 0x1f && legacy_text_cp437(0x00b7) == 0xfa);
    CHECK(legacy_text_cp437(0x00e9) == '?' && legacy_text_cp437(0x1f) == '?' &&
          legacy_text_cp437(0x7f) == '?' && legacy_text_cp437(0x3042) == '?');
    reset_model();
    static struct legacy_text t;
    t.cells = CELLS;
    t.io_base = IA64VMM_LEGACY_IO_BASE;
    t.attribute = 0x1e;
    t.cursor_visible = true;
    legacy_text_clear(&t);
    for (unsigned i = 0; i < 25 * 80; ++i)
        CHECK(screen[i] == 0x1e20);
    CHECK(cursor_offset() == 0 && crtc[0x0a] == 0x0d && !seen.cell_reads && !seen.stray);
    /* CR, LF, BS at column 0, wrap at column 80. */
    for (const char *s = "AB\r\nC\b\bD"; *s; ++s)
        legacy_text_put(&t, (uint16_t)*s);
    CHECK(cell(0, 0) == 0x1e41 && cell(1, 0) == 0x1e42 && cell(0, 1) == 0x1e44 && t.row == 1 &&
          t.column == 1);
    for (unsigned i = 0; i < 80; ++i)
        legacy_text_put(&t, (uint16_t)('0' + i % 10));
    CHECK(t.row == 2 && t.column == 1 && cell(79, 1) == 0x1e38 && cell(0, 2) == 0x1e39);
    legacy_text_cursor(&t);
    CHECK(cursor_offset() == 2 * 80 + 1);
    t.cursor_visible = false;
    legacy_text_cursor(&t);
    CHECK(crtc[0x0a] == 0x2d); /* bit 5 set, start line kept */
    t.cursor_visible = true;
    legacy_text_cursor(&t);
    CHECK(crtc[0x0a] == 0x0d);
    /* Scrolling: from the shadow, never from device memory. */
    t.row = 24;
    t.column = 0;
    for (const char *s = "last"; *s; ++s)
        legacy_text_put(&t, (uint16_t)*s);
    legacy_text_put(&t, '\n');
    CHECK(t.row == 24 && t.column == 4); /* LF alone keeps the column */
    CHECK(cell(0, 23) == 0x1e6c && cell(3, 23) == 0x1e74 && cell(0, 24) == 0x1e20);
    CHECK(cell(0, 0) == 0x1e44 && cell(79, 0) == 0x1e38 &&
          cell(0, 1) == 0x1e39); /* rows 1, 2 moved up */
    CHECK(!seen.cell_reads && !seen.stray);
    /* A full line in the last row wraps and scrolls. */
    t.column = 0;
    for (unsigned i = 0; i < 80; ++i)
        legacy_text_put(&t, 'x');
    CHECK(t.row == 24 && t.column == 0 && cell(0, 23) == 0x1e78 && cell(79, 23) == 0x1e78 &&
          cell(0, 24) == 0x1e20);
    CHECK(!seen.cell_reads && !seen.stray);
}

static CHAR16 *wide(const char *s)
{
    static CHAR16 buffer[256];
    unsigned i = 0;
    for (; s[i] && i < 255; ++i)
        buffer[i] = (CHAR16)(unsigned char)s[i];
    buffer[i] = 0;
    return buffer;
}

static void enter(struct ia64vmm_handoff h)
{
    test_handoff = h;
    installed_gop = installed_text_out = 0;
    CHECK(VmmConsoleEntry(NULL, NULL) == EFI_SUCCESS && installed_text_out == 1);
}

static void test_console(void)
{
    static uint8_t uart[8];
    /* Default path (no tail): no VGA text console, the device untouched. */
    reset_model();
    struct ia64vmm_handoff h = valid_tail();
    h.legacy_version = 0;
    h.legacy_flags = 0;
    h.legacy_bdf = 0;
    h.legacy_ivt_size = h.legacy_rom_base = h.legacy_rom_size = h.legacy_stub_base =
        h.legacy_stub_size = 0;
    enter(h);
    CHECK(text.OutputString(&text, wide("default")) == EFI_SUCCESS && !accesses());
    CHECK(service_console_used == 7 && !memcmp(service_console, "default", 7));
    /* A serial console boot keeps the UART as the console. */
    h = valid_tail();
    h.flags |= IA64VMM_HF_UART_PRESENT | IA64VMM_HF_UART_CONSOLE;
    h.uart_base = (UINTN)uart;
    enter(h);
    CHECK(text.OutputString(&text, wide("serial")) == EFI_SUCCESS && !accesses() && uart[0] == 'l');
    /* display=vbios: cleared in the default attribute, cursor at 0, no GOP.
     * The card is one the GOP leave would handle (std-vga): under the
     * policy nothing may leave anything at ExitBootServices. */
    text.SetAttribute(&text, EFI_LIGHTGRAY);
    h = valid_tail();
    h.vga_kind = IA64VMM_VGA_BOCHS;
    enter(h);
    CHECK(!installed_gop && !seen.stray && !seen.cell_reads);
    for (unsigned i = 0; i < 25 * 80; ++i)
        CHECK(screen[i] == 0x0720);
    CHECK(cursor_offset() == 0 && crtc[0x0a] == 0x0d);
    service_console_used = 0;
    CHECK(text.OutputString(&text, wide("Hello\r\nworld")) == EFI_SUCCESS);
    CHECK(service_console_used == 12 &&
          !memcmp(service_console, "Hello\r\nworld", 12)); /* still mirrored */
    CHECK(cell(0, 0) == 0x0748 && cell(4, 0) == 0x076f && cell(0, 1) == 0x0777 &&
          cell(4, 1) == 0x0764);
    CHECK(text.Mode->CursorColumn == 5 && text.Mode->CursorRow == 1 && cursor_offset() == 85);
    /* SetAttribute is the VGA attribute byte; ClearScreen fills with it. */
    CHECK(text.SetAttribute(&text, EFI_TEXT_ATTR(EFI_YELLOW, EFI_BLUE)) == EFI_SUCCESS);
    CHECK(text.ClearScreen(&text) == EFI_SUCCESS && screen[0] == 0x1e20 && screen[1999] == 0x1e20);
    CHECK(text.Mode->CursorColumn == 0 && text.Mode->CursorRow == 0 && cursor_offset() == 0);
    /* Box drawing as the boot manager draws it. */
    CHAR16 box[] = {0x2554, 0x2550, 0x2557, 0};
    CHECK(text.SetCursorPosition(&text, 10, 3) == EFI_SUCCESS && cursor_offset() == 3 * 80 + 10);
    CHECK(text.OutputString(&text, box) == EFI_SUCCESS);
    CHECK(cell(10, 3) == 0x1ec9 && cell(11, 3) == 0x1ecd && cell(12, 3) == 0x1ebb &&
          cursor_offset() == 253);
    CHECK(text.SetCursorPosition(&text, 80, 0) == EFI_UNSUPPORTED && cursor_offset() == 253);
    CHECK(text.SetAttribute(&text, EFI_TEXT_ATTR(EFI_WHITE, EFI_RED)) == EFI_SUCCESS);
    CHECK(text.OutputString(&text, wide("!")) == EFI_SUCCESS && cell(13, 3) == 0x4f21);
    CHECK(text.SetAttribute(&text, EFI_TEXT_ATTR(EFI_YELLOW, EFI_BLUE)) == EFI_SUCCESS);
    CHECK(text.EnableCursor(&text, FALSE) == EFI_SUCCESS && crtc[0x0a] == 0x2d);
    CHECK(text.EnableCursor(&text, TRUE) == EFI_SUCCESS && crtc[0x0a] == 0x0d);
    /* The last row scrolls without reading the device. */
    CHECK(text.SetCursorPosition(&text, 0, 24) == EFI_SUCCESS);
    CHECK(text.OutputString(&text, wide("bottom\r\n")) == EFI_SUCCESS);
    CHECK(cell(0, 23) == 0x1e62 && cell(0, 24) == 0x1e20 && cell(10, 2) == 0x1ec9);
    CHECK(text.Mode->CursorRow == 24 && text.Mode->CursorColumn == 0 && cursor_offset() == 24 * 80);
    CHECK(!seen.cell_reads && !seen.stray);
    /* Reset clears as well; ExitBootServices leaves the device alone. */
    CHECK(text.SetCursorPosition(&text, 0, 0) == EFI_SUCCESS &&
          text.OutputString(&text, wide("R")) == EFI_SUCCESS);
    CHECK(screen[0] == 0x1e52);
    CHECK(text.Reset(&text, FALSE) == EFI_SUCCESS && screen[0] == 0x1e20 && cursor_offset() == 0);
    unsigned before = accesses();
    exit_boot(NULL, NULL);
    CHECK(accesses() == before && !service_other);
}

int main(void)
{
    test_check();
    test_core();
    test_console();
    puts(
        "legacy BIOS: tail check (22 broken tails), text console core (CP437, wrap, scroll without "
        "device reads, CRTC cursor) and VmmConsole under display=vbios (no GOP, no change at "
        "ExitBootServices, default and serial paths untouched) passed");
    return 0;
}
