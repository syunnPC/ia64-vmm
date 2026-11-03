/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.3 brp B6/B7: no predicate, no architectural side effects. */
.text
.global _start
_start:
    ssm psr.ic ;;
    srlz.d ;;
    brp.sptk .Ldone, .Ldone ;;
    brp.loop.imp .Ldone, .Ldone ;;
    brp.exit .Ldone, .Ldone ;;
    brp.dptk.imp .Ldone, .Ldone ;;
    brp.sptk b0, .Ldone ;;
    brp.ret.dptk.imp b7, .Ldone ;;
.Ldone:
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
