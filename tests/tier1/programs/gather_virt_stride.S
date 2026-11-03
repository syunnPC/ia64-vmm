/* SPDX-License-Identifier: GPL-2.0-or-later */
// gather-virt-stride: gather-virt with a table stride of 4 KiB, so each
// S2 occurrence touches its own page (the data layout is the harness's).
        .set GUEST_ROT, 0
        .set GUEST_ROTP, 0
        .set GUEST_LOAD_AFTER_STORE, 0
        .set GUEST_ALIAS, 0
        .set GUEST_2PAGE, 0
        .include "gather.inc"
