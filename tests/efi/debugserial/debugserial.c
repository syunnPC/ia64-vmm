/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Inspect policy and firmware descriptions without probing absent UARTs. */
#include <efi.h>
#include "../../../include/ia64vmm/platform.h"

static EFI_SYSTEM_TABLE *st;
static unsigned failures;
/* GNU-EFI freestanding applications do not link libc. */
void *memcpy(void *dest, const void *source, UINTN size)
{
    unsigned char *d = dest;
    const unsigned char *s = source;
    while (size--)
        *d++ = *s++;
    return dest;
}
static const EFI_GUID acpi20 = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
static const EFI_GUID hcdp_guid = {
    0xf951938d, 0x620b, 0x42ef, {0x82, 0x79, 0xa8, 0x4b, 0x79, 0x61, 0x78, 0x98}};
static EFI_GUID global = {
    0x8be4df61, 0x93ca, 0x11d2, {0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c}};
static void say(const char *s)
{
    /* A diagnostic service is available even when ConOut is graphics-only. */
    while (*s)
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE) = (unsigned char)*s++;
}
static void check(int ok, const char *what)
{
    say(ok ? "debugserial PASS " : "debugserial FAIL ");
    say(what);
    say("\n");
    failures += !ok;
}
static UINT64 le(const unsigned char *p, unsigned n)
{
    UINT64 v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (UINT64)p[i] << (i * 8);
    return v;
}
static int same(const void *a, const void *b, unsigned n)
{
    const unsigned char *p = a, *q = b;
    for (unsigned i = 0; i < n; ++i)
        if (p[i] != q[i])
            return 0;
    return 1;
}
static int valid(const unsigned char *p)
{
    if (!p)
        return 0;
    UINT64 n = le(p + 4, 4);
    unsigned sum = 0;
    if (n < 36 || n > 65536)
        return 0;
    for (UINT64 i = 0; i < n; ++i)
        sum += p[i];
    return !(sum & 255);
}
static const unsigned char *config(const EFI_GUID *g)
{
    for (UINTN i = 0; i < st->NumberOfTableEntries; ++i)
        if (same(g, &st->ConfigurationTable[i].VendorGuid, 16))
            return st->ConfigurationTable[i].VendorTable;
    return 0;
}
static const unsigned char *table(const unsigned char *xsdt, const char *sig)
{
    if (!valid(xsdt))
        return 0;
    for (UINTN i = 36; i + 8 <= le(xsdt + 4, 4); i += 8) {
        const unsigned char *t = (void *)(UINTN)le(xsdt + i, 8);
        if (same(t, sig, 4))
            return t;
    }
    return 0;
}
static unsigned uart_aml(const unsigned char *t)
{
    static const unsigned char hid[] = {0x08, '_', 'H', 'I', 'D', 0x0c, 0x41, 0xd0, 0x05, 0x01};
    if (!valid(t))
        return 0;
    unsigned found = 0;
    for (UINTN i = 36; i + sizeof(hid) <= le(t + 4, 4); ++i)
        if (same(t + i, hid, sizeof(hid)))
            ++found;
    return found;
}
static unsigned uart_crs(const unsigned char *t)
{
    if (!valid(t))
        return 0;
    unsigned found = 0;
    for (UINTN i = 36; i + 46 <= le(t + 4, 4); ++i) {
        const unsigned char *p = t + i;
        if (p[0] == 0x8a && le(p + 1, 2) == 43 && p[3] == 0 && p[4] == 0x0d &&
            le(p + 14, 8) == IA64VMM_UART_BASE && le(p + 22, 8) == IA64VMM_UART_BASE + 7 &&
            le(p + 30, 8) == 0 && le(p + 38, 8) == 8)
            ++found;
    }
    return found;
}
static unsigned uart_path(CHAR16 *name)
{
    unsigned char data[2048];
    UINTN n = sizeof(data);
    UINT32 attrs;
    EFI_STATUS s = st->RuntimeServices->GetVariable(name, &global, &attrs, &n, data);
    if (EFI_ERROR(s)) {
        check(0, "console variable readable");
        return 0;
    }
    for (UINTN i = 0; i + 4 <= n;) {
        UINTN len = le(data + i + 2, 2);
        if (len < 4 || len > n - i) {
            check(0, "console path well formed");
            return 0;
        }
        if (data[i] == 3 && data[i + 1] == 14)
            return 1;
        i += len;
    }
    return 0;
}
EFI_STATUS _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    (void)image;
    st = system;
    UINT64 flags = *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_DEBUG_FLAGS);
    unsigned present = !!(flags & IA64VMM_HF_UART_PRESENT),
             console = !!(flags & IA64VMM_HF_UART_CONSOLE);
    unsigned vga = !!(flags & IA64VMM_HF_LEGACY_VGA);
    say("debugserial mode=");
    say(console ? "console\n" : present ? "device\n" : "off\n");
    check(!console || present, "console implies present");
    const unsigned char *rsdp = config(&acpi20), *xsdt = 0;
    if (rsdp)
        xsdt = (void *)(UINTN)le(rsdp + 24, 8);
    check(valid(xsdt), "ACPI root");
    const unsigned char *spcr = table(xsdt, "SPCR"), *hcdp = config(&hcdp_guid);
    check(!!spcr == console && (!spcr || (valid(spcr) && spcr[40] == 0 &&
                                          le(spcr + 44, 8) == IA64VMM_UART_BASE && spcr[52] == 0)),
          "SPCR console-only, MMIO, polling");
    check(!!hcdp == !!(console || vga), "PCDP publication");
    const unsigned char *acpi_hcdp = table(xsdt, "HCDP");
    check(!!acpi_hcdp == !!hcdp && (!hcdp || (valid(acpi_hcdp) && valid(hcdp) &&
                                              le(acpi_hcdp + 4, 4) == le(hcdp + 4, 4) &&
                                              same(acpi_hcdp, hcdp, le(hcdp + 4, 4)))),
          "PCDP ACPI and EFI descriptions agree");
    if (hcdp) {
        check(valid(hcdp) && le(hcdp + 36, 4) == console, "PCDP UART count");
        if (console)
            check(hcdp[56] == 0 && le(hcdp + 60, 8) == IA64VMM_UART_BASE && (hcdp[81] & 4),
                  "PCDP serial primary");
        UINTN at = 40 + 48 * console;
        if (vga)
            check(at + 153 <= le(hcdp + 4, 4) && hcdp[at] == 10 &&
                      !!(hcdp[at + 1] & 1) == !console &&
                      le(hcdp + at + 30, 8) == IA64VMM_LEGACY_IO_BASE,
                  "PCDP real VGA and sparse I/O translation");
    }
    unsigned aml_count = 0, crs_count = 0;
    const unsigned char *fadt = table(xsdt, "FACP");
    if (fadt) {
        UINT64 dsdt = le(fadt + 40, 4);
        if (le(fadt + 4, 4) >= 148 && le(fadt + 140, 8))
            dsdt = le(fadt + 140, 8);
        aml_count += uart_aml((void *)(UINTN)dsdt);
        crs_count += uart_crs((void *)(UINTN)dsdt);
    }
    if (valid(xsdt))
        for (UINTN i = 36; i + 8 <= le(xsdt + 4, 4); i += 8) {
            const unsigned char *t = (void *)(UINTN)le(xsdt + i, 8);
            if (same(t, "SSDT", 4)) {
                aml_count += uart_aml(t);
                crs_count += uart_crs(t);
            }
        }
    check(aml_count == present, "ACPI PNP0501 device exactly when present");
    check(crs_count == present, "UART MMIO _CRS exactly when present, no IRQ");
    UINTN n = 0, key, stride;
    UINT32 ver;
    EFI_STATUS s = st->BootServices->GetMemoryMap(&n, 0, &key, &stride, &ver);
    EFI_MEMORY_DESCRIPTOR *map = 0;
    n += stride * 8;
    if (s == EFI_BUFFER_TOO_SMALL)
        s = st->BootServices->AllocatePool(EfiLoaderData, n, (VOID **)&map);
    if (!EFI_ERROR(s))
        s = st->BootServices->GetMemoryMap(&n, map, &key, &stride, &ver);
    unsigned uart_map = 0, vga_map = 0;
    if (!EFI_ERROR(s))
        for (UINTN i = 0; i + stride <= n; i += stride) {
            EFI_MEMORY_DESCRIPTOR *d = (void *)((unsigned char *)map + i);
            if (IA64VMM_UART_BASE >= d->PhysicalStart &&
                IA64VMM_UART_BASE - d->PhysicalStart < d->NumberOfPages * 4096) {
                ++uart_map;
                check(d->Type == EfiMemoryMappedIO &&
                          (d->Attribute & (EFI_MEMORY_UC | EFI_MEMORY_RUNTIME)) ==
                              (EFI_MEMORY_UC | EFI_MEMORY_RUNTIME),
                      "UART EFI runtime UC MMIO");
            }
            if (d->Type == EfiMemoryMappedIO && IA64VMM_VGA_MEMORY_BASE >= d->PhysicalStart &&
                IA64VMM_VGA_MEMORY_BASE - d->PhysicalStart < d->NumberOfPages * 4096) {
                ++vga_map;
                check((d->Attribute & EFI_MEMORY_UC) &&
                          IA64VMM_VGA_MEMORY_SIZE <=
                              d->NumberOfPages * 4096 -
                                  (IA64VMM_VGA_MEMORY_BASE - d->PhysicalStart),
                      "VGA EFI UC MMIO aperture");
            }
        }
    check(!EFI_ERROR(s) && uart_map == present, "UART EFI map membership");
    check(!EFI_ERROR(s) && (!vga || vga_map == 1), "assigned VGA EFI map membership");
    if (map)
        st->BootServices->FreePool(map);
    check(uart_path(L"ConOut") == console, "ConOut UART membership");
    check(uart_path(L"ConIn") == console, "ConIn UART membership");
    if (present) {
        static const unsigned char raw[] = {'D', 'S', ':', 0, '\r', '\n', 0xff, 0x1b, '[', 'm'};
        volatile unsigned char *uart = (void *)(UINTN)IA64VMM_UART_BASE;
        uart[3] = 3;
        uart[4] = 0;
        for (unsigned i = 0; i < sizeof(raw); ++i)
            uart[0] = raw[i];
        /* Console paths must name an actual input implementation. Loopback
         * exercises it without adding a host input transport or TX bytes. */
        if (console) {
            uart[4] = 0x10;
            uart[0] = 'K';
            EFI_INPUT_KEY key = {0};
            EFI_STATUS input = st->ConIn->ReadKeyStroke(st->ConIn, &key);
            check(input == EFI_SUCCESS && key.UnicodeChar == 'K', "serial ConIn loopback");
            uart[4] = 0;
        }
    }
    say(failures ? "debugserial behaviour FAILED\n" : "debugserial behaviour PASSED\n");
    st->RuntimeServices->ResetSystem(EfiResetShutdown, failures ? EFI_ABORTED : EFI_SUCCESS, 0, 0);
    return failures ? EFI_ABORTED : EFI_SUCCESS;
}
