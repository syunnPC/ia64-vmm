/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .ivt,"ax"
.org 0x1800
    { .mii
      nop.m 0
      nop.i 0
      nop.i 0 ;;
    }
    break.m 0x1ffffe ;;
.org 0x1c00
    { .mii
      nop.m 0
      nop.i 0
      nop.i 0 ;;
    }
    break.m 0x1ffffe ;;
.org 0x2000
    { .mii
      nop.m 0
      nop.i 0
      nop.i 0 ;;
    }
    break.m 0x1ffffe ;;
.org 0x2400
    { .mii
      nop.m 0
      nop.i 0
      nop.i 0 ;;
    }
    break.m 0x1ffffe ;;
.org 0x2800
    { .mii
      nop.m 0
      nop.i 0
      nop.i 0 ;;
    }
    break.m 0x1ffffe ;;
.org 0x2c00
    { .mii
      nop.m 0
      nop.i 0
      nop.i 0 ;;
    }
    break.m 0x1ffffe ;;
