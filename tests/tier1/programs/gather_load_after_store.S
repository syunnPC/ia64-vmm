/* SPDX-License-Identifier: GPL-2.0-or-later */
// gather-load-after-store: S4, ld8 r19 = [r38], after S3 in the same iteration;
// r38 points to a status word that is never stored to.
        .set GUEST_ROT, 0
        .set GUEST_ROTP, 0
        .set GUEST_LOAD_AFTER_STORE, 1
        .set GUEST_ALIAS, 0
        .set GUEST_2PAGE, 0
        .include "gather.inc"
