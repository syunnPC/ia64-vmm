/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .text
.global amd64_boot_read64, amd64_boot_read64_fault
.type amd64_boot_read64,@function
amd64_boot_read64:
    mov (%rdi), %rax
    mov %rax, (%rsi)
    mov $1, %eax
    ret
amd64_boot_read64_fault:
    xor %eax, %eax
    ret
.size amd64_boot_read64, .-amd64_boot_read64
.section .note.GNU-stack,"",@progbits
