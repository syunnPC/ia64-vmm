/* SPDX-License-Identifier: GPL-2.0-or-later */
// gather-rotp: gather-rot with p16-p20 for p6-p10; after the br.ctop
// CFM.rrb.pr = 47, so logical p16-p20 are physical p63, p16-p19 (the wrap).
        .set GUEST_ROT, 1
        .set GUEST_ROTP, 1
        .set GUEST_LOAD_AFTER_STORE, 0
        .set GUEST_ALIAS, 0
        .set GUEST_2PAGE, 0
        .include "gather.inc"
