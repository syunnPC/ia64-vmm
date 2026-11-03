/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef uint64_t UINT64;
typedef uint8_t UINT8;
typedef size_t UINTN;
typedef int BOOLEAN;
typedef void VOID;
#define TRUE 1
#define FALSE 0
#define EfiMemoryMappedIO 11
#define EfiMemoryMappedIOPortSpace 12
typedef struct {
    unsigned Type;
    UINT64 PhysicalStart, NumberOfPages;
} EFI_MEMORY_DESCRIPTOR;
#include "firmware/new/gate/runtime-alias.h"
int main(void)
{
    EFI_MEMORY_DESCRIPTOR map[] = {{11, 0xa0000, 32},
                                   {11, 0xc0000000, 256},
                                   {11, 0xf0000000, 4096},
                                   {12, 0x3fffffc000000, 16384}};
    UINTN size = sizeof(map), stride = sizeof(map[0]);
    assert(!alias_safe(0, 1ULL << 30, map, size, stride));
    assert(!alias_safe(3ULL << 30, 1ULL << 30, map, size, stride));
    assert(alias_safe(2ULL << 30, 1ULL << 30, map, size, stride));
    assert(alias_shift(2ULL << 30, map, size, stride) == 30);
    UINTN shift = alias_shift(0x900000, map, size, stride);
    assert(shift && shift < 30 &&
           alias_safe(0x900000 & ~((1ULL << shift) - 1), 1ULL << shift, map, size, stride));
    assert(alias_shift(0xa0000, map, size, stride) == 0);
    assert(alias_shift(0xc0000000, map, size, stride) == 0);
    assert(alias_shift(0xf0000000, map, size, stride) == 0);
    assert(alias_shift(0x3fffffc000000, map, size, stride) == 0);
    /* Exact adjacent edges do not intersect, including a PCI range ending
     * at a chunk boundary and a framebuffer beginning at the next one. */
    map[0] = (EFI_MEMORY_DESCRIPTOR){11, (1ULL << 30) - 4096, 1};
    assert(alias_safe(1ULL << 30, 1ULL << 30, map, size, stride));
    map[0] = (EFI_MEMORY_DESCRIPTOR){11, 2ULL << 30, 1};
    assert(alias_safe(1ULL << 30, 1ULL << 30, map, size, stride));
    return 0;
}
