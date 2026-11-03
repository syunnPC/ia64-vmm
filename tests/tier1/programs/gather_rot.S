/* SPDX-License-Identifier: GPL-2.0-or-later */
// gather-rot: sor = 8 and one br.ctop before the loop, so
// CFM.rrb.gr != 0 at loop entry (static rotating-GR renaming).
        .set GUEST_ROT, 1
        .set GUEST_ROTP, 0
        .set GUEST_LOAD_AFTER_STORE, 0
        .set GUEST_ALIAS, 0
        .set GUEST_2PAGE, 0
        .include "gather.inc"
