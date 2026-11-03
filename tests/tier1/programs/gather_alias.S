/* SPDX-License-Identifier: GPL-2.0-or-later */
// gather-alias: mov ip in the body; the harness
// maps the code page through two ITRs (VRN 0 and 4) and the output through
// VRN 3 and 5, and also enters the image at the bit-63 physical alias.
        .set GUEST_ROT, 0
        .set GUEST_ROTP, 0
        .set GUEST_LOAD_AFTER_STORE, 0
        .set GUEST_ALIAS, 1
        .set GUEST_2PAGE, 0
        .include "gather.inc"
