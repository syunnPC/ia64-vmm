/* SPDX-License-Identifier: GPL-2.0-or-later */
// gather-2page: the loop body straddles the 4 KiB code boundary at
// 0x101000 (two code dependencies).
        .set GUEST_ROT, 0
        .set GUEST_ROTP, 0
        .set GUEST_LOAD_AFTER_STORE, 0
        .set GUEST_ALIAS, 0
        .set GUEST_2PAGE, 1
        .include "gather.inc"
