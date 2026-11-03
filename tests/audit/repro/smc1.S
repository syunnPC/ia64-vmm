/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r20 = 2
// EXPECT r21 = 4
.text
.global _start
_start:
  movl r30 = target + 8
  movl r29 = repl + 8
  ;;
  ld8 r31 = [r29]
  ;;
  br.sptk target
  ;;
.align 16
target:
{ .mmi
  st8 [r30] = r31
  nop.m 0
  mov r20 = 1
}
{ .mmi
  nop.m 0
  nop.m 0
  nop.i 0 ;;
}
  // second test: modify a bundle on another page, already decoded (executed once)
  movl r28 = other
  movl r27 = repl2
  ;;
  ld8 r26 = [r27], 8
  ;;
  ld8 r23 = [r27]
  br.sptk other_call_wrap
  ;;
.align 16
done:
  break.i 0x1ffffe
  ;;
.align 16
repl:
{ .mmi
  st8 [r30] = r31
  nop.m 0
  mov r20 = 2
}
.align 16
repl2:
{ .mii
  nop.m 0
  mov r21 = 4
  nop.i 0 ;;
}
.align 16
other_call_wrap:
  br.sptk other_first
  ;;
.org 0x2000
other_first:
  // execute 'other' once so it is decoded, then patch and re-run
  mov r21 = 0
  movl r25 = 0
  ;;
  br.sptk other
  ;;
.align 16
other:
{ .mii
  nop.m 0
  mov r21 = 3
  nop.i 0 ;;
}
patchit:
  cmp.eq p6, p0 = 1, r25
  ;;
  (p6) br.sptk done
  movl r24 = other
  mov r25 = 1
  ;;
  st8 [r24] = r26, 8
  ;;
  st8 [r24] = r23
  ;;
  br.sptk other
  ;;
