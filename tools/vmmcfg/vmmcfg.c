/* SPDX-License-Identifier: GPL-2.0-or-later */
/* UEFI editor for \vmm-options.conf on the boot volume. */
#include <efi.h>
#include "cfg.h"

#define OPTIONS_PATH L"\\vmm-options.conf"
#define FILE_MAX 8192

static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID file_system_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_SYSTEM_TABLE *st;
static EFI_FILE_HANDLE root;
static struct cfg cfg;
static char file_buf[FILE_MAX + 1], out_buf[FILE_MAX];
static int read_only; /* the file holds more than vmmcfg can keep */

/* The compiler may call these even with -ffreestanding. */
__attribute__((used)) void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}
__attribute__((used)) void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static CHAR16 line[96];
static unsigned line_len;
static void put(char c)
{
    if (line_len < 79)
        line[line_len++] = (CHAR16)(unsigned char)c;
}
static void puts_(const char *s)
{
    while (*s)
        put(*s++);
}
static void put_hex(UINTN v)
{
    static const char digits[] = "0123456789abcdef";
    puts_("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        if (v >> shift || !shift)
            put(digits[(v >> shift) & 15]);
}
/* One output row; pad > 0 fills the row to 79 columns (selection bar). */
static void row(int pad)
{
    while (pad && line_len < 79)
        line[line_len++] = L' ';
    line[line_len] = 0;
    st->ConOut->OutputString(st->ConOut, line);
    line_len = 0;
}
static void nl(void)
{
    st->ConOut->OutputString(st->ConOut, L"\r\n");
}
static void line_(const char *s)
{
    puts_(s);
    row(0);
    nl();
}

static EFI_INPUT_KEY wait_key(void)
{
    EFI_INPUT_KEY key = {0, 0};
    UINTN index;
    for (;;) {
        st->BootServices->WaitForEvent(1, &st->ConIn->WaitForKey, &index);
        if (st->ConIn->ReadKeyStroke(st->ConIn, &key) == EFI_SUCCESS)
            return key;
    }
}

/* \vmm-options.conf into file_buf; absent = empty. Returns the status of a
 * failure other than "not found" (EFI_SUCCESS otherwise). */
static EFI_STATUS read_options(UINTN *size)
{
    EFI_FILE_HANDLE f;
    EFI_STATUS s = root->Open(root, &f, OPTIONS_PATH, EFI_FILE_MODE_READ, 0);
    *size = 0;
    if (s == EFI_NOT_FOUND)
        return EFI_SUCCESS;
    if (s != EFI_SUCCESS)
        return s;
    UINTN n = FILE_MAX + 1;
    s = f->Read(f, &n, file_buf);
    f->Close(f);
    *size = n;
    return s;
}

/* Write the whole file: delete the old one, create, write, flush, close. */
static EFI_STATUS write_options(const char *data, UINTN size)
{
    EFI_FILE_HANDLE f;
    EFI_STATUS s = root->Open(root, &f, OPTIONS_PATH, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (s == EFI_SUCCESS) {
        s = f->Delete(f); /* closes the handle either way */
        if (s != EFI_SUCCESS)
            return s;
    } else if (s != EFI_NOT_FOUND)
        return s;
    s = root->Open(root, &f, OPTIONS_PATH,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (s != EFI_SUCCESS)
        return s;
    UINTN n = size;
    s = f->Write(f, &n, (void *)data);
    if (s == EFI_SUCCESS && n != size)
        s = EFI_VOLUME_FULL;
    if (s == EFI_SUCCESS)
        s = f->Flush(f);
    EFI_STATUS c = f->Close(f);
    return s != EFI_SUCCESS ? s : c;
}

/* Returns 1 on success. */
static int save(void)
{
    st->ConOut->ClearScreen(st->ConOut);
    if (read_only) {
        line_("Not saved: the file holds more than this tool keeps; edit it by hand.");
        return 0;
    }
    size_t n = cfg_serialize(&cfg, out_buf, sizeof(out_buf));
    if (!n) {
        line_("Not saved: the tokens exceed 511 bytes (the VMM command line budget).");
        return 0;
    }
    EFI_STATUS s = write_options(out_buf, n);
    if (s != EFI_SUCCESS) {
        puts_("Not saved: file error, status ");
        put_hex((UINTN)s);
        row(0);
        nl();
        if (s == EFI_WRITE_PROTECTED || s == EFI_ACCESS_DENIED)
            line_("(the boot volume is write protected)");
        return 0;
    }
    line_("Saved \\vmm-options.conf.");
    return 1;
}

static void reset(EFI_STATUS status)
{
    st->RuntimeServices->ResetSystem(EfiResetCold, status, 0, NULL);
    for (;;) {
    }
}

static void draw(unsigned sel, int editing, const char *message)
{
    EFI_SIMPLE_TEXT_OUT_PROTOCOL *o = st->ConOut;
    o->SetAttribute(o, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    o->ClearScreen(o);
    line_("Configure VMM Options - \\vmm-options.conf on the boot volume");
    line_("User-config boot uses all settings; safe mode keeps only media selectors.");
    nl();
    UINTN columns = 80, rows = 25;
    o->QueryMode(o, o->Mode->Mode, &columns, &rows);
    unsigned visible = cfg_page_rows((unsigned)rows);
    unsigned first = cfg_page_first(sel, visible);
    for (unsigned i = first; i <= CFG_SETTINGS && i < first + visible; ++i) {
        int picked = i == sel;
        puts_(picked ? " > " : "   ");
        if (i < CFG_SETTINGS) {
            const char *label = cfg_settings[i].label;
            unsigned n = 0;
            for (; label[n]; ++n)
                put(label[n]);
            for (; n < 28; ++n)
                put(' ');
            puts_(cfg_value_text(&cfg, i));
        } else {
            puts_("extra tokens: ");
            puts_(cfg.extra);
            if (editing)
                put('_');
        }
        if (picked)
            o->SetAttribute(o, EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY));
        row(picked);
        o->SetAttribute(o, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
        nl();
    }
    line_(message ? message
                  : "At boot: capped at half of host RAM and the largest usable contiguous range.");
    line_("Up/Down select   Left/Right or Enter change the value");
    line_("Enter on 'extra tokens' types (Backspace, Enter ends)");
    line_("S save   R save and reset   X reset without saving");
    puts_("After the reset, pick an entry from the boot loader.");
    row(0);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    EFI_LOADED_IMAGE *loaded;
    EFI_FILE_IO_INTERFACE *fs;
    EFI_STATUS s;
    UINTN size;
    st = system;
    st->BootServices->SetWatchdogTimer(0, 0, 0,
                                       NULL); /* no 5-minute reset while reading settings */
    st->ConIn->Reset(st->ConIn, FALSE);
    st->ConOut->EnableCursor(st->ConOut, FALSE);
    cfg_init(&cfg);
    s = st->BootServices->HandleProtocol(image, &loaded_image_guid, (void **)&loaded);
    if (s == EFI_SUCCESS)
        s = st->BootServices->HandleProtocol(loaded->DeviceHandle, &file_system_guid, (void **)&fs);
    if (s == EFI_SUCCESS)
        s = fs->OpenVolume(fs, &root);
    if (s != EFI_SUCCESS) {
        st->ConOut->ClearScreen(st->ConOut);
        puts_("Cannot open the boot volume, status ");
        put_hex((UINTN)s);
        row(0);
        nl();
        line_("Press any key to reset.");
        wait_key();
        reset(s);
    }
    const char *message = NULL;
    s = read_options(&size);
    if (s != EFI_SUCCESS || size > FILE_MAX) {
        read_only = 1;
        message = "Cannot read the options file; saving is disabled. X resets.";
    } else if (!cfg_parse(&cfg, file_buf, size)) {
        read_only = 1;
        message = "The file holds more than this tool keeps; saving is disabled. X resets.";
    }
    unsigned sel = 0;
    int editing = 0;
    for (;;) {
        draw(sel, editing, message);
        message = NULL;
        EFI_INPUT_KEY key = wait_key();
        if (editing) {
            if (key.UnicodeChar == 13 || key.ScanCode == 0x17)
                editing = 0;
            else if (key.UnicodeChar == 8)
                cfg_extra_backspace(&cfg);
            else if (key.UnicodeChar)
                cfg_extra_put(&cfg, (char)(key.UnicodeChar < 0x80 ? key.UnicodeChar : 0));
            continue;
        }
        CHAR16 c = key.UnicodeChar;
        if (c >= L'a' && c <= L'z')
            c = (CHAR16)(c - 32);
        if (key.ScanCode == 1 && sel)
            --sel; /* Up */
        else if (key.ScanCode == 2 && sel < CFG_SETTINGS)
            ++sel;                                                    /* Down */
        else if (key.ScanCode == 3 || key.ScanCode == 4 || c == 13) { /* Right, Left, Enter */
            if (sel < CFG_SETTINGS)
                cfg_cycle(&cfg, sel, key.ScanCode == 4 ? -1 : 1);
            else if (c == 13)
                editing = 1;
        } else if (c == L'S') {
            save();
            line_("");
            puts_("Press any key.");
            row(0);
            wait_key();
        } else if (c == L'R') {
            if (save()) {
                nl();
                line_("Resetting in 4 seconds...");
                st->BootServices->Stall(4000000);
                reset(EFI_SUCCESS);
            }
            nl();
            puts_("Press any key.");
            row(0);
            wait_key();
        } else if (c == L'X')
            reset(EFI_SUCCESS);
    }
}
