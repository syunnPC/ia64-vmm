/* SPDX-License-Identifier: GPL-2.0-or-later */
// gather-virt: the gather loop under PSR.it/dt/rt = 1; the harness maps
// code/IVT and the index array by TRs and leaves the table to the VHPT walker
// and the IVT's Data TLB handler, the output array to the Alternate Data TLB
// and Dirty-Bit handlers (tests/tier1/programs.c).
        .set GUEST_ROT, 0
        .set GUEST_ROTP, 0
        .set GUEST_LOAD_AFTER_STORE, 0
        .set GUEST_ALIAS, 0
        .set GUEST_2PAGE, 0
        .include "gather.inc"
