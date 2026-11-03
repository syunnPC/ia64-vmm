/* SPDX-License-Identifier: GPL-2.0-or-later */
// Reproducer (seed 41); run: python3 tests/fuzz/fuzz.py --run tests/fuzz/repro/terminal-mov-to-ar.pfs-seed41.S
// category: terminal
// where:    bundles interp 2 fork 36; interp {'kind': 'fault', 'fault': 'Reserved Register/Field', 'ip': '0x100010', 'slot': 1, 'info': 9331458467776744091} / fork {'kind': 'end', 'ip': '0x100230', 'slot': 1}
// culprit bundle 0x100010: mov.i ar.pfs=r20
// ia64 differential fuzz program, seed 41
.text
.global _start
_start:
    movl r20=0x818000094824ca9b ;;
    mov ar.pfs=r20 ;;
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
    data8 0x17e00d4644c52adb, 0x40, 0xaaaaaaaaaaaaaaaa, 0xffffffffffffffce
    data8 0x15, 0x2003f, 0x7fffffff, 0x6015ce192e7e7fbc
    data8 0xffff, 0x3b, 0xffffffffffffffff, 0xffff
    data8 0xac0f3a3835b4b8b1, 0x123456789abcdef, 0x8080808080808080, 0x5
    data8 0x1, 0xb10e627bf06bf309, 0x7fff, 0x5555555555555555
    data8 0x5555555555555555, 0xfffffffffffffffa, 0x1, 0x3
    data8 0x43222d2a8811f27e, 0xffffffff80000000, 0xffff8000, 0x7fffbfffffffffff
    data8 0x7fff7fff7fff7fff, 0x40, 0x7fffffff, 0xffff
    data8 0x2fe4af83c76a5598, 0x8000000000000001, 0xb45ea1560ea16ff8, 0x5fb051c9798814b4
    data8 0xbbb6524fc02b9a99, 0x8, 0xffff, 0x80
    data8 0xaaaaaaaaaaaaaaaa, 0x5b087f7571939008, 0xff00ff00ff00ff00, 0x7fffffff
    data8 0xf, 0xffffffff80000000, 0x1f, 0xff00ff00ff00ff
    data8 0xf, 0xffff, 0xf137e39e87514cad, 0xffffffffffffffca
    data8 0x2d64fc2e80a9f9f9, 0x140e0bd88ee920e2, 0xaf974d14f5c0dab6, 0xffffff7f
    data8 0x3f, 0xfedcba9876543210, 0x3f, 0x14
    data8 0x6b4e29943f204f3d, 0x20, 0x123456789abc5ef, 0x35
