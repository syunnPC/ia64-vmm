/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Four independently entered bundles; model-tests.h checks before effects. */
.explicit
.text
.global _start
_start:
{ .mii
    ld16 r8,ar.csd=[r10]
    nop.i 0
    nop.i 0
};;
{ .mii
    st16 [r10]=r9,ar.csd
    nop.i 0
    nop.i 0
};;
{ .mii
    cmp8xchg16.acq r8=[r10],r9,ar.csd,ar.ccv
    nop.i 0
    nop.i 0
};;
{ .mib
    nop.m 0
    nop.i 0
    vmsw.0
};;
{ .mib
    nop.m 0
    nop.i 0
    vmsw.1
};;
/* B4 br.ia b1; GNU binutils does not accept the mnemonic. */
.quad 0x100000011, 0x80001020000200
{ .mib
    nop.m 0
    nop.i 0
    rfi
};;
/* A hot Tier1 prefix whose fall-through is a model boundary. */
{ .mii
    nop.m 0
    adds r8=1,r8
    nop.i 0
};;
{ .mii
    ld16 r8,ar.csd=[r10]
    nop.i 0
    nop.i 0
};;
