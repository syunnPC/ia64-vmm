/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the actual mapper without loading CR3 or executing privileged
 * instructions. The test uses identity addresses for its page-table pool. */
#include "host/amd64/memory.c"
#include <stdio.h>
#include <stdlib.h>

bool amd64_fb_wc;
IA64VMM_NORETURN void hal_panic(const char *message)
{
    fprintf(stderr, "mapper panic: %s\n", message);
    abort();
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "PAT mapping failed at line %d\n", __LINE__);                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void)
{
    const uint64_t wc = PTE_W | PTE_NX | PTE_PCD | PTE_PWT | PTE_PAT_SELECT;
    const uint64_t va = UINT64_C(0xffff800040000000);
    const uint64_t pa = UINT64_C(0x40000000);
    new_table();
    /* Enable 1 GiB leaves explicitly; map_range normally learns CPUID. */
    pages_1g = true;
    map_range(va, pa, UINT64_C(1) << 30, wc, true);
    uint64_t entry = *leaf(va, 3);
    CHECK(entry == (pa | PTE_P | PTE_PS | PTE_PAT_LARGE | PTE_PCD | PTE_PWT | PTE_W | PTE_NX));
    entry = *leaf(va + (UINT64_C(3) << 21), 2);
    CHECK(entry == ((pa + (UINT64_C(3) << 21)) | PTE_P | PTE_PS | PTE_PAT_LARGE | PTE_PCD |
                    PTE_PWT | PTE_W | PTE_NX));
    entry = *leaf(va + (UINT64_C(3) << 21) + PAGE, 1);
    CHECK(entry == ((pa + (UINT64_C(3) << 21) + PAGE) | PTE_P | PTE_PAT_4K | PTE_PCD | PTE_PWT |
                    PTE_W | PTE_NX));
    entry = *leaf(va + (UINT64_C(3) << 21) + PAGE * 2, 1);
    CHECK((entry & PTE_ADDR) == pa + (UINT64_C(3) << 21) + PAGE * 2);
    CHECK(entry & PTE_PAT_4K);
    /* Remapping over split leaves must keep the appropriate PAT selector. */
    map_range(va, pa, UINT64_C(1) << 30, wc, true);
    CHECK(*leaf(va + PAGE * 2, 1) ==
          ((pa + PAGE * 2) | PTE_P | PTE_PAT_4K | PTE_PCD | PTE_PWT | PTE_W | PTE_NX));
    /* UC- fallback uses PAT[2]; an adjacent physical address bit 12 must
     * survive splitting a WB leaf too. */
    map_range(va, pa, UINT64_C(1) << 30, PTE_W | PTE_NX | PTE_PCD, true);
    entry = *leaf(va + PAGE, 1);
    CHECK(entry == ((pa + PAGE) | PTE_P | PTE_PCD | PTE_W | PTE_NX));
    map_page(va + (UINT64_C(1) << 30), pa + (UINT64_C(1) << 30), 3, PTE_W | PTE_NX);
    entry = *leaf(va + (UINT64_C(1) << 30) + PAGE, 1);
    CHECK(entry == ((pa + (UINT64_C(1) << 30) + PAGE) | PTE_P | PTE_W | PTE_NX));
    puts("PAT mapping: WC 1GiB/2MiB/4KiB, split/remap, UC- fallback and WB passed");
    return 0;
}
