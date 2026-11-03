/* SPDX-License-Identifier: GPL-2.0-or-later */
// Reproducer (seed 44); run: python3 tests/fuzz/fuzz.py --run tests/fuzz/repro/state-mov-to-ar.bspstore-seed44.S
// category: state
// where:    state before bundle #3 at 0x100030
// culprit bundle 0x100020: mov.m ar.bspstore=r7
//   ar.bsp       interp=0x0000000006e001f8  fork=0x0000000006e00200
// ia64 differential fuzz program, seed 44
.text
.global _start
_start:
    mov ar.rsc=0 ;;
    movl r7=0x6e001f8 ;;
    mov ar.bspstore=r7 ;;
    movl r17=0x4aae21086a555dfe ;;
    mov ar.rnat=r17 ;;
    mov r7=ar.bsp ;;
    mov r17=ar.rnat ;;
    extr.u r17=r17,0,64 ;;
    mov ar.rsc=2 ;;
fuzz_checkpoint:
    movl r31=0x110000 ;;
    ld8 r1=[r31],8 ;;
    ld8 r2=[r31],8 ;;
    ld8 r3=[r31],8 ;;
    ld8 r4=[r31],8 ;;
    ld8 r5=[r31],8 ;;
    ld8 r6=[r31],8 ;;
    ld8 r7=[r31],8 ;;
    ld8 r8=[r31],8 ;;
    ld8 r9=[r31],8 ;;
    ld8 r10=[r31],8 ;;
    ld8 r11=[r31],8 ;;
    ld8 r12=[r31],8 ;;
    ld8 r13=[r31],8 ;;
    ld8 r14=[r31],8 ;;
    ld8 r15=[r31],8 ;;
    ld8 r16=[r31],8 ;;
    ld8 r17=[r31],8 ;;
    ld8 r18=[r31],8 ;;
    ld8 r19=[r31],8 ;;
    ld8 r20=[r31],8 ;;
    ld8 r21=[r31],8 ;;
    ld8 r22=[r31],8 ;;
    ld8 r23=[r31],8 ;;
    ld8 r24=[r31],8 ;;
    ld8 r25=[r31],8 ;;
    ld8 r26=[r31],8 ;;
    ld8 r27=[r31],8 ;;
    ld8 r28=[r31],8 ;;
    ld8 r29=[r31],8 ;;
    ld8 r30=[r31],8 ;;
    ld8 r1=[r31],8 ;;
    ld8 r2=[r31],8 ;;
    ld8 r3=[r31],8 ;;
    ld8 r4=[r31],8 ;;
    ld8 r5=[r31],8 ;;
    ld8 r6=[r31],8 ;;
    ld8 r7=[r31],8 ;;
    ld8 r8=[r31],8 ;;
    ld8 r9=[r31],8 ;;
    ld8 r10=[r31],8 ;;
    ld8 r11=[r31],8 ;;
    ld8 r12=[r31],8 ;;
    ld8 r13=[r31],8 ;;
    ld8 r14=[r31],8 ;;
    ld8 r15=[r31],8 ;;
    ld8 r16=[r31],8 ;;
    ld8 r17=[r31],8 ;;
    ld8 r18=[r31],8 ;;
    ld8 r19=[r31],8 ;;
    ld8 r20=[r31],8 ;;
    ld8 r21=[r31],8 ;;
    ld8 r22=[r31],8 ;;
    ld8 r23=[r31],8 ;;
    ld8 r24=[r31],8 ;;
    ld8 r25=[r31],8 ;;
    ld8 r26=[r31],8 ;;
    ld8 r27=[r31],8 ;;
    ld8 r28=[r31],8 ;;
    ld8 r29=[r31],8 ;;
    ld8 r30=[r31],8 ;;
    ld8 r1=[r31],8 ;;
    ld8 r2=[r31],8 ;;
    ld8 r3=[r31],8 ;;
    ld8 r4=[r31],8 ;;
    break.i 0x1ffffe ;;
.data
fuzz_scratch:  // linked at SCRATCH
    data8 0x80, 0xd3d785e6cdc5d0b0, 0x7fff7fff5fff7fff, 0x7fff7fff7fff7fff
    data8 0xde4d094d12786c54, 0x3e, 0xffffffff80000000, 0xaf30f672946522a7
    data8 0xffffffffffffffd6, 0xe10153c389a0c6c0, 0x3, 0x45d38c53ebb662aa
    data8 0x5366fef1e3f051a2, 0x92a563d3eb754504, 0xffff0000fbff0000, 0xffff8000
    data8 0xe, 0xc11081486f235b18, 0xffff, 0x67e663bf9ecac002
    data8 0x410, 0xe9a9ac44676779c9, 0x40, 0x8000800080008000
    data8 0x1a, 0x7fffffff, 0xffff8000, 0xe4d28a63e96c643
    data8 0x7fff7fff, 0x1, 0x909e7a6b10c2a80e, 0x20
    data8 0x2aa0a95b3f411e82, 0x1f, 0x3943f96550282026, 0x4d0d05fda0c3414e
    data8 0x7fffffff7fffffff, 0x8, 0xffffffffffffffc6, 0x5555555555555555
    data8 0x14, 0x101010101010101, 0x100000000, 0x8000
    data8 0xdf855267e811a29c, 0x20, 0x5555555555555555, 0x123456789abcdef
    data8 0x7fffffff7fffffff, 0x3b, 0xff00ff00ff00ff00, 0x80000000
    data8 0x94c55361c06cc65f, 0x8080808280808080, 0x7fff7fff7fff7fff, 0xfffe75c6990925c6
    data8 0x7f, 0x994d1aecc0d34673, 0x32, 0x3c
    data8 0xe7bdc08484994bb3, 0x123456789abcdef, 0x1cbaa79fcd6f86e4, 0x5
