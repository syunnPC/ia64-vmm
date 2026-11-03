/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r2 = 0x30
.text
.explicit
.global _start
_start:
    { .mii
      nop.m 0; mov r2=48; nop.i 0 }
    ;;
    { .mmi
      ptc.g r3,r2; nop.m 0; nop.i 0 }
    ;;
    { .mmi
      ptc.ga r3,r2; nop.m 0; nop.i 0 }
    ;;
    { .mii
      nop.m 0; break.i 0x1ffffe; nop.i 0 }
