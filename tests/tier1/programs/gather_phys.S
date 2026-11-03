/* SPDX-License-Identifier: GPL-2.0-or-later */
// gather-phys: the gather loop in physical mode; the harness also runs
// it with PSR.ss = 1 (lower-tier-step) and at its bit-63 alias via gather_alias.
        .set GUEST_ROT, 0
        .set GUEST_ROTP, 0
        .set GUEST_LOAD_AFTER_STORE, 0
        .set GUEST_ALIAS, 0
        .set GUEST_2PAGE, 0
        .include "gather.inc"
