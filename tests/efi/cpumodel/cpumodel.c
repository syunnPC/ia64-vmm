/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute the guest CPUID reader after either firmware reaches StartImage. */
#include <efi.h>
#ifndef CPU_MODEL
#define CPU_MODEL 1
#endif
#ifndef CPU_COUNT
#define CPU_COUNT 1
#endif
struct pal_result {
    UINT64 status, x, y, z;
};
extern struct pal_result model_pal_call(UINT64, UINT64, UINT64, UINT64, UINT64);
static void say(EFI_SYSTEM_TABLE *st, const CHAR16 *s)
{
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)s);
}
static UINT64 le(const unsigned char *p, unsigned size)
{
    UINT64 value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (UINT64)p[i] << (8 * i);
    return value;
}
static BOOLEAN same(const void *a, const void *b, UINTN size)
{
    const unsigned char *x = a, *y = b;
    for (UINTN i = 0; i < size; ++i)
        if (x[i] != y[i])
            return FALSE;
    return TRUE;
}
static BOOLEAN smbios_model(EFI_SYSTEM_TABLE *st)
{
    static const EFI_GUID guid = SMBIOS_TABLE_GUID;
    static const char *const brands[] = {"VMM Montecito-compatible IA-64 CPU 1.60GHz 12MB",
                                         "VMM Madison-compatible IA-64 CPU 1.60GHz 3MB"};
    const char *brand = brands[CPU_MODEL];
    UINTN brand_size = 1;
    while (brand[brand_size - 1])
        ++brand_size;
    const unsigned char *ep = 0;
    for (UINTN i = 0; i < st->NumberOfTableEntries; ++i)
        if (same(&guid, &st->ConfigurationTable[i].VendorGuid, 16))
            ep = st->ConfigurationTable[i].VendorTable;
    if (!ep || !same(ep, "_SM_", 4) || !same(ep + 16, "_DMI_", 5))
        return FALSE;
    const unsigned char *p = (const void *)(UINTN)le(ep + 24, 4), *end = p + le(ep + 22, 2);
    unsigned sockets = 0, cpus = 0;
    while (p + 4 <= end) {
        if (p[1] < 4 || p[1] > (UINTN)(end - p))
            return FALSE;
        const unsigned char *strings = p + p[1], *next = strings;
        while (next + 1 < end && (next[0] || next[1]))
            ++next;
        if (next + 1 >= end)
            return FALSE;
        if (p[0] == 4) {
            if (p[1] < 42 || p[6] != 0xb8 || p[25] != 1 || le(p + 40, 2) != 0xb8 ||
                le(p + 18, 2) != 400 || le(p + 20, 2) != 1600 || le(p + 22, 2) != 1600 ||
                le(p + 8, 8) != (CPU_MODEL ? 0x000000011f010504ULL : 0x0000000520000704ULL) ||
                !p[16])
                return FALSE;
            const unsigned char *version = strings;
            for (unsigned index = 1; index < p[16]; ++index) {
                while (version < next && *version)
                    ++version;
                if (version >= next)
                    return FALSE;
                ++version;
            }
            if (version + brand_size > next + 1 || !same(version, brand, brand_size))
                return FALSE;
            unsigned width = CPU_MODEL || CPU_COUNT == 1 ? 1 : 2;
            unsigned cores = CPU_COUNT - cpus;
            if (cores > width)
                cores = width;
            if (p[35] != cores || p[36] != cores || p[37] != cores || p[24] != 0x41)
                return FALSE;
            cpus += cores;
            ++sockets;
        }
        p = next + 2;
    }
    return cpus == CPU_COUNT && sockets == (CPU_MODEL ? CPU_COUNT : (CPU_COUNT + 1) / 2) &&
           p == end;
}
static BOOLEAN pal_model(EFI_SYSTEM_TABLE *st)
{
    static const EFI_GUID guid = SAL_SYSTEM_TABLE_GUID;
    const unsigned char *table = 0;
    for (UINTN i = 0; i < st->NumberOfTableEntries; ++i)
        if (same(&guid, &st->ConfigurationTable[i].VendorGuid, 16))
            table = st->ConfigurationTable[i].VendorTable;
    if (!table || table[96] != 0)
        return FALSE;
    UINT64 entry = le(table + 104, 8);
    unsigned cores = CPU_MODEL || CPU_COUNT == 1 ? 1 : 2;
    struct pal_result r = model_pal_call(entry, 274, 17, 0, 0);
    if (r.status || r.x != (CPU_MODEL ? 3145728ULL : 12582912ULL) * cores || r.y || r.z)
        return FALSE;
    r = model_pal_call(entry, 42, ~0ULL, 0, 0);
    if (CPU_MODEL)
        return r.status == ~0ULL && !r.x && !r.y && !r.z;
    UINT64 info = cores | (1ULL << 16) | ((UINT64)cores << 32);
    if (r.status || r.x != info || r.y || r.z)
        return FALSE;
    for (unsigned n = 0; n <= cores; ++n) {
        r = model_pal_call(entry, 42, n, 0, 0);
        if (n == cores) {
            if (r.status != ~1ULL || r.x || r.y || r.z)
                return FALSE;
        } else if (r.status || r.x != info || r.y != ((UINT64)n << 32) || r.z != n)
            return FALSE;
    }
    return TRUE;
}
EFI_STATUS _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    UINT64 index = 3, id, features;
    __asm__ volatile("mov %0=cpuid[%1]" : "=r"(id) : "r"(index));
    index = 4;
    __asm__ volatile("mov %0=cpuid[%1]" : "=r"(features) : "r"(index));
    BOOLEAN tables = smbios_model(st);
    say(st, tables ? L"CPU MODEL SMBIOS PASSED\r\n" : L"CPU MODEL SMBIOS FAILED\r\n");
    BOOLEAN pal = pal_model(st);
    say(st, pal ? L"CPU MODEL PAL TOPOLOGY PASSED\r\n" : L"CPU MODEL PAL TOPOLOGY FAILED\r\n");
    BOOLEAN ok = id == (CPU_MODEL ? 0x1f010504 : 0x20000704) && features == (CPU_MODEL ? 1 : 5) &&
                 tables && pal;
    say(st, ok ? (CPU_MODEL ? L"CPU MODEL PASSED: family=31 model=1 revision=5 features=1\r\n"
                            : L"CPU MODEL PASSED: family=32 model=0 revision=7 features=5\r\n")
               : L"CPU MODEL FAILED\r\n");
    st->RuntimeServices->ResetSystem(EfiResetShutdown, ok ? EFI_SUCCESS : EFI_ABORTED, 0, 0);
    return ok ? EFI_SUCCESS : EFI_ABORTED;
}
