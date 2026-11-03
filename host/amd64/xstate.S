/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * void amd64_xstate_switch(void *save, const void *load)
 *
 * XSAVE every state component enabled in XCR0 to save, then XRSTOR them
 * from load (Intel SDM Vol.1 §13.7, §13.8.1: standard form, both areas
 * 64-byte aligned, amd64_xstate_bytes long). Callable from assembly between a
 * native guest's state and the host's: clobbers RAX, RCX and RDX only,
 * leaves RFLAGS, the stack and every other register unchanged.
 */
.section .text
.global amd64_xstate_switch
.type amd64_xstate_switch,@function
amd64_xstate_switch:
    mov $0, %ecx
    xgetbv
    xsave (%rdi)
    xrstor (%rsi)
    ret
.size amd64_xstate_switch, .-amd64_xstate_switch
.section .note.GNU-stack,"",@progbits
