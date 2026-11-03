/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "firmware/new/dxe/fvb.c"
int main(void)
{
    UINT8 bytes[IA64VMM_NVRAM_SIZE], input[16], output[16];
    UINT64 updates = 99;
    memset(bytes, 0xff, sizeof(bytes));
    memset(input, 0xa5, sizeof(input));
    storage = bytes;
    commit_register = &updates;
    UINTN size, count;
    assert(block(&fvb, 15, &size, &count) == EFI_SUCCESS && size == 4096 && count == 1);
    assert(block(&fvb, 16, &size, &count) == EFI_INVALID_PARAMETER);
    size = 16;
    assert(write_block(&fvb, 0, 4090, &size, input) == EFI_BAD_BUFFER_SIZE && size == 6 &&
           updates == 0);
    size = 6;
    assert(read_block(&fvb, 0, 4090, &size, output) == EFI_SUCCESS && !memcmp(input, output, 6));
    input[0] = 0xff;
    size = 6;
    assert(write_block(&fvb, 0, 4090, &size, input) == EFI_DEVICE_ERROR && bytes[4090] == 0xa5);
    assert(erase(&fvb, (EFI_LBA)0, (UINTN)1, (EFI_LBA)16, (UINTN)1, EFI_LBA_LIST_TERMINATOR) ==
           EFI_INVALID_PARAMETER);
    assert(bytes[4090] == 0xa5);
    assert(erase(&fvb, (EFI_LBA)0, (UINTN)1, EFI_LBA_LIST_TERMINATOR) == EFI_SUCCESS);
    assert(bytes[4090] == 0xff);
    size = 1;
    assert(write_block(&fvb, 16, 0, &size, input) == EFI_INVALID_PARAMETER);
    size = 1;
    assert(read_block(&fvb, 0, 4096, &size, output) == EFI_INVALID_PARAMETER);
    size = 1;
    assert(read_block(&fvb, 0, 0, &size, NULL) == EFI_INVALID_PARAMETER);
    puts("FVB: boundaries, partial transfer, NOR transitions, atomic erase-list validation passed");
    return 0;
}
