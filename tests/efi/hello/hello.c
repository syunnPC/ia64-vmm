/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hello.efi: the acceptance application.
 *
 * Prints a fixed banner and the results of a few computations that exercise
 * the IA-64 features ordinary compiled C relies on: integer multiply/divide
 * (FP-unit sequences), deep calls (RSE spill/fill), function descriptors,
 * boot-services memory allocation and the memory map. Every line is
 * deterministic so the output can be compared against a golden file.
 */

#include <efi.h>

static EFI_SYSTEM_TABLE *gST;

static void puts16(const CHAR16 *s)
{
    gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)s);
}

static void puts8(const char *s)
{
    CHAR16 buf[128];
    UINTN n = 0;

    while (*s) {
        buf[n++] = (CHAR16)(unsigned char)*s++;
        if (n == 126 || *s == 0) {
            buf[n] = 0;
            puts16(buf);
            n = 0;
        }
    }
}

static void puthex(UINT64 v)
{
    char buf[19];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xf];
    }
    buf[18] = 0;
    puts8(buf);
}

static void putdec(INT64 v)
{
    char buf[24];
    int i = 23;
    UINT64 u = v < 0 ? (UINT64)-v : (UINT64)v;

    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0) {
        buf[--i] = '-';
    }
    puts8(&buf[i]);
}

static void line(const char *label, UINT64 v)
{
    puts8(label);
    puthex(v);
    puts8("\r\n");
}

/* Deep enough to overflow the 96 stacked registers and force RSE spills. */
static UINT64 __attribute__((noinline)) fib(UINT64 n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static UINT64 __attribute__((noinline)) depth(UINT64 n, UINT64 acc)
{
    volatile UINT64 pad[4] = {n, acc, n ^ acc, n + acc};

    if (n == 0) {
        return acc + pad[2];
    }
    return depth(n - 1, acc * 3 + pad[3]) ^ pad[0];
}

typedef UINT64 (*binop_t)(UINT64, UINT64);

static UINT64 __attribute__((noinline)) op_div(UINT64 a, UINT64 b)
{
    return a / b;
}
static UINT64 __attribute__((noinline)) op_mod(UINT64 a, UINT64 b)
{
    return a % b;
}
static UINT64 __attribute__((noinline)) op_mul(UINT64 a, UINT64 b)
{
    return a * b;
}

static INT64 __attribute__((noinline)) sdiv(INT64 a, INT64 b)
{
    return a / b;
}
static UINT32 __attribute__((noinline)) udiv32(UINT32 a, UINT32 b)
{
    return a / b;
}

static UINT64 crc_like(const UINT8 *p, UINTN n)
{
    UINT64 h = 0xcbf29ce484222325ULL;

    while (n--) {
        h ^= *p++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    static const binop_t ops[3] = {op_div, op_mod, op_mul};
    volatile UINT64 a = 0x123456789abcdef1ULL;
    volatile UINT64 b = 0x1234567ULL;
    UINT64 acc = 0;
    UINTN i;
    UINT8 *pool = NULL;
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_version = 0;
    EFI_STATUS status;

    (void)image;
    gST = st;

    puts8("IA64-VMM: hello from IA-64 EFI\r\n");
    line("fw-revision   ", st->FirmwareRevision);
    line("hdr-revision  ", st->Hdr.Revision);

    for (i = 0; i < 3; i++) {
        acc = acc * 31 + ops[i](a, b);
    }
    line("ops-acc       ", acc);
    line("udiv          ", a / b);
    line("umod          ", a % b);
    puts8("sdiv          ");
    putdec(sdiv(-1000000007LL, 97));
    puts8("\r\n");
    line("udiv32        ", udiv32(4000000000U, 7U));
    line("fib(20)       ", fib(20));
    line("depth(200)    ", depth(200, 1));

    status = st->BootServices->AllocatePool(EfiLoaderData, 4096, (void **)&pool);
    line("alloc-status  ", status);
    if (status == EFI_SUCCESS) {
        for (i = 0; i < 4096; i++) {
            pool[i] = (UINT8)(i * 7 + 3);
        }
        line("pool-hash     ", crc_like(pool, 4096));
        st->BootServices->FreePool(pool);
    }

    status = st->BootServices->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_version);
    line("memmap-probe  ", status == EFI_BUFFER_TOO_SMALL ? 1 : 0);

    puts8("IA64-VMM: done\r\n");
    return EFI_SUCCESS;
}

/* crt0-efi-ia64.S calls _entry after self-relocation. */
EFI_STATUS EFIAPI _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    return efi_main(image, st);
}
